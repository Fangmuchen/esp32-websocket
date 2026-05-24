# 桌球丫 ESP32 控制器 v2.1

无人台球馆 ESP32 下位机控制器，基于 ESP-IDF v5.4.2。

## 功能

- **灯光控制** — PCF8575 #2 (0x21) P0-P7，8 路继电器，支持独立/批量控制
- **锁球器控制** — PCF8575 #1 (0x20) P0-P7，8 路
- **大门电磁锁** — GPIO26 直连继电器，远程开/关
- **台球桌电源** — GPIO27 直连继电器
- **蜂鸣器** — GPIO14，开台/续费/到期提示
- **DFPlayer 语音播报** — UART2 (GPIO17/18)，SD 卡音轨播放
- **WS2812 状态灯** — GPIO2：绿(正常) / 橙(预警) / 红(到期/离线) / 蓝(配网)
- **AP 配网** — WiFi 热点 + DNS 劫持 + 网页配网
- **WebSocket 远程控制** — WSS 连接腾讯云服务器
- **状态持久化** — 灯光/锁/门/电源状态断电保存，重启自动恢复

## 硬件设计 (PCB v2.1)

### ESP32 引脚分配

| GPIO | 功能 | 说明 |
|------|------|------|
| GPIO0 | BOOT 按键 | 上电长按 3s 恢复出厂，内部上拉 |
| GPIO1 | TX0 | USB-UART 调试串口 |
| GPIO3 | RX0 | USB-UART 调试串口 |
| GPIO2 | WS2812 LED | 智能状态指示灯 |
| GPIO21 | I2C SDA | → PCF8575 #1 + #2 |
| GPIO22 | I2C SCL | → PCF8575 #1 + #2 |
| GPIO14 | 蜂鸣器 | 有源蜂鸣器或音频触发 |
| GPIO17 | UART2 TX | → DFPlayer Mini RX |
| GPIO18 | UART2 RX | ← DFPlayer Mini TX |
| GPIO26 | 门锁继电器 | 大门电磁锁 |
| GPIO27 | 台球桌电源继电器 | 总电源控制 |
| EN | RST 按键 | 复位 |
| IO0 | 烧录模式 | RST 时保持 GND = 下载 |

### PCF8575 扩展 IO

| 芯片 | 地址 | A0/A1/A2 | P0-P7 |
|------|------|----------|-------|
| #1 锁球器 | 0x20 | GND/GND/GND | 8 路锁球器 |
| #2 灯光 | 0x21 | 3.3V/GND/GND | 8 路灯光继电器 |

驱动方式：PCF8575 → ULN2803（达林顿阵列）→ 12V 继电器
- PCF8575 输出低电平 (0) → ULN2803 导通 → 继电器吸合（灯亮/开锁）
- PCF8575 输出高电平 (1) → ULN2803 断开 → 继电器释放（灯灭/闭锁）

### PCB 连接器

| 连接器 | 规格 | 数量 | 说明 |
|--------|------|------|------|
| 电源输入 | 5.08mm 2P 接线端子 | 1 | 12V DC |
| 灯光输出 | 5.08mm 3P 接线端子 | 4 | 每路 L/N/PE，共 8 路 |
| 锁球器输出 | 5.08mm 2P 接线端子 | 4 | 每路 COM/NO，共 8 路 |
| 门锁 | 5.08mm 2P 接线端子 | 1 | COM/NO |
| 台球桌电源 | 5.08mm 2P 接线端子 | 1 | COM/NO |
| USB-C | 母座 | 1 | 供电 + 串口下载 |
| DFPlayer | 2.54mm 排母 5P | 1 | VCC/GND/TX/RX/BUSY |
| 按键 | 6×6mm 轻触 | 2 | BOOT + RST |
| LED | 5mm WS2812B | 1 | 状态指示 |

### 电源

```
12V DC ──→ 12V 继电器线圈
         ──→ LM2596 → 5V → PCF8575、ULN2803
                     ──→ AMS1117-3.3 → 3.3V → ESP32、DFPlayer
```

## 快速开始

```bash
cd /media/lucas/Data/esp32/esp32-websocket-tool
rm -rf build
unset IDF_PATH
source ~/esp/v5.4.2/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 配置 WebSocket 服务器

编辑 `main/globals.h`：

```c
#define WS_HOST   "your-server.com"
#define WS_PORT   443
#define WS_PATH   "/"
```

## 配网流程

1. ESP32 首次启动进入 AP 模式，热点 `ESP32-LED-XXXX`
2. 连接该热点，浏览器访问 `192.168.4.1`
3. 填写 WiFi 信息，设备保存后自动连接
4. 连接成功 → 连接 WebSocket → 进入在线模式

## 设备与桌台映射

每台 ESP32 管理最多 8 张桌台（通过 `numTables` 配置）。服务器使用 0-based 索引，直接映射到 PCF8575 通道。

| 服务器 lightNo/lockNo | 对应桌台 | PCF8575 通道 |
|----------------------|---------|-------------|
| 0 | 桌台 1 | P0 |
| 1 | 桌台 2 | P1 |
| 2 | 桌台 3 | P2 |
| 3 | 桌台 4 | P3 |
| 4 | 桌台 5 | P4 |
| 5 | 桌台 6 | P5 |
| 6 | 桌台 7 | P6 |
| 7 | 桌台 8 | P7 |

> `tableNo` 也使用相同的 0-based 索引。

## WebSocket 协议 v2.1

### ① 心跳上报（ESP32 → 服务器，默认 30s）

```json
{
  "type": "ping",
  "sn": "ESP32-30C6F7C681F0",
  "name": "FRH-001",
  "v": "2.0",
  "numTables": 8,
  "lt": 0,
  "lk": 0,
  "au": 0,
  "dr": 0,
  "pt": 0
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `sn` | string | 设备标识（= ESP32- + MAC 地址），用于服务器识别设备 |
| `name` | string | 设备名称（桌球室名称），默认 "FRH-001"，可远程修改 |
| `numTables` | int | 本设备管理的桌台数量，默认 8 |
| `lt` | int | 灯光状态 bitmask（0-255），bit n = 通道 n |
| `lk` | int | 锁球器状态 bitmask |
| `au` | int | 音频状态 bitmask |
| `dr` | int | 门锁：0=关 1=开 |
| `pt` | int | 台球桌电源：0=关 1=开 |

### ② 控制命令（服务器 → ESP32）

#### 按桌号控制灯光

```json
{
  "type": "setting",
  "lightNo": 1,
  "lightStatus": 1,
  "interval": 30
}
```

| 字段 | 说明 |
|------|------|
| `lightNo` | 桌台号（0-7，0-based），直接对应 PCF8575 通道 |
| `lightStatus` | 1=开灯 0=关灯 |
| `interval` | 可选，更新心跳间隔 |

#### 按桌号控制锁球器

```json
{
  "type": "setting",
  "lockNo": 1,
  "lockStatus": 1,
  "interval": 30
}
```

| 字段 | 说明 |
|------|------|
| `lockNo` | 桌台号（0-7，0-based） |
| `lockStatus` | 1=开锁 0=闭锁 |

#### v2.0 格式（兼容）

```json
{
  "type": "setting",
  "ch": {
    "lt": {"n": 0, "v": 1},
    "lk": {"n": 0, "v": 0},
    "dr": 1,
    "pt": 1,
    "lt_all": 0,
    "lk_all": 0
  }
}
```

| 字段 | 说明 |
|------|------|
| `ch.lt` | 灯光：`n`=通道(0-7, 0-based), `v`=0/1 |
| `ch.lk` | 锁球器：同上 |
| `ch.au` | 音频：同上 |
| `ch.dr` | 门锁：0/1 |
| `ch.pt` | 台球桌电源：0/1 |
| `ch.lt_all` | 批量灯光：0=全关 1=全开 |
| `ch.lk_all` | 批量锁球器：0=全关 1=全开 |

#### 设置桌台数量

```json
{
  "type": "setting",
  "numTables": 8
}
```

持久化到 NVS，重启不丢失。

#### 设置设备名称

```json
{
  "type": "setting",
  "set_name": "我的球房"
}
```

### ③ 控制回复（ESP32 → 服务器）

```json
{
  "type": "setting",
  "sn": "ESP32-30C6F7C681F0",
  "r": {"lt": 0, "lk": 0},
  "remark": ""
}
```

| 字段 | 说明 |
|------|------|
| `r` | 执行结果，`key: 0`=成功 `1`=失败 |
| `remark` | 失败原因 |

回复后立即发送一次心跳同步最新状态。

### ④ 到期预警（服务器 → ESP32）

```json
{"type": "session_warning", "tableNo": 1}
```

ESP32 响应：蜂鸣 2 声，WS2812 橙色快闪，回复 ack。

### ⑤ 到期关灯锁球（服务器 → ESP32）

```json
{"type": "session_expired", "tableNo": 1}
```

```json
{"type": "session_expired"}
```

- 携带 `tableNo`：关闭指定桌台的灯 + 锁球
- 不携带 `tableNo`：关闭所有灯 + 所有锁 + 关门

ESP32 响应：蜂鸣 3 声，WS2812 红色，**保存状态到 NVS**。

### ⑥ 语音播报（服务器 → ESP32）

```json
{"type": "voice", "file": 1}
```

| 字段 | 说明 |
|------|------|
| `file` | DFPlayer SD 卡音轨编号 (1-255) |

## 状态持久化

每次收到 `setting` 命令或 `session_expired` 事件后，ESP32 自动将灯光/锁/门/电源状态保存到 NVS（非易失性存储）。

- **保存时机**：每次控制命令执行后
- **恢复时机**：设备上电初始化时
- **擦除时机**：长按 BOOT 按键 3 秒恢复出厂设置
- **存储字段**：`dev_lt`, `dev_lk`, `dev_au`, `dev_dr`, `dev_pt`

## 项目结构

```
main/
├── main.cpp                # 入口 + 状态机
├── globals.h               # 全局硬件/网络配置
├── led_control.cpp/.h      # WS2812 驱动
├── config_manager.cpp/.h   # NVS 配置读写 + 设备状态持久化
├── wifi_manager.cpp/.h     # WiFi STA/AP
├── http_server.cpp/.h      # 配网 Web 页面
├── dns_server.cpp/.h       # DNS 劫持
├── websocket_client.cpp/.h # WebSocket 通信
├── device_control.cpp/.h   # IO 控制 + PCF8575 驱动对接
├── i2c_expander.cpp/.h     # 双 PCF8575 I2C 扩展驱动
└── voice_player.cpp/.h     # DFPlayer 语音播报
```

## License

MIT
