---
name: sticks3-stamps3-adaptation
description: Adapt ble-kvm for M5Stack StickS3 (TFT + battery) and Stamp S3 (RGB LED), with board abstraction layer
type: project
---

# BLE-KVM 多板型适配设计

## 目标

将现有 ble-kvm（ESP32-S3 DevKitC）适配到 M5Stack StickS3 和 Stamp S3，通过板级抽象层实现一套代码支持三个板型。

## 支持的板型

| 板型 | 芯片 | Flash | PSRAM | 状态指示 | 按钮 | 电池 |
|------|------|-------|-------|----------|------|------|
| DevKitC (默认) | ESP32-S3-WROOM-1 | 8MB | 可选8MB | 2x LED (GPIO1/2) | 1x (GPIO0) 切换 | 无 |
| StickS3 | ESP32-S3-PICO-1 | 8MB | 8MB | ST7789 TFT 135x240 | 2x (G11/G12) | 250mAh + M5PM1 |
| Stamp S3 | ESP32-S3FN8 | 8MB | 无 | 1x WS2812B RGB | 1x (G0) 切换 | 无 |

**配对方式**：所有板型统一通过 Web API 配对，按钮仅用于切换 PC。

---

## 1. 板级抽象层 (`board.h`)

新增 `board.h`，编译时通过 `BOARD` 宏选择配置：

```c
// 编译选择: -DBOARD=m5sticks3 / -DBOARD=m5stamps3 / 默认 DevKitC

#ifdef BOARD_M5STICKS3
  // 按钮
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_11
  #define BUTTON_SECONDARY_GPIO  GPIO_NUM_12  // KEY2: 切换 TFT 页面
  #define HAS_SECONDARY_BUTTON   1
  // 显示
  #define HAS_TFT_DISPLAY        1
  #define TFT_MOSI_GPIO          GPIO_NUM_39
  #define TFT_SCLK_GPIO          GPIO_NUM_40
  #define TFT_DC_GPIO            GPIO_NUM_45
  #define TFT_CS_GPIO            GPIO_NUM_41
  #define TFT_RST_GPIO           GPIO_NUM_21
  #define TFT_BL_GPIO            GPIO_NUM_38
  #define TFT_WIDTH              135
  #define TFT_HEIGHT             240
  #define TFT_OFFSET_X           52
  #define TFT_OFFSET_Y           40
  // 电源管理
  #define HAS_BATTERY            1
  #define PMIC_I2C_ADDR          0x6e
  #define I2C_SDA_GPIO           GPIO_NUM_47
  #define I2C_SCL_GPIO           GPIO_NUM_48
  // LED (StickS3 不使用独立 LED)
  #define HAS_RGB_LED            0
  #define HAS_GPIO_LED           0

#elif defined(BOARD_M5STAMPS3)
  // 按钮
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_0
  #define HAS_SECONDARY_BUTTON   0
  // RGB LED
  #define HAS_RGB_LED            1
  #define RGB_LED_GPIO           GPIO_NUM_27  // WS2812B (需确认)
  #define HAS_GPIO_LED           0
  #define HAS_TFT_DISPLAY        0
  #define HAS_BATTERY            0

#else  // DevKitC 默认
  // 按钮
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_0
  #define HAS_SECONDARY_BUTTON   0
  // LED
  #define HAS_GPIO_LED           1
  #define LED1_GPIO              GPIO_NUM_2
  #define LED2_GPIO              GPIO_NUM_1
  #define HAS_RGB_LED            0
  #define HAS_TFT_DISPLAY        0
  #define HAS_BATTERY            0
#endif
```

**构建方式**：
```bash
idf.py build                              # DevKitC 默认
idf.py -DBOARD=m5sticks3 build           # StickS3
idf.py -DBOARD=m5stamps3 build           # Stamp S3
```

`main/CMakeLists.txt` 根据 BOARD 变量添加编译定义和条件源文件。

---

## 2. 状态指示抽象 (`indicator.h/c`)

替代现有 `led_controller.c`，根据板型编译不同实现：

### 2.1 公共接口

```c
typedef enum {
    IND_PC1_ACTIVE,    // PC1 激活
    IND_PC2_ACTIVE,    // PC2 激活
    IND_NO_PC,         // 无 PC 连接
    IND_PAIRING,       // 配对中
} indicator_state_t;

void indicator_init(void);
void indicator_set_state(indicator_state_t state);
```

### 2.2 DevKitC 实现 (gpio_led.c)

编译条件：`HAS_GPIO_LED=1`

与现有 `led_controller.c` 逻辑相同，两个 GPIO LED：
- IND_PC1_ACTIVE → LED1 亮 LED2 灭
- IND_PC2_ACTIVE → LED1 灭 LED2 亮
- IND_NO_PC → LED1 慢闪 LED2 灭
- IND_PAIRING → 交替快闪

### 2.3 Stamp S3 实现 (rgb_led.c)

编译条件：`HAS_RGB_LED=1`

WS2812B 单颗 RGB LED，使用 ESP-IDF RMT 驱动发送数据：
- IND_PC1_ACTIVE → 绿色常亮
- IND_PC2_ACTIVE → 蓝色常亮
- IND_NO_PC → 红色慢闪 (1s 周期)
- IND_PAIRING → 白色快闪 (200ms 周期)

RMT 驑动 WS2812B：1 bit = 1.25us, 0-code 高0.3us+低0.9us, 1-code 高0.9us+低0.3us。24bit GRB 数据 + reset (>50us 低)。

### 2.4 StickS3 实现 (tft_display.c)

编译条件：`HAS_TFT_DISPLAY=1`

使用 ESP-IDF 内置 `esp_lcd` API + `esp_lcd_new_panel_st7789()`，纯 C 无外部依赖。

**初始化流程**：
1. I2C 初始化 (GPIO47/48)，配置 M5PM1 使能 LCD 供电
2. SPI3_HOST 初始化 (GPIO39/40)
3. 创建 `esp_lcd_panel_io_spi` + `esp_lcd_panel_st7789`
4. 设置 offset (52, 40)、颜色翻转、背光 PWM (GPIO38)

**显示内容 — 状态页**（默认页面）：
```
┌─────────────┐
│  PC1 ●      │  ← 绿色圆点=连接, 灰色=断连
│  PC2 ○      │
│             │
│  ▶ PC1      │  ← 大字当前激活 PC
│             │
│  KB ●  MS ● │  ← 键鼠连接状态
│  🔋 85%     │  ← 电量 (仅 StickS3)
└─────────────┘
```

**显示内容 — 调试页**（KEY2 按钮切换）：
```
┌─────────────┐
│ WiFi: AP+STA│
│ IP: 192.168 │
│   .4.1      │
│ SSID: BLE-  │
│   KVM-XXXX  │
│             │
│ BLE: 4 conn │
│ USB: ✓      │
└─────────────┘
```

**字体渲染**：内嵌小型 bitmap 字体（仅数字 + ASCII + 少量符号），通过 `esp_lcd_panel_draw_bitmap()` 渲染。字体数据 ~2KB Flash。

**背光自动休眠**：30s 无按钮操作 → 背光关闭，按钮唤醒重新亮屏。

---

## 3. 电源管理 (`power_manager.h/c`)

仅 StickS3 编译 (`HAS_BATTERY=1`)。

通过 I2C (GPIO47/48) 与 M5PM1 (0x6e) 通信：

**功能**：
- 读取电池电压和电量百分比
- USB 充电状态检测
- 电量信息提供给 TFT 调试页显示
- 低电量 (<15%) 告警：TFT 闪烁提示，RGB LED 不适用

**自动休眠策略**（仅电池供电时生效，USB 供电时禁用）：
- 5 分钟无操作 → 关闭 Wi-Fi STA，保留 AP（降低功耗约 40mA）
- 15 分钟无操作 → 深度睡眠，按钮唤醒后恢复

**实现说明**：M5PM1 的完整寄存器文档需参考其 datasheet。初始实现使用已知寄存器读取电压，后续迭代完善电量计算和休眠控制。

---

## 4. 按钮逻辑变更 (`switch_manager.c`)

### 改动

- `BUTTON_GPIO` 替换为 `BUTTON_SWITCH_GPIO` (来自 board.h)
- 移除长按配对逻辑（配对改由 Web API 触发）
- StickS3 KEY2 (`BUTTON_SECONDARY_GPIO`)：短按切换 TFT 页面
- 仅保留单击切换 PC 功能

### 消抖与中断

保持现有 GPIO 中断 + 50ms 消抖逻辑不变，仅 GPIO 编号通过 board.h 配置。

---

## 5. RESTful API 重新设计

合并键鼠配对为一个端点，统一 RESTful 规范：

| 端点 | 方法 | 认证 | 功能 |
|------|------|------|------|
| `/` | GET | 否 | 嵌入的 Web 前端页面 (gzip) |
| `/api/status` | GET | 是 | 设备状态（PC连接、键鼠、Wi-Fi、电量、固件版本） |
| `/api/switch` | POST | 是 | 切换到另一台 PC，返回当前激活 PC |
| `/api/events` | GET | 是 (query) | SSE 实时事件推送 |
| `/api/pairings` | POST | 是 | 触发配对：`{"type":"pc"}` 或 `{"type":"device","address":"xx:xx:...","addr_type":0,"role":"keyboard"|"mouse"}` |
| `/api/pairings/{id}` | DELETE | 是 | 删除配对设备（PC 或键鼠） |
| `/api/scan` | POST | 是 | 触发 BLE 扫描（持续 5s） |
| `/api/scan` | GET | 是 | 获取最近一次扫描结果 |
| `/api/devices` | GET | 是 | 已配对设备列表（PC + 键鼠） |
| `/api/wifi` | GET | 是 | Wi-Fi 状态（模式、IP、SSID） |
| `/api/wifi` | PATCH | 是 | 更新 Wi-Fi 配置（模式、SSID、密码） |
| `/api/settings` | GET | 是 | 设备设置（PC名称、Token） |
| `/api/settings` | PATCH | 是 | 更新设置（PC名称、重生成 Token） |

**关键变更**：

1. **键鼠配对合并**：`POST /api/pairings` 一个端点处理所有配对类型，通过 `type` 字段区分：
   - `{"type":"pc"}` → Peripheral 开始广播等待 PC 连接
   - `{"type":"device","role":"keyboard","address":"...","addr_type":0}` → Central 连接键盘
   - `{"type":"device","role":"mouse","address":"...","addr_type":0}` → Central 连接鼠标
   - 删除配对：`DELETE /api/pairings/{id}`，id 可为 `pc1`、`pc2`、`keyboard`、`mouse`

2. **扫描合并**：`POST /api/scan` 触发扫描，`GET /api/scan` 返回结果，取代原来的 `/api/scan` + `/api/scan/results`

3. **HTTP 方法语义**：
   - `PATCH` 替代 `POST` 用于部分更新（Wi-Fi 配置、Settings）
   - `POST` 仅用于创建资源或触发动作（配对、扫描、切换）
   - `DELETE` 用于删除配对
   - `GET` 用于读取

4. **移除按钮长按配对**，所有配对通过 Web API 触发

配对状态通过 SSE `/api/events` 实时推送，前端显示配对进度。按钮仅用于切换 PC。

---

## 6. 防休眠功能 (`anti_idle.h/c`)

防止 PC 进入休眠/锁屏。当键鼠 5 分钟无操作时，自动向当前激活 PC 发送微小鼠标移动（1px），保持 PC 唤醒。

**实现**：
- `anti_idle_init()` — 创建 FreeRTOS 软件定时器
- 监听 `hid_router`：每次有键鼠 HID 报告转发时重置定时器
- 定时器超时（5分钟无操作）→ 调用 `ble_peripheral_send_hid_report()` 发送鼠标移动报告 `dx=1, dy=0`，然后 `dx=-1, dy=0` 恢复原位
- 移动间隔：超时后每 4 分钟发送一次（确保不超过 Windows/macOS 默认 5 分钟休眠阈值）

**配置**：
- `anti_idle_enabled` — 开关，存入 NVS (`kvm_config` 命名空间)，默认关闭
- `anti_idle_interval_sec` — 间隔秒数，默认 240（4 分钟），可配置

**Web API**：
- `GET /api/settings` 响应增加 `"anti_idle": true, "anti_idle_interval": 240`
- `PATCH /api/settings` 支持更新：`{"anti_idle": true}` 或 `{"anti_idle_interval": 300}`

**config_manager 变更**：
- `kvm_config_t` 增加 `bool anti_idle_enabled` 和 `uint16_t anti_idle_interval_sec`
- `config_save_anti_idle()` / `load_anti_idle()` 持久化到 NVS

**安全性**：仅在激活 PC 且鼠标已连接时生效；断开鼠标或切换 PC 时暂停定时器。

---

## 7. sdkconfig 适配

三个板型共享大部分 sdkconfig，差异通过 `sdkconfig.defaults.board` 覆盖：

**公共配置**（不变）：
- `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=4`（独立键鼠 + 2 PC）
- `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`

**StickS3 额外**（`sdkconfig.defaults.m5sticks3`）：
- `CONFIG_SPIRAM=y`（8MB PSRAM）
- 新增 `esp_lcd`、`esp_driver_i2c` 依赖

**Stamp S3 额外**（`sdkconfig.defaults.m5stamps3`）：
- 无 PSRAM 配置
- 新增 `esp_driver_rmt` 依赖（WS2812B 驱动）

---

## 8. CMake 构建系统

`main/CMakeLists.txt` 改动：

```cmake
idf_component_register(
    SRCS "main.c" "config_manager.c" "ble_peripheral.c"
         "switch_manager.c" "hid_router.c" "ble_central.c"
         "wifi_manager.c" "web_server.c"
         "indicator.c"
    # 条件源文件通过 BOARD 宏在编译时选择:
    # HAS_GPIO_LED → gpio_led.c
    # HAS_RGB_LED  → rgb_led.c
    # HAS_TFT_DISPLAY → tft_display.c
    # HAS_BATTERY → power_manager.c
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio esp_wifi esp_netif
             esp_event json esp_http_server
)

# 根据 BOARD 添加条件源文件和依赖
if(BOARD STREQUAL "m5sticks3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_M5STICKS3)
    # 添加 tft_display.c, power_manager.c
    # 添加 REQUIRES: esp_lcd esp_driver_i2c
elseif(BOARD STREQUAL "m5stamps3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_M5STAMPS3)
    # 添加 rgb_led.c
    # 添加 REQUIRES: esp_driver_rmt
endif()
```

---

## 9. 模块依赖关系（更新后）

```
main.c
 ├── config_manager (NVS, 最先初始化)
 ├── wifi_manager (Wi-Fi AP/STA)
 ├── indicator (状态指示, 依赖 board.h)
 │   ├── gpio_led   (DevKitC 条件编译)
 │   ├── rgb_led    (Stamp S3 条件编译)
 │   └── tft_display (StickS3 条件编译, 依赖 esp_lcd)
 ├── power_manager  (StickS3 条件编译, 依赖 I2C)
 ├── web_server (HTTP + REST, 依赖 config + switch)
 ├── ble_central (键鼠连接)
 ├── ble_peripheral (PC HID Server)
 ├── hid_router (报告转发)
 └── switch_manager (切换控制, 依赖 indicator)
```

---

## 10. 文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `main/board.h` | 新增 | 板级常量定义 |
| `main/indicator.h` | 新增 | 状态指示公共接口 |
| `main/indicator.c` | 新增 | 状态指示分发（调用对应实现） |
| `main/gpio_led.c` | 新增 | DevKitC GPIO LED 实现 |
| `main/rgb_led.c` | 新增 | Stamp S3 WS2812B 实现 |
| `main/tft_display.c` | 新增 | StickS3 ST7789 TFT 实现 |
| `main/power_manager.h` | 新增 | 电源管理接口 |
| `main/power_manager.c` | 新增 | StickS3 M5PM1 电源管理 |
| `main/anti_idle.h` | 新增 | 防休眠功能接口 |
| `main/anti_idle.c` | 新增 | 防休眠定时器 + 微移鼠标逻辑 |
| `main/led_controller.c` | 删除 | 被 indicator 替代 |
| `main/led_controller.h` | 删除 | 被 indicator 替代 |
| `main/switch_manager.c` | 修改 | 使用 board.h GPIO, 移除长按配对 |
| `main/switch_manager.h` | 修改 | 移除 SWITCH_SRC_BUTTON 配对相关 |
| `main/main.c` | 修改 | 初始化 indicator 代替 led_controller, 条件初始化 power_manager |
| `main/CMakeLists.txt` | 修改 | 条件编译源文件和依赖 |
| `main/config_manager.c` | 修改 | 新增 anti_idle 字段持久化 |
| `main/config_manager.h` | 修改 | kvm_config_t 增加 anti_idle_enabled, anti_idle_interval_sec |
| `main/hid_router.c` | 修改 | 转发报告时通知 anti_idle 重置定时器 |
| `main/web_server.c` | 修改 | RESTful API 重新设计：合并配对端点、PATCH 替代 POST 更新、扫描合并、anti_idle 设置 |

**Why**: 统一三个板型的代码库，避免分支分裂。板级差异通过编译宏隔离，核心 BLE/HID/Web 逻辑共享不变。

**How to apply**: 所有板型共享同一代码库，通过 `idf.py -DBOARD=xxx` 选择目标板构建。