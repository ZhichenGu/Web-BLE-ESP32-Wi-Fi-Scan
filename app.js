/************************************************************
 * ESP32 IoT 控制台 - app.js（BLE + MQTT + OTA）
 ************************************************************/

/* =========================
   🔵 BLE UUID（必须与ESP32一致）
========================= */
const SERVICE = "5a67d678-6361-4f32-8396-54c6926c8fa9";

const SCAN    = "5a67d678-6361-4f32-8396-54c6926c8f01";
const LIST    = "5a67d678-6361-4f32-8396-54c6926c8f02";
const SSID    = "5a67d678-6361-4f32-8396-54c6926c8f03";
const PASS    = "5a67d678-6361-4f32-8396-54c6926c8f04";
const APPLY   = "5a67d678-6361-4f32-8396-54c6926c8f05";
const STATUS  = "5a67d678-6361-4f32-8396-54c6926c8f06";

/* =========================
   🔵 BLE变量
========================= */
let scanChar, listChar, ssidChar, passChar, applyChar, statusChar;

/* =========================
   🟣 MQTT变量
========================= */
let mqttClient;

/* =========================
   🧾 日志函数
========================= */
function logBLE(msg){
  document.getElementById("bleLog").textContent += msg + "\n";
}

function logOTA(msg){
  document.getElementById("otaLog").textContent += msg + "\n";
}

/* =========================
   🔵 BLE连接
========================= */
document.getElementById("connectBtn").onclick = async () => {

  try {
    logBLE("正在打开BLE设备列表...");

    const device = await navigator.bluetooth.requestDevice({
      acceptAllDevices: true,
      optionalServices: [SERVICE]
    });

    logBLE("已选择设备: " + device.name);

    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);

    scanChar  = await service.getCharacteristic(SCAN);
    listChar  = await service.getCharacteristic(LIST);
    ssidChar  = await service.getCharacteristic(SSID);
    passChar  = await service.getCharacteristic(PASS);
    applyChar = await service.getCharacteristic(APPLY);
    statusChar= await service.getCharacteristic(STATUS);

    /* ===== 状态监听 ===== */
    await statusChar.startNotifications();
    statusChar.addEventListener("characteristicvaluechanged", e=>{
      const msg = new TextDecoder().decode(e.target.value);
      logBLE("[ESP32] " + msg);
    });

    /* ===== WiFi列表监听 ===== */
    await listChar.startNotifications();
    listChar.addEventListener("characteristicvaluechanged", e=>{
      try {
        const txt = new TextDecoder().decode(e.target.value);
        const arr = JSON.parse(txt);

        const sel = document.getElementById("ssidList");
        sel.innerHTML = "";

        arr.forEach(ap=>{
          const opt = document.createElement("option");
          opt.value = ap.s;
          opt.textContent = `${ap.s} (${ap.r}dBm)`;
          sel.appendChild(opt);
        });

        logBLE("WiFi列表已更新");

      } catch(err){
        logBLE("解析WiFi列表失败: " + err);
      }
    });

    logBLE("BLE连接成功");

  } catch(err){
    logBLE("BLE错误: " + err);
  }
};

/* =========================
   📶 扫描WiFi
========================= */
document.getElementById("scanBtn").onclick = async () => {

  if(!scanChar){
    logBLE("请先连接ESP32");
    return;
  }

  await scanChar.writeValue(
    new TextEncoder().encode("1")
  );

  logBLE("开始扫描WiFi...");
};

/* =========================
   📡 WiFi配网
========================= */
document.getElementById("applyBtn").onclick = async () => {

  const ssid = document.getElementById("ssidList").value;
  const pass = document.getElementById("password").value;

  if(!ssid){
    logBLE("请选择WiFi");
    return;
  }

  await ssidChar.writeValue(new TextEncoder().encode(ssid));
  await passChar.writeValue(new TextEncoder().encode(pass));
  await applyChar.writeValue(new TextEncoder().encode("1"));

  logBLE("正在连接WiFi...");
};

/* =========================
   🟣 MQTT连接（HiveMQ WebSocket）
========================= */
document.getElementById("mqttConnectBtn").onclick = () => {

  const url  = document.getElementById("mqttUrl").value;
  const user = document.getElementById("mqttUser").value;
  const pass = document.getElementById("mqttPass").value;

  try {

    mqttClient = mqtt.connect(url, {
      username: user,
      password: pass,
      clean: true
    });

    mqttClient.on("connect", () => {
      logOTA("MQTT连接成功");
    });

    mqttClient.on("error", (err) => {
      logOTA("MQTT错误: " + err.message);
    });

    mqttClient.on("reconnect", () => {
      logOTA("MQTT重连中...");
    });

  } catch(err){
    logOTA("MQTT初始化失败: " + err);
  }
};

/* =========================
   🚀 OTA推送
========================= */
document.getElementById("otaBtn").onclick = () => {

  if(!mqttClient){
    logOTA("请先连接MQTT");
    return;
  }

  const url = document.getElementById("firmwareUrl").value;

  if(!url){
    logOTA("请输入固件URL");
    return;
  }

  const msg = {
    type: "ota",
    url: url,
    version: "1.0.0",
    time: Date.now()
  };

  mqttClient.publish(
    "esp32/update",
    JSON.stringify(msg)
  );

  logOTA("已推送OTA指令");
};