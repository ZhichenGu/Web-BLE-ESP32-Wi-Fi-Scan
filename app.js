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

/* =========================
   BLE 特征引用
========================= */
let scanChar, listChar, ssidChar, passChar, applyChar, statusChar;

/* =========================
   工具函数
========================= */
const enc = str => new TextEncoder().encode(str);
const dec = buf => new TextDecoder().decode(buf);

function timestamp() {
  return new Date().toLocaleTimeString("zh-CN", { hour12: false });
}

function logBLE(msg) {
  const el = document.getElementById("bleLog");
  el.textContent += `[${timestamp()}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
}

function setBLEStatus(connected) {
  const badge = document.getElementById("bleStatus");
  badge.textContent = connected ? "已连接" : "未连接";
  badge.className = "status-badge " + (connected ? "connected" : "disconnected");
  document.getElementById("scanBtn").disabled  = !connected;
  document.getElementById("applyBtn").disabled = !connected;
}

/* =========================
   清空日志
========================= */
document.getElementById("clearBtn").onclick = () => {
  document.getElementById("bleLog").textContent = "";
};

/* =========================
   连接 BLE
========================= */
document.getElementById("connectBtn").onclick = async () => {
  if (!navigator.bluetooth) {
    logBLE("❌ 浏览器不支持 Web Bluetooth（请用 Chrome 或 Edge）");
    return;
  }

  try {
    logBLE("🔍 正在搜索 ESP32...");

    const device = await navigator.bluetooth.requestDevice({
      acceptAllDevices: true,
      optionalServices: [SERVICE]
    });

    logBLE(`✅ 已选择: ${device.name || "(无名)"}`);

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

    /* 订阅 ESP32 状态通知 */
    await statusChar.startNotifications();
    statusChar.addEventListener("characteristicvaluechanged", e => {
      logBLE("📡 ESP32: " + dec(e.target.value));
    });

    /* 订阅 WiFi 列表通知 */
    await listChar.startNotifications();
    listChar.addEventListener("characteristicvaluechanged", e => {
      try {
        const arr = JSON.parse(dec(e.target.value));
        const sel = document.getElementById("ssidList");
        sel.innerHTML = "";
        arr.forEach(ap => {
          const opt = document.createElement("option");
          opt.value = ap.s;
          opt.textContent = `${ap.s}  (${ap.r} dBm)`;
          sel.appendChild(opt);
        });
        logBLE(`📶 发现 ${arr.length} 个 WiFi`);
      } catch (err) {
        logBLE("❌ WiFi 列表解析失败: " + err);
      }
    });

    setBLEStatus(true);
    logBLE("✅ BLE 就绪，可以开始扫描 WiFi");

  } catch (err) {
    logBLE("❌ 连接失败: " + err);
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
    logBLE("➡️ 扫描指令已发送，请稍候...");
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

  if (!ssid) { logBLE("❌ 请先选择 WiFi"); return; }

  try {
    await ssidChar .writeValue(enc(ssid));
    await passChar .writeValue(enc(pass));
    await applyChar.writeValue(enc("1"));
    logBLE(`➡️ WiFi 连接指令已发送: ${ssid}`);
    logBLE("⏳ 等待 ESP32 反馈...");
  } catch (err) {
    logBLE("❌ 发送失败: " + err);
  }
};

/* =========================
   初始化
========================= */
setBLEStatus(false);
