/*
 * ESP32 BLE WiFi 配网 (第1块)
 * ---------------------------------------------------------------
 * 功能:
 *   1. 通过 BLE 让网站(Web Bluetooth)给 ESP32 下发 WiFi 账号密码
 *   2. ESP32 可自己扫描周边 WiFi,把列表回传给网站
 *   3. 连接结果通过 notify 实时回传
 *   4. 成功后把账号密码存入 NVS,下次开机自动重连(为阶段二 OTA 铺路)
 *
 * 环境: Arduino IDE + ESP32 board 包 (建议 3.x)。无需额外安装库。
 * 选板: 你的具体 ESP32 开发板 (如 "ESP32 Dev Module")
 *
 * 网站端必须使用与下面完全相同的 UUID。
 * ---------------------------------------------------------------
 */

#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ===== UUID (网站端要和这里一模一样) =====
#define SERVICE_UUID        "5a67d678-6361-4f32-8396-54c6926c8fa9"
#define CHAR_SCAN_UUID      "5a67d678-6361-4f32-8396-54c6926c8f01" // 写 "1" 触发扫描
#define CHAR_SSIDLIST_UUID  "5a67d678-6361-4f32-8396-54c6926c8f02" // 读/通知: 扫描结果 JSON
#define CHAR_SSID_UUID      "5a67d678-6361-4f32-8396-54c6926c8f03" // 写: 选中的 SSID
#define CHAR_PASS_UUID      "5a67d678-6361-4f32-8396-54c6926c8f04" // 写: 密码
#define CHAR_APPLY_UUID     "5a67d678-6361-4f32-8396-54c6926c8f05" // 写 "1" 触发连接+保存
#define CHAR_STATUS_UUID    "5a67d678-6361-4f32-8396-54c6926c8f06" // 读/通知: 状态反馈

#define DEVICE_NAME         "ESP32-Setup"   // 网站扫描时看到的名字

Preferences prefs;

BLECharacteristic *chScanList;
BLECharacteristic *chStatus;

String pendingSsid = "";
String pendingPass = "";

volatile bool doScan  = false;
volatile bool doApply = false;
bool deviceConnected  = false;

// 兼容 ESP32 core 2.x(std::string)和 3.x(String):统一转成 Arduino String
static String charValue(BLECharacteristic *c) {
  return String(c->getValue().c_str());
}
static String charUuid(BLECharacteristic *c) {
  return String(c->getUUID().toString().c_str());
}

void notifyStatus(const String &msg) {
  if (chStatus) {
    chStatus->setValue(msg.c_str());
    chStatus->notify();
  }
  Serial.println("[STATUS] " + msg);
}

// ---- BLE 连接/断开回调:断开后重新广播,方便重新配网 ----
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    deviceConnected = true;
    Serial.println("[BLE] client connected");
  }
  void onDisconnect(BLEServer *s) override {
    deviceConnected = false;
    Serial.println("[BLE] client disconnected, re-advertising");
    delay(300);
    BLEDevice::startAdvertising();
  }
};

// ---- 写特征回调:根据 UUID 区分是哪个特征被写 ----
class WriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String uuid = charUuid(c);
    String val  = charValue(c);

    if (uuid.equalsIgnoreCase(CHAR_SCAN_UUID)) {
      if (val == "1") doScan = true;                 // 在 loop 里执行扫描
    } else if (uuid.equalsIgnoreCase(CHAR_SSID_UUID)) {
      pendingSsid = val;
      Serial.println("[RX] ssid = " + pendingSsid);
    } else if (uuid.equalsIgnoreCase(CHAR_PASS_UUID)) {
      pendingPass = val;
      Serial.println("[RX] pass = (len " + String(pendingPass.length()) + ")");
    } else if (uuid.equalsIgnoreCase(CHAR_APPLY_UUID)) {
      if (val == "1") doApply = true;                // 在 loop 里执行连接
    }
  }
};

// ---- 扫描周边 WiFi,拼成 JSON 存入 chScanList ----
// 必须在 loop 里调用,不要在 BLE 回调线程里跑扫描
void runScan() {
  notifyStatus("scanning");
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();      // 阻塞式扫描
  int cap = (n > 12) ? 12 : n;      // 上限 12,避免超过 BLE 512 字节特征上限

  String json = "[";
  for (int i = 0; i < cap; i++) {
    if (i) json += ",";
    String ss = WiFi.SSID(i);
    ss.replace("\\", "\\\\");       // 转义,保证 JSON 合法
    ss.replace("\"", "\\\"");
    int locked = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? 0 : 1;
    json += "{\"s\":\"" + ss + "\",\"r\":" + String(WiFi.RSSI(i)) +
            ",\"l\":" + String(locked) + "}";
  }
  json += "]";
  WiFi.scanDelete();

  chScanList->setValue(json.c_str());
  chScanList->notify();             // 通知网站:可以来读列表了
  notifyStatus("scan_done");
  Serial.println("[SCAN] " + json);
}

// ---- 用 pending 的账号密码连接,成功则保存 ----
void runApply() {
  notifyStatus("connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(pendingSsid.c_str(), pendingPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", pendingSsid);
    prefs.putString("pass", pendingPass);
    prefs.end();
    notifyStatus("connected:" + WiFi.localIP().toString());
  } else {
    notifyStatus("failed");
  }
}

// ---- 开机尝试用已保存的账号密码自动连接 ----
bool tryAutoConnect() {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  prefs.end();
  if (s.length() == 0) return false;

  Serial.println("[BOOT] auto-connecting to saved SSID: " + s);
  WiFi.mode(WIFI_STA);
  WiFi.begin(s.c_str(), p.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(300);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[BOOT] connected: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("[BOOT] auto-connect failed, waiting for BLE provisioning");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 BLE Provisioning ===");

  tryAutoConnect();   // 有保存的就先连上;BLE 仍然开着,可随时重新配网

  // ---- 初始化 BLE ----
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(517);   // 请求较大 MTU,方便回传较长的扫描结果

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *svc = pServer->createService(SERVICE_UUID);

  // 触发扫描 (write)
  BLECharacteristic *chScan = svc->createCharacteristic(
      CHAR_SCAN_UUID, BLECharacteristic::PROPERTY_WRITE);
  chScan->setCallbacks(new WriteCallbacks());

  // 扫描结果 (read + notify)
  chScanList = svc->createCharacteristic(
      CHAR_SSIDLIST_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chScanList->addDescriptor(new BLE2902());
  chScanList->setValue("[]");

  // SSID (write)
  BLECharacteristic *chSsid = svc->createCharacteristic(
      CHAR_SSID_UUID, BLECharacteristic::PROPERTY_WRITE);
  chSsid->setCallbacks(new WriteCallbacks());

  // PASS (write)
  BLECharacteristic *chPass = svc->createCharacteristic(
      CHAR_PASS_UUID, BLECharacteristic::PROPERTY_WRITE);
  chPass->setCallbacks(new WriteCallbacks());

  // 触发连接 (write)
  BLECharacteristic *chApply = svc->createCharacteristic(
      CHAR_APPLY_UUID, BLECharacteristic::PROPERTY_WRITE);
  chApply->setCallbacks(new WriteCallbacks());

  // 状态反馈 (read + notify)
  chStatus = svc->createCharacteristic(
      CHAR_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chStatus->addDescriptor(new BLE2902());
  chStatus->setValue("idle");

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] advertising as \"" DEVICE_NAME "\"");
}

void loop() {
  // BLE 回调里只置标志位,真正耗时的活在 loop 里做
  if (doScan)  { doScan  = false; runScan();  }
  if (doApply) { doApply = false; runApply(); }
  delay(20);
}
