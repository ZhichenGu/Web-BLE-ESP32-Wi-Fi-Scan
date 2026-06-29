#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Update.h>

/* =========================
   BLE UUID（与网页保持一致）
========================= */
#define SERVICE_UUID "5a67d678-6361-4f32-8396-54c6926c8fa9"

#define WIFI_SSID_UUID  "5a67d678-6361-4f32-8396-54c6926c8f03"
#define WIFI_PASS_UUID  "5a67d678-6361-4f32-8396-54c6926c8f04"
#define APPLY_WIFI_UUID "5a67d678-6361-4f32-8396-54c6926c8f05"

#define MQTT_CFG_UUID   "5a67d678-6361-4f32-8396-54c6926c9001"
#define STATUS_UUID     "5a67d678-6361-4f32-8396-54c6926c8f06"

#define SCAN_UUID       "5a67d678-6361-4f32-8396-54c6926c8f01"
#define LIST_UUID       "5a67d678-6361-4f32-8396-54c6926c8f02"

/* =========================
   全局对象
========================= */
Preferences prefs;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

/* =========================
   BLE 特征指针
========================= */
BLECharacteristic *ssidChar;
BLECharacteristic *passChar;
BLECharacteristic *applyChar;
BLECharacteristic *mqttChar;
BLECharacteristic *statusChar;
BLECharacteristic *scanChar;
BLECharacteristic *listChar;

/* =========================
   全局配置变量（带默认值）
========================= */
String saved_ssid = "";
String saved_pass = "";
String mqtt_broker = "broker.hivemq.com";
int mqtt_port = 1883;
String mqtt_user = "";
String mqtt_pass = "";

/* =========================
   任务标志位
========================= */
bool flag_connect_wifi = false;
bool flag_scan_wifi = false;
bool flag_ota_update = false;
String ota_url = "";

/* =========================
   状态通知
========================= */
void notify(String msg) {
  if (statusChar) {
    statusChar->setValue(msg.c_str());
    statusChar->notify();
  }
  Serial.println("[ESP32] " + msg);
}

/* =========================
   BLE 结构检查
========================= */
void printBLECheck() {
  Serial.println("\n========== BLE STRUCT CHECK ==========");
  Serial.println(ssidChar  ? "SSID_CHAR   OK" : "SSID_CHAR   MISSING");
  Serial.println(passChar  ? "PASS_CHAR   OK" : "PASS_CHAR   MISSING");
  Serial.println(applyChar ? "APPLY_CHAR  OK" : "APPLY_CHAR  MISSING");
  Serial.println(mqttChar  ? "MQTT_CHAR   OK" : "MQTT_CHAR   MISSING");
  Serial.println(statusChar? "STATUS_CHAR OK" : "STATUS_CHAR MISSING");
  Serial.println(scanChar  ? "SCAN_CHAR   OK" : "SCAN_CHAR   MISSING");
  Serial.println(listChar  ? "LIST_CHAR   OK" : "LIST_CHAR   MISSING");
  Serial.println("======================================\n");
}

/* =========================
   WiFi 扫描
========================= */
void runScan() {
  notify("scanning");
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
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
   WiFi 连接
========================= */
void connectWiFi() {
  notify("wifi_connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(300);
    yield();
  }
  if (WiFi.status() == WL_CONNECTED) {
    notify("wifi_ok");
    prefs.begin("espcfg", false);
    prefs.putString("ssid", saved_ssid);
    prefs.putString("pass", saved_pass);
    prefs.end();
  } else {
    notify("wifi_failed");
  }
}

/* =========================
   MQTT 连接
========================= */
void connectMQTT() {
  mqtt.setServer(mqtt_broker.c_str(), mqtt_port);
  mqtt.setCallback([](char* topic, byte* payload, unsigned int length) {
    String msg;
    for (int i = 0; i < length; i++) msg += (char)payload[i];
    Serial.println("[MQTT] " + msg);
    if (String(topic) == "esp32/update") {
      int key = msg.indexOf("\"url\":\"");
      if (key != -1) {
        int start = key + 7;
        int end = msg.indexOf("\"", start);
        if (end != -1) {
          ota_url = msg.substring(start, end);
          flag_ota_update = true;
        }
      }
    }
  });

  String clientId = "esp32-" + String(random(1000, 9999));
  while (!mqtt.connected()) {
    notify("MQTT connecting");
    if (mqtt.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
      mqtt.subscribe("esp32/update");
      notify("MQTT connected");
      prefs.begin("espcfg", false);
      prefs.putString("mqtt_host", mqtt_broker);
      prefs.putInt("mqtt_port", mqtt_port);
      prefs.putString("mqtt_user", mqtt_user);
      prefs.putString("mqtt_pass", mqtt_pass);
      prefs.end();
    } else {
      delay(2000);
      yield();
    }
  }
}

/* =========================
   OTA 更新（无看门狗，但使用 yield 保证系统响应）
========================= */
void performOTA(String url) {
  notify("OTA start");
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    notify("OTA http fail");
    http.end();
    return;
  }
  int contentLength = http.getSize();
  if (contentLength <= 0) {
    notify("OTA invalid size");
    http.end();
    return;
  }

  if (!Update.begin(contentLength)) {
    notify("OTA begin fail");
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buff[1024];
  while (http.connected() && written < contentLength) {
    size_t avail = stream->available();
    if (avail) {
      int len = stream->readBytes(buff, min(avail, (size_t)1024));
      Update.write(buff, len);
      written += len;
      yield(); // 每次循环都让出CPU，保证蓝牙/WiFi任务运行
    } else {
      delay(1);
      yield();
    }
  }
  http.end();

  if (written == contentLength && Update.end(true)) {
    notify("OTA success, rebooting...");
    delay(1000);
    ESP.restart();
  } else {
    notify("OTA failed");
    Update.abort();
  }
}

/* =========================
   BLE 回调
========================= */
class BLECB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) {
    String uuid = c->getUUID().toString().c_str();
    String val  = c->getValue().c_str();
    Serial.println("[BLE RX] " + uuid + " = " + val);

    if (uuid == WIFI_SSID_UUID) {
      saved_ssid = val;
      prefs.begin("espcfg", false);
      prefs.putString("ssid", saved_ssid);
      prefs.end();
    }
    if (uuid == WIFI_PASS_UUID) {
      saved_pass = val;
      prefs.begin("espcfg", false);
      prefs.putString("pass", saved_pass);
      prefs.end();
    }

    if (uuid == APPLY_WIFI_UUID && val == "1") {
      flag_connect_wifi = true;
    }

    if (uuid == SCAN_UUID && val == "1") {
      flag_scan_wifi = true;
    }

    if (uuid == MQTT_CFG_UUID) {
      int idx1 = val.indexOf('|');
      int idx2 = val.indexOf('|', idx1 + 1);
      int idx3 = val.indexOf('|', idx2 + 1);
      if (idx1 > 0 && idx2 > 0 && idx3 > 0) {
        mqtt_broker = val.substring(0, idx1);
        mqtt_port = val.substring(idx1 + 1, idx2).toInt();
        mqtt_user = val.substring(idx2 + 1, idx3);
        mqtt_pass = val.substring(idx3 + 1);
        prefs.begin("espcfg", false);
        prefs.putString("mqtt_host", mqtt_broker);
        prefs.putInt("mqtt_port", mqtt_port);
        prefs.putString("mqtt_user", mqtt_user);
        prefs.putString("mqtt_pass", mqtt_pass);
        prefs.end();
        notify("mqtt_saved");
      } else {
        notify("mqtt_format_error");
      }
    }
  }
};

/* =========================
   BLE 初始化
========================= */
void setupBLE() {
  BLEDevice::init("ESP32-Setup-2");
  BLEServer *server = BLEDevice::createServer();
  BLEService *service = server->createService(SERVICE_UUID);

  ssidChar  = service->createCharacteristic(WIFI_SSID_UUID, BLECharacteristic::PROPERTY_WRITE);
  passChar  = service->createCharacteristic(WIFI_PASS_UUID, BLECharacteristic::PROPERTY_WRITE);
  applyChar = service->createCharacteristic(APPLY_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  mqttChar  = service->createCharacteristic(MQTT_CFG_UUID, BLECharacteristic::PROPERTY_WRITE);
  statusChar= service->createCharacteristic(STATUS_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  scanChar  = service->createCharacteristic(SCAN_UUID, BLECharacteristic::PROPERTY_WRITE);
  listChar  = service->createCharacteristic(LIST_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  ssidChar->setCallbacks(new BLECB());
  passChar->setCallbacks(new BLECB());
  applyChar->setCallbacks(new BLECB());
  mqttChar->setCallbacks(new BLECB());
  scanChar->setCallbacks(new BLECB());

  statusChar->addDescriptor(new BLE2902());
  listChar->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();

  Serial.println("BLE READY");
  printBLECheck();
}

/* =========================
   setup
========================= */
void setup() {
  Serial.begin(115200);

  prefs.begin("espcfg", false);
  saved_ssid = prefs.getString("ssid", "");
  saved_pass = prefs.getString("pass", "");
  mqtt_broker = prefs.getString("mqtt_host", "broker.hivemq.com");
  mqtt_port = prefs.getInt("mqtt_port", 1883);
  mqtt_user = prefs.getString("mqtt_user", "");
  mqtt_pass = prefs.getString("mqtt_pass", "");
  prefs.end();

  if (saved_ssid.length() > 0) {
    flag_connect_wifi = true;
  }

  setupBLE();

  // 看门狗已移除，避免冲突
  Serial.println("ESP32 ready.");
}

/* =========================
   loop
========================= */
void loop() {
  if (flag_connect_wifi) {
    flag_connect_wifi = false;
    connectWiFi();
  }

  if (flag_scan_wifi) {
    flag_scan_wifi = false;
    runScan();
  }

  if (flag_ota_update) {
    flag_ota_update = false;
    if (ota_url.length() > 0) {
      performOTA(ota_url);
      ota_url = "";
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      connectMQTT();
    }
    mqtt.loop();
  }

  delay(10);
  yield();
}