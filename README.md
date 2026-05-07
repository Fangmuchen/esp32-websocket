# ESP32 WebSocket LED Controller

基于 ESP-IDF 的 WS2812 LED 控制器，支持 WebSocket 远程控制和 AP 配网。

## 功能特性

- **WS2812 灯控** — 通过 RMT 驱动 WS2812 LED，支持颜色设置和闪烁提示
- **AP 配网** — ESP32 启动 AP 热点，用户通过手机/电脑连接后访问 Web 页面配置 WiFi
- **WebSocket 远程控制** — 连接远程 WebSocket 服务端，接收设备控制指令
- **设备控制** — 支持 8 路灯光、8 路音频通道控制，以及台球桌模式
- **配置持久化** — WiFi 凭据等信息保存到 NVS，下次启动自动连接
- **看门狗超时** — AP 模式 5 分钟无操作自动重启
- **恢复出厂** — 上电时按住 BOOT 键 3 秒清除配置
- **默认凭据** — 内置默认 SSID/密码 (`esp32` / `123456789012`)

## 硬件要求

- ESP32 开发板
- WS2812 LED（或兼容的 RGB LED）
- 5V 电源（根据 LED 数量确定功率）

## 引脚定义

| 引脚 | 功能 |
|------|------|
| GPIO2 | WS2812 数据输出 |
| GPIO0 | BOOT 按键（上电按住 3 秒恢复出厂） |

## 软件依赖

- [ESP-IDF](https://github.com/espressif/esp-idf) v5.x
- 组件依赖（通过 idf_component.yml 管理）：
  - nvs_flash
  - esp_wifi / esp_netif / esp_event
  - esp_http_server
  - esp_websocket_client
  - cJSON
  - esp_driver_rmt

## 快速开始

### 1. 设置 ESP-IDF 环境

```bash
. $IDF_PATH/export.sh
```

### 2. 配置项目

```bash
idf.py menuconfig
```

或在 `main/globals.h` 中修改默认配置：

- `WS_HOST` / `WS_PORT` — WebSocket 服务器地址
- `AP_SSID_PREFIX` — AP 热点名称前缀
- `DEFAULT_SSID` / `DEFAULT_PASS` — 默认 WiFi 凭据

### 3. 编译 & 烧录

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 配网流程

1. ESP32 首次启动进入 AP 模式，热点名称为 `ESP32-LED-XXXX`
2. 连接该热点，浏览器访问 `192.168.4.1`
3. 在页面中输入 WiFi SSID 和密码
4. ESP32 连接 WiFi 成功后自动连接 WebSocket 服务器
5. 蓝色 LED 闪烁表示配网状态，常亮表示在线

## WebSocket 协议

设备连接 WebSocket 服务器后发送 `hello` 消息进行注册：

```json
{
  "cmd": "hello",
  "sn": "设备序列号",
  "type": "led_controller"
}
```

控制指令格式：

```json
{
  "cmd": "setting",
  "light": [true, false, ...],
  "audio": [true, false, ...],
  "pool_table": true
}
```

## 项目结构

```
main/
├── CMakeLists.txt          # 组件构建配置
├── main.cpp                # 入口 + 状态机
├── globals.h               # 全局硬件/网络配置
├── led_control.cpp/.h      # WS2812/板载 LED 控制
├── config_manager.cpp/.h   # NVS 配置读写
├── wifi_manager.cpp/.h     # WiFi STA/AP 管理
├── http_server.cpp/.h      # 配网 Web 服务
├── dns_server.cpp/.h       # DNS 劫持（配网用）
├── websocket_client.cpp/.h # WebSocket 客户端
├── device_control.cpp/.h   # 灯光/音频/台球桌控制
└── idf_component.yml       # 组件清单
```

## License

MIT
