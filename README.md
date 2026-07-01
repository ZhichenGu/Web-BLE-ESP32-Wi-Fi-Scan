# ESP32 IoT 配网 / MQTT / OTA 控制台

通过浏览器使用 Web Bluetooth 给 ESP32 配置 WiFi 和 MQTT，并通过 MQTT 远程推送 OTA 固件升级。

## 项目结构

```
.
├── esp32_iot.ino   # ESP32 固件源码（BLE Server + WiFi + MQTT + OTA）
├── index.html      # 控制台页面
├── style.css       # 样式
└── app.js          # 浏览器端逻辑（Web Bluetooth + MQTT.js）
```

## 功能概览

- **BLE 配网**：浏览器通过 Web Bluetooth 扫描并连接 ESP32，下发 WiFi SSID/密码
- **MQTT 配置下发**：通过 BLE 把 MQTT Broker 地址、端口、账号密码发给 ESP32，ESP32 保存到 NVS（断电不丢失）
- **OTA 升级**：浏览器连接 MQTT Broker，发布固件 URL 到指定 topic，ESP32 订阅后自动下载并升级

## 一、硬件 / 环境要求

- ESP32 开发板（需支持 BLE，大多数 ESP32 都支持）
- Arduino IDE（已安装 ESP32 开发板支持包）
- 已安装库：`PubSubClient`、`ArduinoHttpClient`（HTTPUpdate 内置于 ESP32 核心库）
- 一个 MQTT Broker，本项目以 **HiveMQ Cloud** 为例
- 浏览器需支持 Web Bluetooth（**Chrome / Edge**，且需要 HTTPS 或 localhost 环境）

## 二、烧录前必须确认的 Arduino IDE 设置

`工具（Tools）` 菜单中：

| 设置项 | 要求 |
|--------|------|
| Board | 与你的 ESP32 型号一致 |
| Partition Scheme | **必须包含 OTA 分区**，例如 "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)"，**不要选 Huge APP（无 OTA 分区）** |
| Flash Size | 与板子实际容量一致 |

⚠️ 首次烧录时使用的 Partition Scheme，决定了 flash 上是否预留了 OTA 分区。如果当初选的是无 OTA 方案，后续无论怎么编译都无法 OTA，需要重新整体擦除并烧录。

## 三、烧录固件

1. 用 Arduino IDE 打开 `esp32_iot.ino`
2. 确认上方的板卡设置无误
3. USB 连接 ESP32，选择正确的端口，点击上传

首次烧录后，串口监视器（115200 波特率）应看到：

```
BLE READY
========== BLE STRUCT CHECK ==========
SSID_CHAR   OK
PASS_CHAR   OK
APPLY_CHAR  OK
MQTT_CHAR   OK
STATUS_CHAR OK
SCAN_CHAR   OK
LIST_CHAR   OK
======================================
```

## 四、使用流程

### 1. 打开控制台页面

`index.html` / `style.css` / `app.js` 放在同一目录下，用本地 HTTPS 服务器或 `localhost` 打开（Web Bluetooth 要求安全上下文，直接双击打开 `file://` 可能无法使用）。

### 2. BLE 配网

1. 点击「连接 ESP32」，浏览器弹出设备选择框，选择 `ESP32-Setup`
2. 点击「扫描 WiFi」，下拉框会出现附近的 WiFi 列表
3. 选择 WiFi，输入密码，点击「连接 WiFi」
4. ESP32 反馈日志区会显示 `wifi_connecting` → `wifi_ok`

### 3. MQTT 配置下发（通过 BLE）

ESP32 端使用 **TLS 加密连接（端口 8883）**，因此：

| 字段 | 填写内容（以 HiveMQ Cloud 为例） |
|------|-----------------------------------|
| Broker 地址 | `xxxxxxx.s1.eu.hivemq.cloud`（裸域名，不要加 `wss://`） |
| 端口 | `8883` |
| 用户名 / 密码 | HiveMQ Cloud **Access Management → Credentials** 里单独创建的 MQTT 账号 |

点击「下发 MQTT 配置」，ESP32 反馈日志区应显示 `mqtt_saved`，随后 ESP32 会在 5 秒内自动尝试连接 Broker，串口监视器应看到 `MQTT connected`。

### 4. 浏览器端连接 MQTT（用于推送 OTA）

浏览器只能用 **WebSocket + TLS（端口 8884）**，格式：

```
wss://xxxxxxx.s1.eu.hivemq.cloud:8884/mqtt
```

填写完整 URL（含 `wss://` 前缀和 `/mqtt` 路径），输入同一组用户名密码，点击「连接 MQTT」，状态徽章变绿即为成功。

### 5. 编译并导出固件

1. Arduino IDE 修改代码后，点击 **项目 → 导出已编译的二进制文件**（Sketch → Export Compiled Binary）
2. 在 sketch 文件夹中找到 `esp32_iot.ino.bin`（只需要这一个文件，`.elf`/`.map`/`bootloader.bin`/`partitions.bin` 均不需要）
3. 重命名为 `firmware.bin`

### 6. 上传固件并获取直链

1. 上传 `firmware.bin` 到 GitHub 仓库（**仓库需为 Public**，否则 ESP32 无法直接下载）
2. 打开该文件页面，点击 **Raw** 按钮，复制地址栏链接，格式如下：

```
https://raw.githubusercontent.com/用户名/仓库名/main/firmware.bin
```

⚠️ 必须用 `raw.githubusercontent.com` 链接，普通 `github.com/.../blob/...` 页面返回的是 HTML，会导致 OTA 失败。

### 7. 推送 OTA

1. 在「固件 URL」输入框填入上一步获取的 raw 链接
2. 点击「推送 OTA 指令」
3. ESP32 通过已订阅的 MQTT topic（`esp32/update`）收到指令，串口监视器应依次显示：

```
[MQTT] topic=esp32/update msg={"url":"https://raw.githubusercontent.com/..."}
[ESP32] OTA start: https://raw.githubusercontent.com/...
[ESP32] OTA success - rebooting
```

升级成功后 ESP32 会自动重启，运行新固件。

## 五、BLE 特征 UUID 对照表

| 名称 | UUID | 属性 | 说明 |
|------|------|------|------|
| Service | `5a67d678-6361-4f32-8396-54c6926c8fa9` | - | 主服务 |
| WiFi SSID | `...8f03` | Write | WiFi 名称 |
| WiFi 密码 | `...8f04` | Write | WiFi 密码 |
| Apply WiFi | `...8f05` | Write | 写 `1` 触发连接 |
| MQTT 配置 | `...9001` | Write | 格式 `host\|port\|user\|pass` |
| Status | `...8f06` | Notify | ESP32 状态反馈 |
| Scan | `...8f01` | Write | 写 `1` 触发 WiFi 扫描 |
| WiFi 列表 | `...8f02` | Notify | 扫描结果（JSON） |

## 六、常见问题排查

**BLE 连接后特征获取失败 / 部分特征 MISSING**
检查串口监视器的 BLE STRUCT CHECK 输出，若有 MISSING，多为 `createService()` 的 handle 数量不足，本项目已设为 32，一般够用，特征更多时需调大。

**MQTT 报错 `Connection refused: Not authorized`**
用户名或密码错误，或该账号没有对应 topic 的权限。需在 HiveMQ Cloud 控制台 **Access Management** 检查凭据与权限。

**浏览器 MQTT 一直显示"未连接"且无报错**
检查 URL 格式，必须是完整的 `wss://域名:8884/mqtt`，缺少协议头、端口或路径都会导致连接静默失败。

**OTA 失败 / 设备无响应**
- 确认固件 URL 是 `raw.githubusercontent.com` 而不是网页链接
- 确认仓库为 Public
- 确认烧录固件时的 Partition Scheme 包含 OTA 分区
- 确认固件文件确实是最新编译版本（非 .elf/.map 等其他产物）

**ESP32 收不到 OTA 指令**
确认 ESP32 已成功连接 MQTT（串口监视器有 `MQTT connected`），且浏览器和 ESP32 用的是同一个 Broker 域名、同一个用户名密码。两端必须先各自连上同一个 Broker，浏览器发布的消息才能被 ESP32 收到。

## 七、安全提示

- 当前 ESP32 端 TLS 使用 `setInsecure()` 跳过证书校验，便于快速联调。生产环境建议改为校验 HiveMQ 提供的 CA 证书，避免中间人攻击
- MQTT 账号密码、固件下载地址等信息会通过 BLE 明文传输，仅建议在可信环境下使用
