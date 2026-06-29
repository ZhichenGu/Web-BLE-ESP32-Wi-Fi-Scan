
/* =========================
   BLE UUID
========================= */
const SERVICE = "5a67d678-6361-4f32-8396-54c6926c8fa9";
const SCAN   = "5a67d678-6361-4f32-8396-54c6926c8f01";
const LIST   = "5a67d678-6361-4f32-8396-54c6926c8f02";
const SSID   = "5a67d678-6361-4f32-8396-54c6926c8f03";
const PASS   = "5a67d678-6361-4f32-8396-54c6926c8f04";
const APPLY  = "5a67d678-6361-4f32-8396-54c6926c8f05";
const STATUS = "5a67d678-6361-4f32-8396-54c6926c8f06";
const MQTT   = "5a67d678-6361-4f32-8396-54c6926c9001";

/* =========================
   BLE变量
========================= */
let scanChar, listChar, ssidChar, passChar, applyChar, statusChar, mqttChar;

/* =========================
   MQTT
========================= */
let mqttClient;

/* =========================
   LOG
========================= */
function logBLE(msg){
  document.getElementById("bleLog").textContent += msg + "\n";
}

function logOTA(msg){
  document.getElementById("otaLog").textContent += msg + "\n";
}

/* =========================
   连接BLE
========================= */
document.getElementById("connectBtn").onclick = async () => {

  try {
    logBLE("🔍 searching ESP32...");

    const device = await navigator.bluetooth.requestDevice({
      acceptAllDevices: true,
      optionalServices: [SERVICE]
    });

    logBLE("✅ selected: " + device.name);

    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);

    scanChar  = await service.getCharacteristic(SCAN);
    listChar  = await service.getCharacteristic(LIST);
    ssidChar  = await service.getCharacteristic(SSID);
    passChar  = await service.getCharacteristic(PASS);
    applyChar = await service.getCharacteristic(APPLY);
    statusChar= await service.getCharacteristic(STATUS);
    mqttChar  = await service.getCharacteristic(MQTT);

    /* ===== ESP32状态 ===== */
    await statusChar.startNotifications();
    statusChar.addEventListener("characteristicvaluechanged", e=>{
      const msg = new TextDecoder().decode(e.target.value);
      logBLE("📡 ESP32: " + msg);
    });

    /* ===== WiFi列表 ===== */
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

      logBLE("📶 WiFi列表更新");
    });

    logBLE("✅ BLE READY");

  } catch(err){
    logBLE("❌ BLE ERROR: " + err);
  }
};

/* =========================
   扫描WiFi（加确认）
========================= */
document.getElementById("scanBtn").onclick = async () => {

  if(!scanChar){
    logBLE("❌ 未连接ESP32");
    return;
  }

  try {
    await scanChar.writeValue(new TextEncoder().encode("1"));
    logBLE("➡️ scan command sent");
  } catch(err){
    logBLE("❌ scan failed: " + err);
  }
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

  logBLE("➡️ WiFi connect sent");
};

/* =========================
   MQTT配置 BLE下发（关键）
========================= */
document.getElementById("sendMqttBtn").onclick = async () => {

  const url  = document.getElementById("mqttUrl").value;
  const user = document.getElementById("mqttUser").value;
  const pass = document.getElementById("mqttPass").value;

  const payload = `${url}|8883|${user}|${pass}`;

  try {
    await mqttChar.writeValue(new TextEncoder().encode(payload));
    logBLE("✅ MQTT config sent");
  } catch(err){
    logBLE("❌ MQTT send failed: " + err);
  }
};

/* =========================
   MQTT连接
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
    logOTA("MQTT connected");
  });

  mqttClient.on("error", e=>{
    logOTA("MQTT error: " + e.message);
  });
};

/* =========================
   OTA
========================= */
document.getElementById("otaBtn").onclick = () => {

  const url = document.getElementById("firmwareUrl").value;

  mqttClient.publish("esp32/update", JSON.stringify({
    url: url
  }));

  logOTA("OTA sent");
};