/*
  ESP32 IoT - BLE配网 + 串口触发OTA
  =====================================
  功能：
  1. BLE配网：扫描WiFi、连接WiFi（浏览器/手机操作）
  2. 串口输入"1" → 触发OTA，从内置URL下载固件并更新
  3. OTA使用原生 HTTPUpdate 库，URL硬编码在代码里

  开发阶段用串口触发OTA，稳定后再加MQTT远程触发。
*/

#include <WiFi.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

/* =========================
   OTA固件URL（改这里）
========================= */
#define OTA_URL "https://pub-29ba3e6b1a064dc19cd79398bac206eb.r2.dev/firmware.bin"

/* =========================
   BLE UUID
========================= */
#define SERVICE_UUID    "5a67d678-6361-4f32-8396-54c6926c8fa9"
#define WIFI_SSID_UUID  "5a67d678-6361-4f32-8396-54c6926c8f03"
#define WIFI_PASS_UUID  "5a67d678-6361-4f32-8396-54c6926c8f04"
#define APPLY_WIFI_UUID "5a67d678-6361-4f32-8396-54c6926c8f05"
#define STATUS_UUID     "5a67d678-6361-4f32-8396-54c6926c8f06"
#define SCAN_UUID       "5a67d678-6361-4f32-8396-54c6926c8f01"
#define LIST_UUID       "5a67d678-6361-4f32-8396-54c6926c8f02"

/* =========================
   全局对象
========================= */
Preferences prefs;
WiFiClient  wifiClient;
WiFiClientSecure wifiClientSecure;

/* BLE特征 */
BLECharacteristic *ssidChar;
BLECharacteristic *passChar;
BLECharacteristic *applyChar;
BLECharacteristic *statusChar;
BLECharacteristic *scanChar;
BLECharacteristic *listChar;

/* =========================
   标志位
========================= */
volatile bool flagScan        = false;
volatile bool flagConnectWiFi = false;

/* WiFi凭据 */
String wifiSSID = "";
String wifiPASS = "";

/* =========================
   状态通知（BLE + 串口）
========================= */
void notify(String msg) {
  if (statusChar) {
    statusChar->setValue(msg.c_str());
    statusChar->notify();
  }
  Serial.println("[ESP32] " + msg);
}

/* =========================
   WiFi扫描
========================= */
void runScan() {
  notify("scanning");
  WiFi.mode(WIFI_STA);

  int n   = WiFi.scanNetworks();
  int cap = (n > 10) ? 10 : n;

  String json = "[";
  for (int i = 0; i < cap; i++) {
    if (i) json += ",";
    String s = WiFi.SSID(i);
    s.replace("\"", "\\\"");
    json += "{\"s\":\"" + s + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";

  listChar->setValue(json.c_str());
  listChar->notify();
  notify("scan_done");
}

/* =========================
   WiFi连接
========================= */
void connectWiFi() {
  notify("wifi_connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    notify("wifi_ok: " + WiFi.localIP().toString());
    prefs.begin("wifi", false);
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPASS);
    prefs.end();
  } else {
    notify("wifi_failed");
  }
}

/* =========================
   OTA更新
   - 先打印固件头部字节（确认文件合法）
   - 再用HTTPUpdate烧录
========================= */
void doOTA() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] WiFi未连接，无法OTA");
    return;
  }

  String url = OTA_URL;
  Serial.println("[OTA] 开始下载: " + url);

  /* ---- 步骤1：先用HTTPClient取前16字节验证固件头 ---- */
  {
    WiFiClientSecure check;
    check.setInsecure();
    HTTPClient http;
    http.begin(check, url);
    int code = http.GET();
    Serial.println("[OTA] HTTP状态码: " + String(code));

    if (code == 200) {
      WiFiClient* stream = http.getStreamPtr();
      uint8_t buf[16] = {0};
      int got = stream->readBytes(buf, 16);
      Serial.print("[OTA] 固件头(hex): ");
      for (int i = 0; i < got; i++) Serial.printf("%02X ", buf[i]);
      Serial.println();

      if (buf[0] != 0xE9) {
        Serial.println("[OTA] ❌ 固件头不合法（第一字节应为0xE9），取消OTA");
        http.end();
        return;
      }
      Serial.println("[OTA] ✅ 固件头合法，开始烧录...");
    } else {
      Serial.println("[OTA] ❌ HTTP请求失败，取消OTA");
      http.end();
      return;
    }
    http.end();
  }

  /* ---- 步骤2：正式OTA烧录 ---- */
  // 进度回调
  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("[OTA] 进度: %d / %d bytes (%.1f%%)\n",
                  cur, total, total > 0 ? (cur * 100.0 / total) : 0);
  });

  httpUpdate.rebootOnUpdate(true);

  WiFiClientSecure ota;
  ota.setInsecure();

  t_httpUpdate_return ret = httpUpdate.update(ota, url);

  switch (ret) {
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] ✅ 成功，正在重启...");
      break;
    case HTTP_UPDATE_FAILED:
      Serial.println("[OTA] ❌ 失败: " + String(httpUpdate.getLastErrorString())
                     + " code=" + String(httpUpdate.getLastError()));
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] ℹ️ 无需更新");
      break;
  }
}

/* =========================
   BLE回调
========================= */
class BLECB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) {
    String uuid = c->getUUID().toString().c_str();
    String val  = c->getValue().c_str();
    Serial.println("[BLE RX] " + uuid + " = " + val);

    if (uuid == WIFI_SSID_UUID)  wifiSSID = val;
    if (uuid == WIFI_PASS_UUID)  wifiPASS = val;

    if (uuid == APPLY_WIFI_UUID && val == "1") flagConnectWiFi = true;
    if (uuid == SCAN_UUID       && val == "1") flagScan        = true;
  }
};

/* =========================
   BLE初始化
========================= */
void setupBLE() {
  BLEDevice::init("ESP32-Setup");
  BLEServer  *server  = BLEDevice::createServer();
  BLEService *service = server->createService(BLEUUID(SERVICE_UUID), 32);

  ssidChar  = service->createCharacteristic(WIFI_SSID_UUID,  BLECharacteristic::PROPERTY_WRITE);
  passChar  = service->createCharacteristic(WIFI_PASS_UUID,  BLECharacteristic::PROPERTY_WRITE);
  applyChar = service->createCharacteristic(APPLY_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  statusChar= service->createCharacteristic(STATUS_UUID,     BLECharacteristic::PROPERTY_NOTIFY);
  scanChar  = service->createCharacteristic(SCAN_UUID,       BLECharacteristic::PROPERTY_WRITE);
  listChar  = service->createCharacteristic(LIST_UUID,       BLECharacteristic::PROPERTY_NOTIFY);

  ssidChar ->setCallbacks(new BLECB());
  passChar ->setCallbacks(new BLECB());
  applyChar->setCallbacks(new BLECB());
  scanChar ->setCallbacks(new BLECB());

  statusChar->addDescriptor(new BLE2902());
  listChar  ->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  Serial.println("[BLE] READY - 设备名: ESP32-Setup");
}

/* =========================
   SETUP
========================= */
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========== ESP32 启动 ==========");
  Serial.println("串口命令：输入 1 触发OTA更新");
  Serial.println("================================\n");

  // 加载保存的WiFi配置
  prefs.begin("wifi", true);
  wifiSSID = prefs.getString("ssid", "");
  wifiPASS = prefs.getString("pass", "");
  prefs.end();

  setupBLE();

  // 有保存的WiFi则自动连接
  if (wifiSSID.length() > 0) {
    Serial.println("[AUTO] 自动连接WiFi: " + wifiSSID);
    connectWiFi();
  } else {
    Serial.println("[AUTO] 无保存WiFi，请用BLE配网");
  }
}

/* =========================
   LOOP
========================= */
void loop() {
  // 处理串口输入
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input == "1") {
      Serial.println("[串口] 收到OTA指令");
      doOTA();
    } else {
      Serial.println("[串口] 未知指令: " + input + "（输入1触发OTA）");
    }
  }

  // 处理BLE标志位
  if (flagScan) {
    flagScan = false;
    runScan();
  }

  if (flagConnectWiFi) {
    flagConnectWiFi = false;
    connectWiFi();
  }

  delay(10);
}
