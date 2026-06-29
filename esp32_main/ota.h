#pragma once
/*
 * ota.h — HTTP OTA 更新模块
 *
 * 流程:
 *   1. 从 GitHub Pages 的 version.json 获取最新版本号
 *   2. 与当前固件版本比较
 *   3. 如果有新版本，从 GitHub Releases 下载 .bin 并刷入
 *
 * 使用方法:
 *   #include "ota.h"
 *   OTA ota("https://你的用户名.github.io/你的仓库名");
 *   ota.check();   // 检查并更新（阻塞直到完成或失败）
 */

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// ── 当前固件版本（每次发布前手动 +1，或由 CI 自动注入）────────
#define FIRMWARE_VERSION "1.0.0"

// OTA 进度回调类型
typedef void (*OtaProgressCb)(int percent);
typedef void (*OtaLogCb)(const String& msg);

class OTA {
public:
  // baseUrl: GitHub Pages 根目录，例如
  //   "https://yourname.github.io/yourrepo"
  OTA(const String& baseUrl,
      OtaLogCb      logCb      = nullptr,
      OtaProgressCb progressCb = nullptr)
    : _baseUrl(baseUrl), _logCb(logCb), _progressCb(progressCb) {}

  // 检查版本，有新版则自动下载刷入，返回是否触发了更新
  bool check() {
    log("OTA: 当前版本 " + String(FIRMWARE_VERSION));
    log("OTA: 检查 " + _baseUrl + "/version.json");

    String latestVer, binUrl;
    if (!fetchVersionInfo(latestVer, binUrl)) {
      log("OTA: 获取版本信息失败");
      return false;
    }

    log("OTA: 最新版本 " + latestVer);

    if (!isNewer(latestVer, FIRMWARE_VERSION)) {
      log("OTA: 已是最新版本，无需更新");
      return false;
    }

    log("OTA: 发现新版本！开始下载...");
    log("OTA: " + binUrl);

    if (downloadAndFlash(binUrl)) {
      log("OTA: 更新成功，3 秒后重启...");
      delay(3000);
      ESP.restart();
      return true;  // 不会执行到这里
    } else {
      log("OTA: 更新失败");
      return false;
    }
  }

  // 强制更新（跳过版本比较，直接从指定 URL 刷入）
  bool forceUpdate(const String& binUrl) {
    log("OTA: 强制更新 " + binUrl);
    if (downloadAndFlash(binUrl)) {
      log("OTA: 更新成功，3 秒后重启...");
      delay(3000);
      ESP.restart();
      return true;
    }
    return false;
  }

private:
  String        _baseUrl;
  OtaLogCb      _logCb;
  OtaProgressCb _progressCb;

  void log(const String& msg) {
    Serial.println(msg);
    if (_logCb) _logCb(msg);
  }

  // ── 从 version.json 解析最新版本和固件下载地址 ────────────────
  bool fetchVersionInfo(String& version, String& binUrl) {
    HTTPClient http;
    // 加 ?t= 防止 CDN 缓存
    String url = _baseUrl + "/version.json?t=" + String(millis());
    http.begin(url);
    http.addHeader("Cache-Control", "no-cache");
    int code = http.GET();

    if (code != 200) {
      log("OTA: HTTP 错误 " + String(code));
      http.end();
      return false;
    }

    String body = http.getString();
    http.end();

    // 解析 JSON: {"version":"1.2.0","url":"https://...firmware.bin"}
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      log("OTA: JSON 解析失败 " + String(err.c_str()));
      return false;
    }

    version = doc["version"].as<String>();
    binUrl  = doc["url"].as<String>();
    return version.length() > 0 && binUrl.length() > 0;
  }

  // ── 简单语义版本比较: a > b 返回 true ────────────────────────
  bool isNewer(const String& a, const String& b) {
    int aMaj=0, aMin=0, aPat=0;
    int bMaj=0, bMin=0, bPat=0;
    sscanf(a.c_str(), "%d.%d.%d", &aMaj, &aMin, &aPat);
    sscanf(b.c_str(), "%d.%d.%d", &bMaj, &bMin, &bPat);
    if (aMaj != bMaj) return aMaj > bMaj;
    if (aMin != bMin) return aMin > bMin;
    return aPat > bPat;
  }

  // ── 下载 .bin 并通过 Update 库刷入 ───────────────────────────
  bool downloadAndFlash(const String& url) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);  // 30s 超时（固件可能较大）
    int code = http.GET();

    if (code != 200) {
      log("OTA: 下载失败，HTTP " + String(code));
      http.end();
      return false;
    }

    int totalBytes = http.getSize();
    log("OTA: 固件大小 " + String(totalBytes) + " bytes");

    if (!Update.begin(totalBytes > 0 ? totalBytes : UPDATE_SIZE_UNKNOWN)) {
      log("OTA: Update.begin 失败，分区空间不足？");
      http.end();
      return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    int lastPct = -1;

    while (http.connected() && (totalBytes <= 0 || written < totalBytes)) {
      int avail = stream->available();
      if (avail > 0) {
        int toRead = min(avail, (int)sizeof(buf));
        int n = stream->readBytes(buf, toRead);
        if (Update.write(buf, n) != (size_t)n) {
          log("OTA: 写入错误");
          http.end();
          return false;
        }
        written += n;
        if (totalBytes > 0 && _progressCb) {
          int pct = (written * 100) / totalBytes;
          if (pct != lastPct) { _progressCb(pct); lastPct = pct; }
        }
      } else {
        delay(1);
      }
    }

    http.end();

    if (!Update.end()) {
      log("OTA: Update.end 错误 " + String(Update.getError()));
      return false;
    }
    if (!Update.isFinished()) {
      log("OTA: 更新未完成");
      return false;
    }
    return true;
  }
};
