/*
  ESP32 IoT BLE配网 + MQTT + OTA
  修复：
  1. BLE service handle数量（改为32）
  2. 回调内阻塞操作改为标志位，移到loop()
  3. OTA支持HTTPS（WiFiClientSecure）
  4. MQTT配置字符串完整解析并用Preferences保存
  5. MQTT重连改为非阻塞
*/

#include <WiFi.h>
#include <Preferences.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <PubSubClient.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

/* =========================
   UUID（必须和JS一致）
========================= */
#define SERVICE_UUID    "5a67d678-6361-4f32-8396-54c6926c8fa9"

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

WiFiClient    wifiClient;
WiFiClientSecure wifiClientSecure;
PubSubClient  mqtt(wifiClient);

/* BLE 特征 */
BLECharacteristic *ssidChar;
BLECharacteristic *passChar;
BLECharacteristic *applyChar;
BLECharacteristic *mqttChar;
BLECharacteristic *statusChar;
BLECharacteristic *scanChar;
BLECharacteristic *listChar;

/* =========================
   标志位（修复：回调不阻塞）
========================= */
volatile bool flagScan        = false;
volatile bool flagConnectWiFi = false;
volatile bool flagOTA         = false;
String        otaUrl          = "";

/* WiFi凭据 */
String wifiSSID = "";
String wifiPASS = "";

/* MQTT凭据 */
String mqttUrl  = "";
int    mqttPort = 1883;
String mqttUser = "";
String mqttPass = "";

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
   BLE结构检查
========================= */
void printBLECheck() {
  Serial.println("\n========== BLE STRUCT CHECK ==========");
  Serial.println(ssidChar   ? "SSID_CHAR   OK" : "SSID_CHAR   MISSING");
  Serial.println(passChar   ? "PASS_CHAR   OK" : "PASS_CHAR   MISSING");
  Serial.println(applyChar  ? "APPLY_CHAR  OK" : "APPLY_CHAR  MISSING");
  Serial.println(mqttChar   ? "MQTT_CHAR   OK" : "MQTT_CHAR   MISSING");
  Serial.println(statusChar ? "STATUS_CHAR OK" : "STATUS_CHAR MISSING");
  Serial.println(scanChar   ? "SCAN_CHAR   OK" : "SCAN_CHAR   MISSING");
  Serial.println(listChar   ? "LIST_CHAR   OK" : "LIST_CHAR   MISSING");
  Serial.println("======================================\n");
}

/* =========================
   WiFi扫描（在loop里调用，非回调）
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
   OTA（支持HTTP和HTTPS）
========================= */
void handleOTA(String url) {
  notify("OTA start: " + url);

  t_httpUpdate_return ret;

  if (url.startsWith("https://")) {
    wifiClientSecure.setInsecure(); // 生产环境请替换为证书校验
    ret = httpUpdate.update(wifiClientSecure, url);
  } else {
    ret = httpUpdate.update(wifiClient, url);
  }

  switch (ret) {
    case HTTP_UPDATE_OK:
      notify("OTA success - rebooting");
      break;
    case HTTP_UPDATE_FAILED:
      notify("OTA failed: " + String(httpUpdate.getLastErrorString()));
      break;
    case HTTP_UPDATE_NO_UPDATES:
      notify("OTA no update");
      break;
  }
}

/* =========================
   MQTT回调
========================= */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println("[MQTT] topic=" + String(topic) + " msg=" + msg);

  if (String(topic) == "esp32/update") {
    // 解析JSON中的url字段
    int key = msg.indexOf("\"url\":\"");
    if (key != -1) {
      int start = key + 7;
      int end   = msg.indexOf("\"", start);
      otaUrl    = msg.substring(start, end);
      flagOTA   = true; // 在loop()里执行，不在回调里阻塞
    } else {
      notify("OTA parse error");
    }
  }
}

/* =========================
   WiFi连接（在loop里调用）
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
    // 保存成功的凭据
    prefs.begin("wifi", false);
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPASS);
    prefs.end();
  } else {
    notify("wifi_failed");
  }
}

/* =========================
   MQTT连接（非阻塞，每次loop尝试一次）
========================= */
void connectMQTT() {
  if (mqtt.connected()) return;
  if (mqttUrl.isEmpty()) return;

  // 从保存的配置设置server
  mqtt.setServer(mqttUrl.c_str(), mqttPort);
  mqtt.setCallback(mqttCallback);

  String clientId = "esp32-" + String(random(1000, 9999));

  Serial.println("[MQTT] connecting to " + mqttUrl + ":" + String(mqttPort));

  bool ok;
  if (mqttUser.length() > 0) {
    ok = mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
  } else {
    ok = mqtt.connect(clientId.c_str());
  }

  if (ok) {
    mqtt.subscribe("esp32/update");
    notify("MQTT connected");
  } else {
    Serial.println("[MQTT] connect failed, rc=" + String(mqtt.state()));
    // 不在这里delay或while，loop()会再次尝试
  }
}

/* =========================
   MQTT配置解析（格式：url|port|user|pass）
========================= */
void parseMQTTConfig(String raw) {
  // 格式: broker.hivemq.com|1883|user|pass
  // 或者: wss://xxx:8884/mqtt|8884|user|pass（JS传来的格式）
  int p1 = raw.indexOf('|');
  int p2 = raw.indexOf('|', p1 + 1);
  int p3 = raw.indexOf('|', p2 + 1);

  if (p1 == -1) {
    notify("mqtt_cfg_parse_error");
    return;
  }

  mqttUrl  = raw.substring(0, p1);
  mqttPort = (p2 != -1) ? raw.substring(p1 + 1, p2).toInt() : 1883;
  mqttUser = (p2 != -1 && p3 != -1) ? raw.substring(p2 + 1, p3) : "";
  mqttPass = (p3 != -1) ? raw.substring(p3 + 1) : "";

  if (mqttPort == 0) mqttPort = 1883;

  // 保存到NVS
  prefs.begin("mqtt", false);
  prefs.putString("url",  mqttUrl);
  prefs.putInt   ("port", mqttPort);
  prefs.putString("user", mqttUser);
  prefs.putString("pass", mqttPass);
  prefs.end();

  Serial.println("[MQTT CFG] url="  + mqttUrl  +
                 " port=" + String(mqttPort) +
                 " user=" + mqttUser);

  // 断开旧连接，loop()会重新连接
  mqtt.disconnect();

  notify("mqtt_saved");
}

/* =========================
   BLE回调（只写标志位，不做阻塞操作）
========================= */
class BLECB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) {
    String uuid = c->getUUID().toString().c_str();
    String val  = c->getValue().c_str();
    Serial.println("[BLE RX] " + uuid + " = " + val);

    if (uuid == WIFI_SSID_UUID)  wifiSSID = val;
    if (uuid == WIFI_PASS_UUID)  wifiPASS = val;

    if (uuid == APPLY_WIFI_UUID && val == "1") {
      flagConnectWiFi = true; // 在loop()里执行
    }

    if (uuid == SCAN_UUID && val == "1") {
      flagScan = true; // 在loop()里执行
    }

    if (uuid == MQTT_CFG_UUID) {
      parseMQTTConfig(val);
    }
  }
};

/* =========================
   BLE初始化
   修复：service创建时指定handle数量为32
========================= */
void setupBLE() {
  BLEDevice::init("ESP32-Setup");

  BLEServer  *server  = BLEDevice::createServer();

  // 修复：第二个参数指定numHandles=32，防止超出默认15个handle的限制
  BLEService *service = server->createService(BLEUUID(SERVICE_UUID), 32);

  /* ===== 创建所有Characteristic ===== */
  ssidChar  = service->createCharacteristic(WIFI_SSID_UUID,  BLECharacteristic::PROPERTY_WRITE);
  passChar  = service->createCharacteristic(WIFI_PASS_UUID,  BLECharacteristic::PROPERTY_WRITE);
  applyChar = service->createCharacteristic(APPLY_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  mqttChar  = service->createCharacteristic(MQTT_CFG_UUID,   BLECharacteristic::PROPERTY_WRITE);
  statusChar= service->createCharacteristic(STATUS_UUID,     BLECharacteristic::PROPERTY_NOTIFY);
  scanChar  = service->createCharacteristic(SCAN_UUID,       BLECharacteristic::PROPERTY_WRITE);
  listChar  = service->createCharacteristic(LIST_UUID,       BLECharacteristic::PROPERTY_NOTIFY);

  /* ===== 绑定回调 ===== */
  ssidChar ->setCallbacks(new BLECB());
  passChar ->setCallbacks(new BLECB());
  applyChar->setCallbacks(new BLECB());
  mqttChar ->setCallbacks(new BLECB());
  scanChar ->setCallbacks(new BLECB());

  /* ===== Notify描述符 ===== */
  statusChar->addDescriptor(new BLE2902());
  listChar  ->addDescriptor(new BLE2902());

  /* ===== 启动服务和广播 ===== */
  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  Serial.println("BLE READY");
  printBLECheck();
}

/* =========================
   从NVS加载已保存的配置
========================= */
void loadSavedConfig() {
  prefs.begin("wifi", true);
  wifiSSID = prefs.getString("ssid", "");
  wifiPASS = prefs.getString("pass", "");
  prefs.end();

  prefs.begin("mqtt", true);
  mqttUrl  = prefs.getString("url",  "");
  mqttPort = prefs.getInt   ("port", 1883);
  mqttUser = prefs.getString("user", "");
  mqttPass = prefs.getString("pass", "");
  prefs.end();

  Serial.println("[NVS] wifi ssid=" + wifiSSID);
  Serial.println("[NVS] mqtt url="  + mqttUrl + " port=" + String(mqttPort));
}

/* =========================
   SETUP
========================= */
void setup() {
  Serial.begin(115200);
  delay(500);

  loadSavedConfig();
  setupBLE();

  // 如果有保存的WiFi，自动连接
  if (wifiSSID.length() > 0) {
    Serial.println("[AUTO] connecting saved WiFi: " + wifiSSID);
    connectWiFi();
  }
}

/* =========================
   LOOP（非阻塞）
========================= */
unsigned long lastMQTTRetry = 0;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

void loop() {
  // 处理WiFi扫描标志
  if (flagScan) {
    flagScan = false;
    runScan();
  }

  // 处理WiFi连接标志
  if (flagConnectWiFi) {
    flagConnectWiFi = false;
    connectWiFi();
  }

  // 处理OTA标志
  if (flagOTA) {
    flagOTA = false;
    handleOTA(otaUrl);
  }

  // MQTT维护（非阻塞重试）
  if (WiFi.status() == WL_CONNECTED && mqttUrl.length() > 0) {
    if (!mqtt.connected()) {
      unsigned long now = millis();
      if (now - lastMQTTRetry > MQTT_RETRY_INTERVAL) {
        lastMQTTRetry = now;
        connectMQTT();
      }
    } else {
      mqtt.loop();
    }
  }

  delay(10);
}
