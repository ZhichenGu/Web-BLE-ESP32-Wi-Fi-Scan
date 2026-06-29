/*
 * esp32_main.ino
 * 
 * 功能:
 *   1. BLE 配网  — 通过网页蓝牙给 ESP32 配置 WiFi
 *   2. HTTP OTA  — WiFi 连上后自动检查 GitHub Pages 上的版本，有新版本则自动更新
 *
 * 需要安装的库 (Arduino Library Manager):
 *   - ArduinoJson  (by Benoit Blanchon)
 *   - ESP32 BLE Arduino (随 esp32 board package 自带)
 *   - Update       (随 esp32 board package 自带)
 *
 * !! 修改前必须做的事:
 *   1. 将下方 GITHUB_PAGES_URL 替换为你自己的 GitHub Pages 地址
 *   2. 每次发布新固件前，更新 ota.h 里的 FIRMWARE_VERSION
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <Preferences.h>  // 持久化存储 WiFi 凭证（存 NVS Flash）
#include "ota.h"

// ════════════════════════════════════════════════════════════
//  !! 修改这里 !!
// ════════════════════════════════════════════════════════════
#define GITHUB_PAGES_URL  "https://ZhichenGu.github.io/Web-BLE-ESP32-Wi-Fi-Scan"
//
// OTA 检查间隔（毫秒），默认每 6 小时检查一次
#define OTA_CHECK_INTERVAL_MS  (6UL * 60 * 60 * 1000)
// ════════════════════════════════════════════════════════════

// ── BLE UUID（与网页保持一致）───────────────────────────────
#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define SCAN_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define SSID_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PASS_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define CMD_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define STATUS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"

// ── 全局对象 ─────────────────────────────────────────────────
BLEServer*         pServer     = nullptr;
BLECharacteristic* pScanChar   = nullptr;
BLECharacteristic* pStatusChar = nullptr;
Preferences        prefs;
OTA*               ota         = nullptr;

// ── 状态标志 ─────────────────────────────────────────────────
String  targetSSID      = "";
String  targetPass      = "";
bool    doScan          = false;
bool    doConnect       = false;
bool    doOtaCheck      = false;
bool    bleConnected    = false;
bool    wifiConnected   = false;
unsigned long lastOtaCheck = 0;

// ════════════════════════════════════════════════════════════
//  BLE 回调
// ════════════════════════════════════════════════════════════
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    { bleConnected = true;  Serial.println("[BLE] 已连接"); }
  void onDisconnect(BLEServer*) { bleConnected = false; Serial.println("[BLE] 已断开，重新广播");
                                  BLEDevice::startAdvertising(); }
};

class CmdCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String cmd = c->getValue().c_str(); cmd.trim();
    if (cmd == "scan")     doScan     = true;
    if (cmd == "connect")  doConnect  = true;
    if (cmd == "ota")      doOtaCheck = true;   // 网页手动触发 OTA
    Serial.println("[CMD] " + cmd);
  }
};

class SsidCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) { targetSSID = c->getValue().c_str(); }
};

class PassCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) { targetPass = c->getValue().c_str(); }
};

// ════════════════════════════════════════════════════════════
//  BLE 推送
// ════════════════════════════════════════════════════════════
void sendStatus(const String& s) {
  Serial.println("[STATUS] " + s);
  if (bleConnected) {
    pStatusChar->setValue(s.c_str());
    pStatusChar->notify();
  }
}

// ════════════════════════════════════════════════════════════
//  WiFi 扫描
// ════════════════════════════════════════════════════════════
void doScanWifi() {
  sendStatus("scanning");
  int n = WiFi.scanNetworks();
  const int CHUNK = 480;
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + WiFi.RSSI(i)
          + ",\"enc\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    bool last = (i == n - 1);
    if ((int)json.length() > CHUNK || last) {
      if (last) json += "]";
      String chunk = (last ? "1:" : "0:") + json;
      if (bleConnected) { pScanChar->setValue(chunk.c_str()); pScanChar->notify(); delay(50); }
      if (!last) json = "[";
    }
  }
  if (n == 0) { pScanChar->setValue("1:[]"); pScanChar->notify(); }
  WiFi.scanDelete();
  sendStatus("scan_done");
}

// ════════════════════════════════════════════════════════════
//  WiFi 连接（含持久化存储）
// ════════════════════════════════════════════════════════════
bool connectWifi(const String& ssid, const String& pass, bool silent = false) {
  if (!silent) sendStatus("connecting");
  Serial.println("[WiFi] 连接: " + ssid);
  WiFi.begin(ssid.c_str(), pass.c_str());
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) { delay(500); retry++; }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    String ip = WiFi.localIP().toString();
    Serial.println("[WiFi] 连接成功 IP: " + ip);
    if (!silent) sendStatus("connected:" + ip);
    // 持久化保存凭证
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    return true;
  }
  wifiConnected = false;
  Serial.println("[WiFi] 连接失败");
  if (!silent) sendStatus("error:failed");
  WiFi.disconnect();
  return false;
}

// ════════════════════════════════════════════════════════════
//  OTA 日志回调（转发给 BLE）
// ════════════════════════════════════════════════════════════
void otaLog(const String& msg)  { sendStatus("ota_log:" + msg); }
void otaProgress(int pct)       { sendStatus("ota_pct:" + String(pct)); }

// ════════════════════════════════════════════════════════════
//  Setup
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[ESP32] 启动 v" FIRMWARE_VERSION);

  // ── 尝试用上次保存的 WiFi 自动连接 ──────────────────────
  prefs.begin("wifi", true);
  String savedSsid = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (savedSsid.length() > 0) {
    Serial.println("[WiFi] 尝试自动连接: " + savedSsid);
    connectWifi(savedSsid, savedPass, true);  // silent=true，不发 BLE 通知
  }

  // ── OTA 对象 ─────────────────────────────────────────────
  ota = new OTA(GITHUB_PAGES_URL, otaLog, otaProgress);

  // 启动后立刻检查一次（如果 WiFi 已连上）
  if (wifiConnected) {
    Serial.println("[OTA] 启动检查...");
    ota->check();
    lastOtaCheck = millis();
  }

  // ── BLE 初始化 ───────────────────────────────────────────
  BLEDevice::init("ESP32-WiFi-Setup");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  BLEService* svc = pServer->createService(SERVICE_UUID);

  auto makeChar = [&](const char* uuid, uint32_t props) {
    return svc->createCharacteristic(uuid, props);
  };

  pScanChar = makeChar(SCAN_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pScanChar->addDescriptor(new BLE2902());

  makeChar(SSID_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE)->setCallbacks(new SsidCB());
  makeChar(PASS_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE)->setCallbacks(new PassCB());
  makeChar(CMD_CHAR_UUID,  BLECharacteristic::PROPERTY_WRITE)->setCallbacks(new CmdCB());

  pStatusChar = makeChar(STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pStatusChar->addDescriptor(new BLE2902());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] 广播中...");

  // 如果已连上 WiFi，通过 BLE 状态推送 IP（等蓝牙连上后可见）
  if (wifiConnected) {
    // 存起来，等 BLE 连上后在 loop 里发
    sendStatus("connected:" + WiFi.localIP().toString());
  }
}

// ════════════════════════════════════════════════════════════
//  Loop
// ════════════════════════════════════════════════════════════
void loop() {
  // BLE 触发的 WiFi 扫描
  if (doScan) {
    doScan = false;
    doScanWifi();
  }

  // BLE 触发的 WiFi 连接
  if (doConnect) {
    doConnect = false;
    if (connectWifi(targetSSID, targetPass)) {
      // 连上 WiFi 后立即检查 OTA
      Serial.println("[OTA] WiFi 刚连上，检查更新...");
      ota->check();
      lastOtaCheck = millis();
    }
  }

  // BLE 手动触发 OTA
  if (doOtaCheck) {
    doOtaCheck = false;
    if (wifiConnected) {
      ota->check();
      lastOtaCheck = millis();
    } else {
      sendStatus("ota_log:ERROR: 请先连接 WiFi");
    }
  }

  // 定时自动 OTA 检查
  if (wifiConnected && ota &&
      millis() - lastOtaCheck > OTA_CHECK_INTERVAL_MS) {
    Serial.println("[OTA] 定时检查...");
    ota->check();
    lastOtaCheck = millis();
  }

  delay(10);
}
