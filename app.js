/* =========================
   BLE UUID（必须匹配ESP32）
========================= */
const SERVICE = "5a67d678-6361-4f32-8396-54c6926c8fa9";

const SCAN    = "5a67d678-6361-4f32-8396-54c6926c8f01";
const LIST    = "5a67d678-6361-4f32-8396-54c6926c8f02";
const SSID    = "5a67d678-6361-4f32-8396-54c6926c8f03";
const PASS    = "5a67d678-6361-4f32-8396-54c6926c8f04";
const APPLY   = "5a67d678-6361-4f32-8396-54c6926c8f05";
const STATUS  = "5a67d678-6361-4f32-8396-54c6926c8f06";

const MQTT_CFG = "5a67d678-6361-4f32-8396-54c6926c9001";

/* =========================
   BLE变量
========================= */
let scanChar, listChar, ssidChar, passChar, applyChar, statusChar, mqttChar;

/* =========================
   MQTT变量
========================= */
let mqttClient;

/* =========================
   log
========================= */
function logBLE(msg){
  document.getElementById("bleLog").textContent += msg + "\n";
}

function logOTA(msg){
  document.getElementById("otaLog").textContent += msg + "\n";
}

/* =========================
   BLE连接
========================= */
document.getElementById("connectBtn").onclick = async () => {

  const device = await navigator.bluetooth.requestDevice({
    acceptAllDevices: true,
    optionalServices: [SERVICE]
  });

  const server = await device.gatt.connect();
  const service = await server.getPrimaryService(SERVICE);

  scanChar   = await service.getCharacteristic(SCAN);
  listChar   = await service.getCharacteristic(LIST);
  ssidChar   = await service.getCharacteristic(SSID);
  passChar   = await service.getCharacteristic(PASS);
  applyChar  = await service.getCharacteristic(APPLY);
  statusChar = await service.getCharacteristic(STATUS);
  mqttChar   = await service.getCharacteristic(MQTT_CFG);

  await statusChar.startNotifications();
  statusChar.addEventListener("characteristicvaluechanged", e=>{
    logBLE("[ESP32] " + new TextDecoder().decode(e.target.value));
  });

  await listChar.startNotifications();
  listChar.addEventListener("characteristicvaluechanged", e=>{
    const arr = JSON.parse(new TextDecoder().decode(e.target.value));

    const sel = document.getElementById("ssidList");
    sel.innerHTML = "";

    arr.forEach(ap=>{
      const opt = document.createElement("option");
      opt.value = ap.s;
      opt.textContent = `${ap.s} (${ap.r})`;
      sel.appendChild(opt);
    });

    logBLE("WiFi列表更新");
  });

  logBLE("BLE连接成功");
};

/* =========================
   WiFi扫描
========================= */
document.getElementById("scanBtn").onclick = async () => {
  await scanChar.writeValue(new TextEncoder().encode("1"));
  logBLE("扫描WiFi...");
};

/* =========================
   WiFi连接
========================= */
document.getElementById("applyBtn").onclick = async () => {

  const ssid = document.getElementById("ssidList").value;
  const pass = document.getElementById("password").value;

  await ssidChar.writeValue(new TextEncoder().encode(ssid));
  await passChar.writeValue(new TextEncoder().encode(pass));
  await applyChar.writeValue(new TextEncoder().encode("1"));

  logBLE("连接WiFi...");
};

/* =========================
   MQTT配置 BLE下发
========================= */
document.getElementById("sendMqttBtn").onclick = async () => {

  const url  = document.getElementById("mqttUrl").value;
  const user = document.getElementById("mqttUser").value;
  const pass = document.getElementById("mqttPass").value;
  const port = 8883;

  const data = `${url}|${port}|${user}|${pass}`;

  await mqttChar.writeValue(
    new TextEncoder().encode(data)
  );

  logBLE("MQTT配置已发送到ESP32");
};

/* =========================
   MQTT连接（WebSocket）
========================= */
document.getElementById("mqttConnectBtn").onclick = () => {

  const url  = document.getElementById("mqttUrl").value;
  const user = document.getElementById("mqttUser").value;
  const pass = document.getElementById("mqttPass").value;

  mqttClient = mqtt.connect(url, {
    username: user,
    password: pass
  });

  mqttClient.on("connect", () => {
    logOTA("MQTT连接成功");
  });

  mqttClient.on("error", err => {
    logOTA("MQTT错误: " + err.message);
  });
};

/* =========================
   OTA推送
========================= */
document.getElementById("otaBtn").onclick = () => {

  const url = document.getElementById("firmwareUrl").value;

  mqttClient.publish(
    "esp32/update",
    JSON.stringify({
      url: url,
      version: "1.0.0"
    })
  );

  logOTA("OTA已推送");
};