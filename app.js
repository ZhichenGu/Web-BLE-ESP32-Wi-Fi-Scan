/* =========================
   BLE UUID（与ESP32一致）
========================= */
const SERVICE = "5a67d678-6361-4f32-8396-54c6926c8fa9";
const SCAN    = "5a67d678-6361-4f32-8396-54c6926c8f01";
const LIST    = "5a67d678-6361-4f32-8396-54c6926c8f02";
const SSID    = "5a67d678-6361-4f32-8396-54c6926c8f03";
const PASS    = "5a67d678-6361-4f32-8396-54c6926c8f04";
const APPLY   = "5a67d678-6361-4f32-8396-54c6926c8f05";
const STATUS  = "5a67d678-6361-4f32-8396-54c6926c8f06";
const MQTT_CH = "5a67d678-6361-4f32-8396-54c6926c9001";

/* =========================
   BLE 特征引用
========================= */
let scanChar, listChar, ssidChar, passChar, applyChar, statusChar, mqttChar;

/* =========================
   MQTT 客户端
========================= */
let mqttClient = null;

/* =========================
   工具函数
========================= */
const enc = str => new TextEncoder().encode(str);
const dec = buf => new TextDecoder().decode(buf);

function logBLE(msg) {
  const el = document.getElementById("bleLog");
  el.textContent += `[${timestamp()}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
}

function logOTA(msg) {
  const el = document.getElementById("otaLog");
  el.textContent += `[${timestamp()}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
}

function timestamp() {
  return new Date().toLocaleTimeString("zh-CN", { hour12: false });
}

function setBLEStatus(connected) {
  const badge  = document.getElementById("bleStatus");
  const btns   = ["scanBtn", "applyBtn", "sendMqttBtn"];
  badge.textContent = connected ? "已连接" : "未连接";
  badge.className   = "status-badge " + (connected ? "connected" : "disconnected");
  btns.forEach(id => document.getElementById(id).disabled = !connected);
}

function setMQTTStatus(connected) {
  const badge = document.getElementById("mqttStatus");
  badge.textContent = connected ? "已连接" : "未连接";
  badge.className   = "status-badge " + (connected ? "connected" : "disconnected");
  document.getElementById("otaBtn").disabled = !connected;
}

/* =========================
   清空日志
========================= */
document.getElementById("clearBleBtn").onclick = () => {
  document.getElementById("bleLog").textContent = "";
};
document.getElementById("clearOtaBtn").onclick = () => {
  document.getElementById("otaLog").textContent = "";
};

/* =========================
   连接 BLE
========================= */
document.getElementById("connectBtn").onclick = async () => {
  if (!navigator.bluetooth) {
    logBLE("❌ 浏览器不支持 Web Bluetooth（请用 Chrome / Edge）");
    return;
  }

  try {
    logBLE("🔍 正在搜索 ESP32...");

    const device = await navigator.bluetooth.requestDevice({
      acceptAllDevices: true,
      optionalServices: [SERVICE]
    });

    logBLE(`✅ 已选择设备: ${device.name || "(无名)"}`);

    device.addEventListener("gattserverdisconnected", () => {
      logBLE("⚠️ BLE 断开连接");
      setBLEStatus(false);
    });

    const server  = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);

    scanChar   = await service.getCharacteristic(SCAN);
    listChar   = await service.getCharacteristic(LIST);
    ssidChar   = await service.getCharacteristic(SSID);
    passChar   = await service.getCharacteristic(PASS);
    applyChar  = await service.getCharacteristic(APPLY);
    statusChar = await service.getCharacteristic(STATUS);
    mqttChar   = await service.getCharacteristic(MQTT_CH);

    /* ===== 订阅 ESP32 状态通知 ===== */
    await statusChar.startNotifications();
    statusChar.addEventListener("characteristicvaluechanged", e => {
      logBLE("📡 ESP32: " + dec(e.target.value));
    });

    /* ===== 订阅 WiFi 列表通知 ===== */
    await listChar.startNotifications();
    listChar.addEventListener("characteristicvaluechanged", e => {
      try {
        const arr = JSON.parse(dec(e.target.value));
        const sel = document.getElementById("ssidList");
        sel.innerHTML = "";
        arr.forEach(ap => {
          const opt = document.createElement("option");
          opt.value       = ap.s;
          opt.textContent = `${ap.s}  (${ap.r} dBm)`;
          sel.appendChild(opt);
        });
        logBLE(`📶 收到 ${arr.length} 个 WiFi`);
      } catch (err) {
        logBLE("❌ WiFi 列表解析失败: " + err);
      }
    });

    setBLEStatus(true);
    logBLE("✅ BLE 全部特征就绪");

  } catch (err) {
    logBLE("❌ BLE 连接失败: " + err);
    setBLEStatus(false);
  }
};

/* =========================
   扫描 WiFi
========================= */
document.getElementById("scanBtn").onclick = async () => {
  if (!scanChar) { logBLE("❌ 未连接 ESP32"); return; }
  try {
    await scanChar.writeValue(enc("1"));
    logBLE("➡️ 扫描指令已发送");
  } catch (err) {
    logBLE("❌ 扫描失败: " + err);
  }
};

/* =========================
   连接 WiFi
========================= */
document.getElementById("applyBtn").onclick = async () => {
  if (!ssidChar) { logBLE("❌ 未连接 ESP32"); return; }

  const ssid = document.getElementById("ssidList").value;
  const pass = document.getElementById("password").value;

  if (!ssid) { logBLE("❌ 请先选择 SSID"); return; }

  try {
    await ssidChar .writeValue(enc(ssid));
    await passChar .writeValue(enc(pass));
    await applyChar.writeValue(enc("1"));
    logBLE(`➡️ WiFi 连接指令已发送: ${ssid}`);
  } catch (err) {
    logBLE("❌ 发送 WiFi 配置失败: " + err);
  }
};

/* =========================
   下发 MQTT 配置（格式：host|port|user|pass）
========================= */
document.getElementById("sendMqttBtn").onclick = async () => {
  if (!mqttChar) { logBLE("❌ 未连接 ESP32"); return; }

  const host = document.getElementById("mqttHost").value.trim();
  const port = document.getElementById("mqttPort").value.trim() || "1883";
  const user = document.getElementById("mqttUser").value.trim();
  const pass = document.getElementById("mqttPassInput").value;

  if (!host) { logBLE("❌ 请填写 Broker 地址"); return; }
  // 格式与ESP32 parseMQTTConfig 对应：host|port|user|pass
  const payload = `${host}|${port}|${user}|${pass}`;

  try {
    await mqttChar.writeValue(enc(payload));
    logBLE(`✅ MQTT 配置已下发: ${host}:${port}`);
  } catch (err) {
    logBLE("❌ MQTT 配置发送失败: " + err);
  }
};

/* =========================
   浏览器端 MQTT 连接（用于 OTA 推送）
========================= */
document.getElementById("mqttConnectBtn").onclick = () => {
  const url  = document.getElementById("mqttWsUrl").value.trim();
  const user = document.getElementById("mqttWsUser").value.trim();
  const pass = document.getElementById("mqttWsPass").value;

  if (!url) { logOTA("❌ 请填写 WSS 地址"); return; }

  if (mqttClient && mqttClient.connected) {
    mqttClient.end();
    logOTA("⚠️ 已断开旧 MQTT 连接");
  }

  logOTA(`🔗 正在连接: ${url}`);

  const opts = {};
  if (user) opts.username = user;
  if (pass) opts.password = pass;

  mqttClient = mqtt.connect(url, opts);

  mqttClient.on("connect", () => {
    logOTA("✅ MQTT 已连接");
    setMQTTStatus(true);
  });

  mqttClient.on("error", e => {
    logOTA("❌ MQTT 错误: " + e.message);
    setMQTTStatus(false);
  });

  mqttClient.on("close", () => {
    logOTA("⚠️ MQTT 连接关闭");
    setMQTTStatus(false);
  });

  mqttClient.on("message", (topic, message) => {
    logOTA(`📩 [${topic}] ${message.toString()}`);
  });
};

/* =========================
   推送 OTA 指令
========================= */
document.getElementById("otaBtn").onclick = () => {
  if (!mqttClient || !mqttClient.connected) {
    logOTA("❌ MQTT 未连接");
    return;
  }

  const url = document.getElementById("firmwareUrl").value.trim();
  if (!url) { logOTA("❌ 请填写固件 URL"); return; }

  const payload = JSON.stringify({ url });
  mqttClient.publish("esp32/update", payload);
  logOTA(`🚀 OTA 指令已发送: ${url}`);
};

/* =========================
   初始化状态
========================= */
setBLEStatus(false);
setMQTTStatus(false);
