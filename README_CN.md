# BLE-KVM

[English](README.md) | [中文](README_CN.md)

基于 ESP32-S3 的蓝牙 KVM（键盘-视频-鼠标）切换器。通过 BLE HID 同时连接最多三台电脑，一键切换键盘/鼠标输入。支持 USB 设备模式、USB 主机模式、WiFi Web 配置和云端语音转文字输入。

## 支持的开发板

| 开发板 | 语音输入 | TFT屏幕 | 电池监测 | USB | IMU | LED |
|--------|---------|---------|---------|-----|-----|-----|
| **M5StickS3** | ✅ | ✅ | ✅ | ✅ | ✅ | 无（背光指示） |
| **M5StampS3** | ❌ | ❌ | ❌ | ✅ | ❌ | RGB (WS2812) |
| **通用 ESP32-S3** | ❌ | ❌ | ❌ | ❌ | ❌ | GPIO LED |

## 功能特性

- **BLE HID KVM** — 通过蓝牙连接最多 3 台电脑。按下按钮即可切换当前控制的主机。键盘和鼠标输入透明路由到当前选中的连接。
- **USB 设备模式** — 通过 USB OTG 端口，以 USB HID 键盘/鼠标设备身份连接到主机。
- **USB 主机模式** — 接受外部 USB HID 键盘/鼠标，将其输入通过 BLE 转发给已连接的电脑。

### M5StickS3 专属

- **云端语音输入** — 长按主按钮开始录音，音频通过 WebSocket 实时传输至[百度语音识别](https://ai.baidu.com/tech/speech/asr)服务。识别结果以 HID 键盘输入方式键入当前活动电脑。
- **TFT 显示屏** — 135×240 彩色屏幕，显示当前活动电脑、连接状态、电池电量、输入模式、录音中状态和恢复出厂设置警告。
- **IMU 空中鼠标** — 6 轴 IMU 支持空中鼠标 / PPT 演示指针模式。按副按钮循环切换输入模式。
- **电池监测** — 实时电量百分比和充电状态（AXP2101 PMIC）。

### 全平台通用

- **WiFi Web 配置** — 内置 Web 服务器（支持 STA 或 AP 模式），可配置 WiFi、已配对电脑、USB 模式、防空闲、语音识别参数等。通过可配置的认证令牌保护。
- **双按钮操作** — 主按钮：短按切换电脑，长按启动语音输入。副按钮：短按切换输入模式，长按 5 秒提示恢复出厂，长按 10 秒执行。
- **防空闲** — 定期发送 HID 保活信号，防止主机休眠或锁屏。
- **Web 调试日志** — 通过 SSE 在 Web 仪表板上实时查看 ESP-IDF 日志输出。默认关闭，可在设置中开启。
- **恢复出厂设置** — 通过副按钮长按 10 秒或 `POST /api/factory-reset` 接口（需 `{"confirm": true}`）。清除所有 NVS 设置并重启。

## 环境准备

- **ESP-IDF v5.5** — [安装指南](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html)
- **Python 3.10+**
- **ESP32-S3 开发板** — 推荐 M5StickS3 以获得全部功能

### ESP-IDF 组件依赖

定义在 `main/idf_component.yml` 中：

| 组件 | 版本 | 用途 |
|------|------|------|
| `espressif/esp_tinyusb` | ^2.0.0 | USB 设备协议栈 |
| `espressif/usb_host_hid` | ^1.0.1 | USB 主机 HID 驱动 |
| `espressif/esp_websocket_client` | ^1.7.0 | 语音识别的 WebSocket 客户端 |

### M5StickS3 I2C 总线

M5StickS3 使用共享 I2C 总线（GPIO47=SDA，GPIO48=SCL），连接三个外设：

| 设备 | I2C 地址 | 驱动文件 |
|------|---------|---------|
| AXP2101 电源管理 | 0x6E | `power_manager.c` |
| BMI270 惯性测量 | 0x68 | `imu_driver.c` |
| ES8311 音频编解码 | 0x18 | `es8311_driver.c` |

## 构建与烧录

### 1. 克隆仓库并初始化组件

```bash
git clone <repo-url> && cd ble-kvm
```

`managed_components/` 目录已加入 .gitignore —— ESP-IDF 会在构建时自动获取依赖。

### 2. 配置 ESP-IDF 环境

```bash
. $HOME/esp/esp-idf/export.sh
```

### 3. 选择开发板构建

```bash
# M5StickS3（全部功能）
BOARD=m5sticks3 idf.py build

# M5StampS3（BLE + USB + RGB LED）
BOARD=m5stamps3 idf.py build

# 通用 ESP32-S3（BLE + GPIO LED）
BOARD=default idf.py build
```

可将 `BOARD` 设为环境变量，免去每次指定：

```bash
export BOARD=m5sticks3
```

### 4. 烧录固件

```bash
idf.py -p /dev/ttyACM0 flash
```

## 项目结构

```
ble-kvm/
├── main/                    # ESP-IDF 主组件
│   ├── main.c               # 应用入口，初始化顺序
│   ├── board.h              # 各板卡引脚映射及功能开关定义
│   ├── config_manager.h/c   # 基于 NVS 的配置管理（WiFi、电脑、语音等）
│   ├── ble_peripheral.h/c   # BLE HID 外设广播与连接管理
│   ├── ble_central.h/c      # BLE 中心设备扫描与连接
│   ├── hid_router.h/c       # 键盘/鼠标报文路由到当前活动电脑
│   ├── switch_manager.h/c   # 按钮处理、电脑切换、语音/恢复出厂触发
│   ├── wifi_manager.h/c     # WiFi AP + STA 管理
│   ├── web_server.h/c       # HTTP API + SSE + 嵌入式 Web 仪表板
│   ├── web_log.h/c          # vprintf 钩子 → SSE 日志广播
│   ├── indicator.h/c        # LED/TFT 状态管理
│   ├── anti_idle.h/c        # 防空闲 HID 保活
│   ├── gpio_led.h/c         # GPIO LED 驱动（通用板）
│   ├── rgb_led.h/c          # WS2812 RGB LED 驱动（StampS3）
│   ├── tft_display.h/c      # ST7789 TFT 屏幕驱动（M5StickS3）
│   ├── power_manager.h/c    # AXP2101 PMIC 电池/充电管理（M5StickS3）
│   ├── input_mode.h/c       # KVM ↔ 空中鼠标模式切换（M5StickS3）
│   ├── imu_driver.h/c       # BMI270 IMU 驱动（M5StickS3）
│   ├── es8311_driver.h/c    # ES8311 音频编解码器驱动（M5StickS3）
│   ├── mic_driver.h/c       # I2S MEMS 麦克风驱动（M5StickS3）
│   ├── voice_input.h/c      # 语音录音 + 百度 ASR WebSocket 客户端
│   ├── usb_device.h/c       # TinyUSB HID 设备
│   ├── usb_host.h/c         # USB 主机 HID 驱动
│   ├── CMakeLists.txt       # 组件构建脚本（按板卡条件编译源文件）
│   └── idf_component.yml    # ESP-IDF 组件依赖声明
├── web/                     # React Web 仪表板（Vite + TypeScript）
│   ├── src/
│   ├── index.html
│   ├── vite.config.ts
│   └── package.json
├── CMakeLists.txt           # 根项目定义
├── sdkconfig                # 默认 SDK 配置
├── dependencies.lock        # ESP-IDF 组件锁定文件
├── README.md                # 英文文档
├── README_CN.md             # 本文档（中文）
└── .gitignore
```

## 配置参数

所有设置均持久化存储在 NVS 闪存中，可通过 Web 仪表板 `http://<设备IP>/` 进行配置。

| 设置项 | 默认值 | 说明 |
|--------|-------|------|
| `auth_token` | 9 位随机字符 | Web 界面认证令牌 |
| `wifi_ssid` / `wifi_password` | — | STA 模式连接凭证 |
| `wifi_enabled` | false | 启动时是否开启 WiFi |
| `usb_mode` | 0（禁用） | 0=仅 BLE，1=USB 设备，2=USB 主机 |
| `active_pc` | 0 | 当前选中的电脑（0-2） |
| `anti_idle_enabled` | false | 是否定期发送 HID 保活信号 |
| `anti_idle_interval_sec` | — | 保活信号间隔（秒） |
| `input_mode` | 0（KVM） | 0=KVM 模式，1=PPT 空中鼠标 |
| `air_mouse_sensitivity` | 5 | IMU 灵敏度（1-10） |
| `voice_asr_enabled` | false | 是否启用语音输入功能 |
| `voice_asr_appid` | 0 | 百度语音识别 App ID |
| `voice_asr_api_key` | — | 百度语音识别 API Key |
| `voice_lang` | "zh" | 语音识别语言："zh"（中文）或 "en"（英文） |
| `voice_input_mode` | 0（自动） | 文本输入模式：0=自动，1=拼音，2=仅 ASCII |

## Web API

Web 服务器提供 REST API（所有接口需要 `Authorization: Bearer <token>` 请求头）：

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/status` | 设备状态（电脑连接、电池等） |
| `GET` | `/api/settings` | 全部配置项 |
| `PATCH` | `/api/settings` | 修改配置 |
| `POST` | `/api/switch` | 切换当前活动电脑 |
| `POST` | `/api/factory-reset` | 清除所有 NVS 数据并重启（需 `{"confirm": true}`） |
| `GET` | `/api/logs` | SSE 实时 ESP-IDF 日志流 |

## 语音输入（M5StickS3 专属）

### 工作原理

1. 长按主按钮（≥500ms）—— TFT 屏幕变为红色常亮
2. 对着内置麦克风说话
3. 松开按钮 → 发送 FINISH 帧给百度语音识别服务
4. 识别结果以 HID 键盘扫描码的方式键入当前活动电脑

### 音频链路

```
SPM1423 MEMS 麦克风（模拟）→ ES8311 ADC → I2S → ESP32-S3
                                                  ↓
                              百度实时语音识别 ← WebSocket ← PCM 16kHz/16bit/单声道
```

### 百度语音识别设置

1. 前往[百度 AI 云平台](https://console.bce.baidu.com/ai/)注册
2. 创建一个"实时语音识别"应用
3. 记录 **App ID** 和 **API Key**
4. 在 Web 仪表板的设置页面填写上述信息

### 文本输入模式

- **自动 (0)**：尽力而为 —— ASCII 字符直接键入；中文字符跳过（需在目标电脑上激活中文输入法才能输入中文）
- **拼音 (1)**：同自动模式；中文输入依赖目标电脑已激活中文输入法
- **仅 ASCII (2)**：只键入 ASCII 字符，跳过所有非 ASCII 字节

> **注意：** HID 键盘报告只能发送 USB HID 键码（美式布局）。输入中文字符需要目标电脑已激活中文输入法。设备将拼音罗马字以 ASCII 按键方式发送；输入法负责将其转换为汉字。

## 恢复出厂设置

**方式一 —— 按键（M5StickS3）：** 长按副按钮 5 秒（TFT 显示警告提示），继续按住 5 秒（共 10 秒）确认执行。

**方式二 —— Web API：** `POST /api/factory-reset`，请求体 `{"confirm": true}`，需要有效的认证令牌。

两种方式均会清除所有 NVS 数据并重启设备。

## 许可证

MIT
