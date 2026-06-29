// ===== ESP32 UUID（必须和你的代码一致） =====
const SERVICE = "5a67d678-6361-4f32-8396-54c6926c8fa9";

const SCAN    = "5a67d678-6361-4f32-8396-54c6926c8f01";
const LIST    = "5a67d678-6361-4f32-8396-54c6926c8f02";
const SSID    = "5a67d678-6361-4f32-8396-54c6926c8f03";
const PASS    = "5a67d678-6361-4f32-8396-54c6926c8f04";
const APPLY   = "5a67d678-6361-4f32-8396-54c6926c8f05";
const STATUS  = "5a67d678-6361-4f32-8396-54c6926c8f06";

// ===== BLE变量 =====
let server;
let service;
let scanChar, listChar, ssidChar, passChar, applyChar, statusChar;

// ===== UI =====
const logBox = document.getElementById("log");

function log(msg){
  logBox.textContent += msg + "\n";
}

// ======================
// 连接 ESP32（关键修复点）
// ======================
document.getElementById("connectBtn").onclick = async () => {

  try {
    log("正在搜索 ESP32...");

    // ⚠️ 关键：必须用 acceptAllDevices 才一定弹框
    const device = await navigator.bluetooth.requestDevice({
      acceptAllDevices: true,
      optionalServices: [SERVICE]
    });

    log("已选择设备: " + device.name);

    server = await device.gatt.connect();
    service = await server.getPrimaryService(SERVICE);

    scanChar   = await service.getCharacteristic(SCAN);
    listChar   = await service.getCharacteristic(LIST);
    ssidChar   = await service.getCharacteristic(SSID);
    passChar   = await service.getCharacteristic(PASS);
    applyChar  = await service.getCharacteristic(APPLY);
    statusChar = await service.getCharacteristic(STATUS);

    // 状态监听
    await statusChar.startNotifications();
    statusChar.addEventListener("characteristicvaluechanged", e => {
      const msg = new TextDecoder().decode(e.target.value);
      log("[ESP32] " + msg);
    });

    // WiFi列表监听
    await listChar.startNotifications();
    listChar.addEventListener("characteristicvaluechanged", e => {
      const txt = new TextDecoder().decode(e.target.value);
      const arr = JSON.parse(txt);

      const list = document.getElementById("ssidList");
      list.innerHTML = "";

      arr.forEach(ap => {
        const opt = document.createElement("option");
        opt.value = ap.s;
        opt.textContent = `${ap.s} (${ap.r})`;
        list.appendChild(opt);
      });

      log("WiFi列表已更新");
    });

    log("BLE 连接成功");

  } catch (err) {
    log("错误: " + err);
  }
};

// ======================
// 扫描 WiFi
// ======================
document.getElementById("scanBtn").onclick = async () => {
  if (!scanChar) return log("请先连接ESP32");

  await scanChar.writeValue(
    new TextEncoder().encode("1")
  );

  log("开始扫描 WiFi...");
};

// ======================
// 连接 WiFi
// ======================
document.getElementById("applyBtn").onclick = async () => {

  const ssid = document.getElementById("ssidList").value;
  const pass = document.getElementById("password").value;

  if (!ssid) return log("请选择WiFi");

  await ssidChar.writeValue(new TextEncoder().encode(ssid));
  await passChar.writeValue(new TextEncoder().encode(pass));
  await applyChar.writeValue(new TextEncoder().encode("1"));

  log("正在连接 WiFi...");
};