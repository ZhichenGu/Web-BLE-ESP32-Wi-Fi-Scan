/*
 * ESP32 BLE WiFi 配网
 * 
 * 依赖库（Arduino Library Manager 安装）：
 *   - ESP32 BLE Arduino（通常随 esp32 board 自带）
 * 
 * Board: ESP32 Dev Module
 * 
 * BLE Service UUID:  4fafc201-1fb5-459e-8fcc-c5c9c331914b
 * 
 * Characteristics:
 *   SCAN_CHAR   (notify) : ESP32 推送扫描到的 WiFi 列表 (JSON)
 *   SSID_CHAR   (write)  : 网页写入目标 SSID
 *   PASS_CHAR   (write)  : 网页写入密码
 *   CMD_CHAR    (write)  : 网页发送命令 ("scan" / "connect")
 *   STATUS_CHAR (notify) : ESP32 推送连接状态
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <ArduinoJson.h>  // 需要安装: ArduinoJson by Benoit Blanchon

// ── UUID 定义 ──────────────────────────────────────────────
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define SCAN_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define SSID_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PASS_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define CMD_CHAR_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define STATUS_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26ac"

// ── 全局变量 ───────────────────────────────────────────────
BLEServer*         pServer        = nullptr;
BLECharacteristic* pScanChar      = nullptr;
BLECharacteristic* pStatusChar    = nullptr;

String targetSSID   = "";
String targetPass   = "";
bool   doScan       = false;
bool   doConnect    = false;
bool   deviceConnected = false;

// ── BLE 连接回调 ───────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("[BLE] 网页已连接");
  }
  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("[BLE] 网页已断开，重新开始广播...");
    BLEDevice::startAdvertising();
  }
};

// ── 命令 Characteristic 回调 ──────────────────────────────
class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String cmd = pChar->getValue().c_str();
    cmd.trim();
    Serial.print("[CMD] 收到命令: ");
    Serial.println(cmd);
    if (cmd == "scan")    doScan    = true;
    if (cmd == "connect") doConnect = true;
  }
};

// ── SSID Characteristic 回调 ──────────────────────────────
class SsidCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    targetSSID = pChar->getValue().c_str();
    Serial.print("[SSID] 收到: ");
    Serial.println(targetSSID);
  }
};

// ── 密码 Characteristic 回调 ──────────────────────────────
class PassCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    targetPass = pChar->getValue().c_str();
    Serial.println("[PASS] 密码已收到");
  }
};

// ── 推送状态到网页 ─────────────────────────────────────────
void sendStatus(const String& status) {
  Serial.print("[STATUS] ");
  Serial.println(status);
  if (deviceConnected) {
    pStatusChar->setValue(status.c_str());
    pStatusChar->notify();
  }
}

// ── 扫描 WiFi 并通过 BLE 推送 ─────────────────────────────
void scanAndSend() {
  sendStatus("scanning");
  Serial.println("[SCAN] 开始扫描 WiFi...");

  int n = WiFi.scanNetworks();
  Serial.print("[SCAN] 找到 ");
  Serial.print(n);
  Serial.println(" 个网络");

  // 构建 JSON，分块发送（BLE MTU ~20-512 bytes）
  // 格式: [{"ssid":"...","rssi":-60,"enc":true}, ...]
  // 每次最多发 500 字节，超出则分多条
  const int CHUNK = 500;
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"";
    // 转义 SSID 中的双引号
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");
    json += ssid;
    json += "\",\"rssi\":";
    json += WiFi.RSSI(i);
    json += ",\"enc\":";
    json += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
    json += "}";

    // 如果快撑满了，先发出去
    if (json.length() > CHUNK || i == n - 1) {
      if (i == n - 1) json += "]";  // 最后一包才关闭数组
      // 加序号前缀让网页知道是否结束: "0:data" 中间包, "1:data" 最后包
      String chunk = (i == n - 1 ? "1:" : "0:") + json;
      if (deviceConnected) {
        pScanChar->setValue(chunk.c_str());
        pScanChar->notify();
        delay(50);
      }
      // 为下一块重置，但不重置已发送的网络
      if (i < n - 1) json = "[";
    }
  }

  if (n == 0) {
    if (deviceConnected) {
      pScanChar->setValue("1:[]");
      pScanChar->notify();
    }
  }

  WiFi.scanDelete();
  sendStatus("scan_done");
}

// ── 连接 WiFi ─────────────────────────────────────────────
void connectToWiFi() {
  if (targetSSID.isEmpty()) {
    sendStatus("error:no_ssid");
    return;
  }
  sendStatus("connecting");
  Serial.print("[WiFi] 正在连接: ");
  Serial.println(targetSSID);

  WiFi.begin(targetSSID.c_str(), targetPass.c_str());

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    retry++;
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.print("[WiFi] 连接成功！IP: ");
    Serial.println(ip);
    sendStatus("connected:" + ip);
  } else {
    Serial.println("[WiFi] 连接失败");
    sendStatus("error:failed");
    WiFi.disconnect();
  }
}

// ── Setup ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[ESP32] BLE WiFi 配网启动");

  // WiFi 设为 STA 模式（不自动连接，只用于扫描和连接）
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // 初始化 BLE
  BLEDevice::init("ESP32-WiFi-Setup");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Scan characteristic（notify）
  pScanChar = pService->createCharacteristic(
    SCAN_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pScanChar->addDescriptor(new BLE2902());

  // SSID characteristic（write）
  BLECharacteristic* pSsidChar = pService->createCharacteristic(
    SSID_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pSsidChar->setCallbacks(new SsidCallbacks());

  // Password characteristic（write）
  BLECharacteristic* pPassChar = pService->createCharacteristic(
    PASS_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pPassChar->setCallbacks(new PassCallbacks());

  // Command characteristic（write）
  BLECharacteristic* pCmdChar = pService->createCharacteristic(
    CMD_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCmdChar->setCallbacks(new CmdCallbacks());

  // Status characteristic（notify）
  pStatusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pStatusChar->addDescriptor(new BLE2902());

  pService->start();

  // 开始广播
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] 广播中，设备名: ESP32-WiFi-Setup");
}

// ── Loop ───────────────────────────────────────────────────
void loop() {
  if (doScan) {
    doScan = false;
    scanAndSend();
  }
  if (doConnect) {
    doConnect = false;
    connectToWiFi();
  }
  delay(10);
}
