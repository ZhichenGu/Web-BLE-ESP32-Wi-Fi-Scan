#include <WiFi.h>
#include <Preferences.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <PubSubClient.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

/* =========================
   UUID（必须和JS一致）
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

/* BLE 特征 */
BLECharacteristic *ssidChar;
BLECharacteristic *passChar;
BLECharacteristic *applyChar;
BLECharacteristic *mqttChar;
BLECharacteristic *statusChar;
BLECharacteristic *scanChar;
BLECharacteristic *listChar;

/* =========================
   状态通知
========================= */
void notify(String msg){
  if(statusChar){
    statusChar->setValue(msg.c_str());
    statusChar->notify();
  }
  Serial.println("[ESP32] " + msg);
}

/* =========================
   BLE结构检查（重点新增）
========================= */
void printBLECheck(){

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
   WiFi扫描
========================= */
void runScan(){

  notify("scanning");

  WiFi.mode(WIFI_STA);

  int n = WiFi.scanNetworks();
  int cap = (n > 10) ? 10 : n;

  String json = "[";

  for(int i=0;i<cap;i++){
    if(i) json += ",";

    String s = WiFi.SSID(i);
    s.replace("\"","\\\"");

    json += "{\"s\":\"" + s + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
  }

  json += "]";

  listChar->setValue(json.c_str());
  listChar->notify();

  notify("scan_done");
}

/* =========================
   OTA
========================= */
void handleOTA(String msg){

  int key = msg.indexOf("\"url\":\"");
  if(key == -1){
    notify("OTA parse error");
    return;
  }

  int start = key + 7;
  int end = msg.indexOf("\"", start);

  String url = msg.substring(start, end);

  notify("OTA start");

  WiFiClient client;
  t_httpUpdate_return ret = httpUpdate.update(client, url);

  if(ret == HTTP_UPDATE_OK){
    notify("OTA success");
  } else {
    notify("OTA failed");
  }
}

/* =========================
   MQTT
========================= */
void callback(char* topic, byte* payload, unsigned int length){

  String msg;
  for(int i=0;i<length;i++) msg += (char)payload[i];

  Serial.println("[MQTT] " + msg);

  if(String(topic) == "esp32/update"){
    handleOTA(msg);
  }
}

/* =========================
   WiFi
========================= */
String ssid, pass;

void connectWiFi(){

  notify("wifi_connecting");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t = millis();

  while(WiFi.status() != WL_CONNECTED && millis() - t < 15000){
    delay(300);
  }

  if(WiFi.status() == WL_CONNECTED){
    notify("wifi_ok");
  } else {
    notify("wifi_failed");
  }
}

/* =========================
   MQTT连接
========================= */
void connectMQTT(){

  mqtt.setServer("broker.hivemq.com", 1883);
  mqtt.setCallback(callback);

  String clientId = "esp32-" + String(random(1000,9999));

  while(!mqtt.connected()){

    notify("MQTT connecting");

    if(mqtt.connect(clientId.c_str())){
      mqtt.subscribe("esp32/update");
      notify("MQTT connected");
    } else {
      delay(2000);
    }
  }
}

/* =========================
   BLE回调
========================= */
class BLECB : public BLECharacteristicCallbacks {

  void onWrite(BLECharacteristic *c){

    String uuid = c->getUUID().toString().c_str();
    String val  = c->getValue().c_str();

    Serial.println("[BLE RX] " + uuid + " = " + val);

    if(uuid == WIFI_SSID_UUID) ssid = val;
    if(uuid == WIFI_PASS_UUID) pass = val;

    if(uuid == APPLY_WIFI_UUID && val == "1"){
      connectWiFi();
    }

    if(uuid == SCAN_UUID && val == "1"){
      runScan();
    }

    if(uuid == MQTT_CFG_UUID){
      notify("mqtt_saved");
    }
  }
};

/* =========================
   BLE初始化（关键）
========================= */
void setupBLE(){

  BLEDevice::init("ESP32-Setup");

  BLEServer *server = BLEDevice::createServer();
  BLEService *service = server->createService(SERVICE_UUID);

  /* ===== 创建所有Characteristic ===== */
  ssidChar  = service->createCharacteristic(WIFI_SSID_UUID, BLECharacteristic::PROPERTY_WRITE);
  passChar  = service->createCharacteristic(WIFI_PASS_UUID, BLECharacteristic::PROPERTY_WRITE);
  applyChar = service->createCharacteristic(APPLY_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);

  mqttChar  = service->createCharacteristic(MQTT_CFG_UUID, BLECharacteristic::PROPERTY_WRITE);

  statusChar= service->createCharacteristic(STATUS_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  scanChar  = service->createCharacteristic(SCAN_UUID, BLECharacteristic::PROPERTY_WRITE);
  listChar  = service->createCharacteristic(LIST_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  /* ===== callback ===== */
  ssidChar->setCallbacks(new BLECB());
  passChar->setCallbacks(new BLECB());
  applyChar->setCallbacks(new BLECB());
  mqttChar->setCallbacks(new BLECB());
  scanChar->setCallbacks(new BLECB());

  /* ===== notify ===== */
  statusChar->addDescriptor(new BLE2902());
  listChar->addDescriptor(new BLE2902());

  /* ===== 启动 ===== */
  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();

  Serial.println("BLE READY");

  /* ===== ⭐关键：启动后检查 ===== */
  printBLECheck();
}

/* =========================
   SETUP
========================= */
void setup(){

  Serial.begin(115200);

  setupBLE();
}

/* =========================
   LOOP
========================= */
void loop(){

  if(WiFi.status() == WL_CONNECTED){
    if(!mqtt.connected()){
      connectMQTT();
    }
    mqtt.loop();
  }

  delay(10);
}