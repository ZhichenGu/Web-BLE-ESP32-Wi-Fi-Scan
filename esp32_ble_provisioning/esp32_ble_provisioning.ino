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
   BLE UUID
========================= */
#define SERVICE_UUID "5a67d678-6361-4f32-8396-54c6926c8fa9"

#define WIFI_SSID_UUID  "5a67d678-6361-4f32-8396-54c6926c8f03"
#define WIFI_PASS_UUID  "5a67d678-6361-4f32-8396-54c6926c8f04"
#define APPLY_WIFI_UUID "5a67d678-6361-4f32-8396-54c6926c8f05"

#define MQTT_CFG_UUID   "5a67d678-6361-4f32-8396-54c6926c9001"
#define STATUS_UUID     "5a67d678-6361-4f32-8396-54c6926c8f06"

/* =========================
   存储
========================= */
Preferences prefs;

/* =========================
   WiFi / MQTT
========================= */
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String mqttHost;
int mqttPort = 1883;
String mqttUser;
String mqttPass;

String ssid;
String pass;

/* =========================
   BLE
========================= */
BLECharacteristic *ssidChar;
BLECharacteristic *passChar;
BLECharacteristic *applyChar;
BLECharacteristic *mqttChar;
BLECharacteristic *statusChar;

/* =========================
   状态通知
========================= */
void notify(String msg){
  if(statusChar){
    statusChar->setValue(msg.c_str());
    statusChar->notify();
  }
  Serial.println("[STATUS] " + msg);
}

/* =========================
   OTA处理（稳定版解析）
========================= */
void handleOTA(String msg){

  String url = "";

  int key = msg.indexOf("\"url\":\"");
  if (key != -1) {
    int start = key + 7;
    int end = msg.indexOf("\"", start);
    url = msg.substring(start, end);
  }

  if(url == ""){
    notify("OTA url error");
    return;
  }

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
   MQTT回调
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
   MQTT连接（带自动重连）
========================= */
void connectMQTT(){

  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setCallback(callback);

  String clientId = "esp32-" + String(random(1000,9999));

  while(!mqtt.connected()){

    Serial.println("MQTT connecting...");

    if(mqtt.connect(
      clientId.c_str(),
      mqttUser.c_str(),
      mqttPass.c_str()
    )){
      mqtt.subscribe("esp32/update");
      notify("MQTT connected");
    } else {
      delay(2000);
    }
  }
}

/* =========================
   WiFi连接
========================= */
void connectWiFi(){

  notify("WiFi connecting");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t = millis();

  while(WiFi.status() != WL_CONNECTED && millis() - t < 15000){
    delay(300);
  }

  if(WiFi.status() == WL_CONNECTED){

    notify("WiFi OK");

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();

    connectMQTT();

  } else {
    notify("WiFi failed");
  }
}

/* =========================
   BLE回调
========================= */
class BLECB : public BLECharacteristicCallbacks {

  void onWrite(BLECharacteristic *c){

    String uuid = c->getUUID().toString().c_str();
    String val  = c->getValue().c_str();

    if(uuid == WIFI_SSID_UUID){
      ssid = val;
      Serial.println("SSID: " + ssid);
    }

    if(uuid == WIFI_PASS_UUID){
      pass = val;
    }

    if(uuid == APPLY_WIFI_UUID){
      if(val == "1"){
        connectWiFi();
      }
    }

    if(uuid == MQTT_CFG_UUID){

      Serial.println("MQTT config received");

      int p1 = val.indexOf('|');
      int p2 = val.indexOf('|', p1+1);
      int p3 = val.indexOf('|', p2+1);

      mqttHost = val.substring(0, p1);
      mqttPort = val.substring(p1+1, p2).toInt();
      mqttUser = val.substring(p2+1, p3);
      mqttPass = val.substring(p3+1);

      prefs.begin("mqtt", false);
      prefs.putString("host", mqttHost);
      prefs.putString("user", mqttUser);
      prefs.putString("pass", mqttPass);
      prefs.putInt("port", mqttPort);
      prefs.end();

      notify("MQTT saved");
    }
  }
};

/* =========================
   BLE初始化
========================= */
void setupBLE(){

  BLEDevice::init("ESP32-Setup");

  BLEServer *server = BLEDevice::createServer();
  BLEService *service = server->createService(SERVICE_UUID);

  ssidChar  = service->createCharacteristic(WIFI_SSID_UUID, BLECharacteristic::PROPERTY_WRITE);
  passChar  = service->createCharacteristic(WIFI_PASS_UUID, BLECharacteristic::PROPERTY_WRITE);
  applyChar = service->createCharacteristic(APPLY_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  mqttChar  = service->createCharacteristic(MQTT_CFG_UUID, BLECharacteristic::PROPERTY_WRITE);
  statusChar= service->createCharacteristic(STATUS_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  ssidChar->setCallbacks(new BLECB());
  passChar->setCallbacks(new BLECB());
  applyChar->setCallbacks(new BLECB());
  mqttChar->setCallbacks(new BLECB());

  statusChar->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();

  Serial.println("BLE Ready");
}

/* =========================
   加载MQTT
========================= */
void loadMQTT(){

  prefs.begin("mqtt", true);

  mqttHost = prefs.getString("host","");
  mqttUser = prefs.getString("user","");
  mqttPass = prefs.getString("pass","");
  mqttPort = prefs.getInt("port",1883);

  prefs.end();
}

/* =========================
   自动WiFi
========================= */
void autoWiFi(){

  prefs.begin("wifi", true);

  ssid = prefs.getString("ssid","");
  pass = prefs.getString("pass","");

  prefs.end();

  if(ssid != ""){
    connectWiFi();
  }
}

/* =========================
   SETUP
========================= */
void setup(){

  Serial.begin(115200);

  setupBLE();

  loadMQTT();
  autoWiFi();
}

/* =========================
   LOOP（关键修复：自动重连）
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