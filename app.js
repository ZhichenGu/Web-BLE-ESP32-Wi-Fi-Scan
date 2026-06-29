/* =========================
   BLE UUID（与硬件一致）
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
   BLE 变量（全局保存设备引用）
========================= */
let gattDevice = null;          // 用于断开旧连接
let scanChar, listChar, ssidChar, passChar, applyChar, statusChar, mqttChar;

/* =========================
   MQTT 客户端
========================= */
let mqttClient;

/* =========================
   日志函数
========================= */
function logBLE(msg) {
  document.getElementById("bleLog").textContent += msg + "\n";
}

function logOTA(msg) {
  document.getElementById("otaLog").textContent += msg + "\n";
}

/* =========================
   连接 BLE（带强制断开旧连接）
========================= */
document.getElementById("connectBtn").onclick = async () => {
  try {
    // 🔁 1. 如果之前已连接，先主动断开
    if (gattDevice && gattDevice.gatt.connected) {
      logBLE("🔌 断开旧连接...");
      await gattDevice.gatt.disconnect();
      gattDevice = null;          // 清空引用
      // 等待片刻确保底层释放
      await new Promise(r => setTimeout(r, 300));
    }

    logBLE("🔍 搜索 ESP32...");

    // 2. 重新请求设备（强制浏览器重新扫描）
    const device = await navigator.bluetooth.requestDevice({
      acceptAllDevices: true,
      optionalServices: [SERVICE]
    });

    gattDevice = device;   // 保存全局引用，以便下次断开
    logBLE("✅ 已选择: " + (device.name || "未知设备"));

    // 3. 连接 GATT
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);

    // 4. 获取所有特征
    scanChar  = await service.getCharacteristic(SCAN);
    listChar  = await service.getCharacteristic(LIST);
    ssidChar  = await service.getCharacteristic(SSID);
    passChar  = await service.getCharacteristic(PASS);
    applyChar = await service.getCharacteristic(APPLY);
    statusChar= await service.getCharacteristic(STATUS);
    mqttChar  = await service.getCharacteristic(MQTT);

    // 5. 开启状态通知
    await statusChar.startNotifications();
    statusChar.addEventListener("characteristicvaluechanged", e => {
      const msg = new TextDecoder().decode(e.target.value);
      logBLE("📡 ESP32: " + msg);
    });

    // 6. 开启 WiFi 列表通知
    await listChar.startNotifications();
    listChar.addEventListener("characteristicvaluechanged", e => {
      const data = new TextDecoder().decode(e.target.value);
      const arr = JSON.parse(data);
      const sel = document.getElementById("ssidList");
      sel.innerHTML = "";
      arr.forEach(ap => {
        const opt = document.createElement("option");
        opt.value = ap.s;
        opt.textContent = `${ap.s} (${ap.r})`;
        sel.appendChild(opt);
      });
      logBLE("📶 WiFi 列表已更新");
    });

    logBLE("✅ BLE 就绪");

  } catch (err) {
    logBLE("❌ BLE 错误: " + err.message || err);
  }
};

/* =========================
   扫描 WiFi
========================= */
document.getElementById("scanBtn").onclick = async () => {
  if (!scanChar) {
    logBLE("❌ 未连接 ESP32");
    return;
  }
  try {
    await scanChar.writeValue(new TextEncoder().encode("1"));
    logBLE("➡️ 扫描指令已发送");
  } catch (err) {
    logBLE("❌ 扫描失败: " + err.message);
  }
};

/* =========================
   连接 WiFi
========================= */
document.getElementById("applyBtn").onclick = async () => {
  if (!ssidChar || !passChar || !applyChar) {
    logBLE("❌ BLE 未就绪");
    return;
  }
  const ssid = document.getElementById("ssidList").value;
  const pass = document.getElementById("password").value;
  try {
    await ssidChar.writeValue(new TextEncoder().encode(ssid));
    await passChar.writeValue(new TextEncoder().encode(pass));
    await applyChar.writeValue(new TextEncoder().encode("1"));
    logBLE("➡️ WiFi 配置已下发");
  } catch (err) {
    logBLE("❌ 下发失败: " + err.message);
  }
};

/* =========================
   下发 MQTT 配置
========================= */
document.getElementById("sendMqttBtn").onclick = async () => {
  if (!mqttChar) {
    logBLE("❌ BLE 未就绪");
    return;
  }
  const url  = document.getElementById("mqttUrl").value;
  const user = document.getElementById("mqttUser").value;
  const pass = document.getElementById("mqttPass").value;
  const payload = `${url}|8883|${user}|${pass}`;
  try {
    await mqttChar.writeValue(new TextEncoder().encode(payload));
    logBLE("✅ MQTT 配置已下发");
  } catch (err) {
    logBLE("❌ 下发失败: " + err.message);
  }
};

/* =========================
   MQTT 连接（网页端）
========================= */
document.getElementById("mqttConnectBtn").onclick = () => {
  const url  = document.getElementById("mqttUrl").value;
  const user = document.getElementById("mqttUser").value;
  const pass = document.getElementById("mqttPass").value;
  if (!url) {
    logOTA("❌ 请输入 MQTT Broker 地址");
    return;
  }
  try {
    mqttClient = mqtt.connect(url, {
      username: user,
      password: pass
    });
    mqttClient.on("connect", () => {
      logOTA("✅ MQTT 已连接");
    });
    mqttClient.on("error", (e) => {
      logOTA("❌ MQTT 错误: " + e.message);
    });
  } catch (e) {
    logOTA("❌ 连接异常: " + e.message);
  }
};

/* =========================
   推送 OTA
========================= */
document.getElementById("otaBtn").onclick = () => {
  const url = document.getElementById("firmwareUrl").value;
  if (!url) {
    logOTA("❌ 请输入固件 URL");
    return;
  }
  if (!mqttClient || !mqttClient.connected) {
    logOTA("❌ MQTT 未连接，请先连接 MQTT");
    return;
  }
  try {
    mqttClient.publish("esp32/update", JSON.stringify({ url: url }));
    logOTA("✅ OTA 指令已发送");
  } catch (err) {
    logOTA("❌ 发送失败: " + err.message);
  }
};