# BLE-KVM 设计文档

## 概述

基于 ESP32-S3 的蓝牙 KVM 切换器，支持 2 台电脑之间的键鼠切换。

### 核心功能

- BLE 键盘/鼠标作为输入源，连接到 ESP32-S3
- ESP32-S3 通过 BLE HID 同时连接 2 台电脑
- 物理按钮切换激活目标，LED 指示当前状态
- Web 服务器辅助 BLE 配对和设备管理

### 技术栈

- **硬件**: ESP32-S3-WROOM-1-N8R8 (512KB SRAM + 8MB PSRAM + 8MB Flash)
- **框架**: ESP-IDF 5.x
- **协议**: BLE 4.2/5.0, Wi-Fi 802.11 b/g/n
- **前端**: React + TypeScript，Vite 构建，产物嵌入 Flash

---

## 系统架构

```
┌──────────────────────────────────────────────────────┐
│                  ESP32-S3 BLE KVM                    │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐          │
│  │BLE GAP   │  │  HID     │  │  Switch   │          │
│  │Central   │  │  Router  │  │  Manager  │          │
│  │(键鼠输入)│  │(数据转发) │  │(切换控制) │          │
│  └────┬─────┘  └────┬─────┘  └─────┬─────┘          │
│       │             │              │                  │
│  ┌────┴─────────────┴──────────────┴──────────┐      │
│  │           BLE GAP Peripheral               │      │
│  │       (HID Server: PC1, PC2)               │      │
│  └────────────────┬───────────────────────────┘      │
│                   │                                  │
│  ┌────────────────┴──────────────────────────┐      │
│  │  LED Indicator  │  Button GPIO            │      │
│  └────────────────────────────────────────────┘      │
│                                                      │
│  ┌────────────────────────────────────────────┐      │
│  │         Wi-Fi + Web Server                 │      │
│  │  ┌──────────┐  ┌───────────────────────┐   │      │
│  │  │ HTTP     │  │  Config Manager       │   │      │
│  │  │ Server   │  │  - BLE 配对管理       │   │      │
│  │  │ (REST)   │  │  - 设备命名/排序      │   │      │
│  │  └──────────┘  │  - Wi-Fi 配置         │   │      │
│  │                │  - 固件升级(OTA)      │   │      │
│  │                └───────────────────────┘   │      │
│  └────────────────────────────────────────────┘      │
└──────────────────────────────────────────────────────┘
```

### 核心组件

| 组件 | 职责 |
|------|------|
| BLE GAP Central | 扫描并连接 BLE 键盘/鼠标，接收 HID 报告 |
| BLE GAP Peripheral | 向 PC1 和 PC2 提供 BLE HID 服务 |
| HID Router | 将键鼠 HID 报告转发给当前激活的电脑 |
| Switch Manager | 处理按钮事件，切换激活目标，控制 LED |
| Web Server | HTTP REST API，提供配置界面 |
| Config Manager | NVS 配置管理，持久化配对信息 |

---

## BLE 通信设计

### BLE 角色分工

| 连接 | 角色 | 用途 |
|------|------|------|
| 键盘 → ESP32 | Central (GATT Client) | 读取键盘 HID 报告 |
| 鼠标 → ESP32 | Central (GATT Client) | 读取鼠标 HID 报告 |
| ESP32 → PC1 | Peripheral (GATT Server) | 向 PC1 发送 HID 报告，PC1 为 Central 主动连接 |
| ESP32 → PC2 | Peripheral (GATT Server) | 向 PC2 发送 HID 报告，PC2 为 Central 主动连接 |

**总连接数**: 3-4 个同时连接（键鼠合一设备为 3，分离键鼠为 4）

**角色说明**：ESP32-S3 同时承担 Central（连键鼠）和 Peripheral（连 PC）角色。PC 作为 Central 主动连接 ESP32-S3，ESP32-S3 无法主动向 PC 发起连接——PC 断连后，ESP32-S3 只能重新开始广播等待 PC 重连。

### PC 身份识别

通过身份地址（Identity Address）绑定 PC 身份。首次配对时通过 bonding 的 IRK 解析 PC 的身份地址，分配编号（PC1/PC2），存入 NVS。重连时通过 IRK 解析恢复身份，而非依赖连接顺序或随机 MAC 地址。

```c
// PC 身份映射表（NVS 持久化）
typedef struct {
    uint8_t identity_addr[6]; // PC 的身份地址（通过 IRK 解析）
    uint8_t pc_id;            // 1 或 2
    char name[32];            // 用户自定义名称（如"工作机"）
    uint16_t conn_handle;     // 当前连接句柄（运行时）
    bool connected;           // 当前是否连接
} pc_device_t;
```

GAP connect 事件中，通过 IRK 解析对端身份地址，查找映射表恢复 conn_handle → PC 编号的关联。

**注意**：Windows/macOS 默认使用 BLE 随机 MAC 地址（隐私保护），每次连接可能变化。解决方案：在配对完成后通过 bonding 信息中的 IRK（Identity Resolving Key）解析真实身份，而非直接比较 MAC 地址。NimBLE 的 `ble_store_util_bonded_peers()` 可获取已配对设备的身份地址。

### HID 报告转发流程

```
BLE 键盘 ──HID Report──> ESP32 GAP Central
                              │
                         HID Router ──根据激活目标──> PC1 HID Report
                                                    or
                                                    PC2 HID Report
```

1. ESP32-S3 作为 Central 连接 BLE 键鼠，订阅其 HID Report Characteristic
2. 收到 HID 报告后，HID Router 查询当前激活目标
3. 将报告写入对应 PC 的 HID Report Characteristic
4. 非激活 PC 不收到任何 HID 数据

### GATT Server 多连接通知路由

ESP32-S3 运行单个 GATT Server，PC1 和 PC2 作为 Central 连接到同一 GATT Server。HID Service 只注册一份，两个 PC 共享同一套 Characteristic。

**关键实现**：使用 NimBLE 的 per-connection notification 机制，通过 `ble_gatts_notify_custom()` 指定 `conn_handle`，只向活跃 PC 发送 HID Notification，非活跃 PC 不收到任何数据。传入 `BLE_HS_CONN_HANDLE_NONE` 则通知所有已订阅连接。

```c
// HID Router 转发伪代码
void hid_router_forward_report(uint8_t *report, size_t len) {
    uint16_t active_conn = switch_manager_get_active_conn_handle();
    if (active_conn != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(report, len);
        if (om) {
            ble_gatts_notify_custom(active_conn, report_chr_handle, om);
        }
    }
}
```

这要求 HID Router 维护 conn_handle → PC 编号的映射，在 GAP connect/disconnect 事件中更新。

### BLE HID Profile (Peripheral 侧)

ESP32-S3 注册一套 BLE HID Service，所有 PC 连接共享：

- **Service UUID**: 0x1812 (HID)
- **Report Map**: 支持键盘 + 鼠标的 HID 描述符
- **Report Characteristic**: 用于发送 HID 数据
- **Protocol Mode**: 仅支持 Report Protocol Mode（见下方说明）
- **Boot Protocol**: 不支持。共享 GATT Server 下多 PC 可能请求不同 Protocol Mode 产生冲突，且现代 PC BIOS/UEFI 已普遍支持 Report Protocol

### 配对安全

- PC 端 BLE HID 使用 bonding + encryption (SC: Just Works or Passkey)
- 键鼠端根据设备能力选择 Just Works 或 Passkey
- 配对信息通过 NVS 持久化，重启后自动重连

### 断连检测与自动重连

| 场景 | 检测方式 | 恢复策略 |
|------|----------|----------|
| PC 休眠 | GAP disconnect 事件 | ESP32-S3 重新开始定向广播（基于身份地址），等待 PC 唤醒后重连 |
| PC 蓝牙关闭/重启 | GAP disconnect 事件 | 定向广播（身份地址）+ 通用广播交替，等待 PC 重连 |
| 键鼠超出范围 | GAP disconnect 事件 | 指数退避重连（Central 角色可主动发起） |
| ESP32 重启 | NVS 保存的配对信息 | 启动后开始广播，等待已配对 PC 主动连接 |
| 单 PC 连接失败 | 广播 5 分钟无响应 | 降级为单 PC 模式，LED 指示仅一个 PC 连接 |

重连策略因角色不同而异：
- **PC 连接（Peripheral 侧）**：ESP32-S3 无法主动连接 PC，只能重新广播等待 PC 重连。使用定向广播（Directed Advertising）指向已配对 PC 的身份地址（通过 IRK 解析，非随机 MAC），加速重连。定向广播 1.28s 后未连接则切换为通用广播。
- **键鼠连接（Central 侧）**：ESP32-S3 可主动发起连接，使用指数退避重连。

### PC 配对流程

ESP32-S3 作为 Peripheral，通过广播等待 PC 主动连接：

```
空闲 ──> 用户触发配对（按钮长按或 Web API）──> 开始通用广播（限时 60s）
──> PC1 主动连接并完成配对 ──> 记录身份地址 ──> 继续广播
──> PC2 主动连接并完成配对 ──> 记录身份地址 ──> 停止广播 ──> 正常运行
```

**规则**：
- 正常运行时使用定向广播（Directed Advertising）指向已配对 PC 的身份地址（IRK 解析），加速重连；所有 PC 已连接后停止广播
- 已配满 2 台 PC 时，需先通过 Web API 删除一台才能配对新 PC
- 广播超时 60s 未配满则回到空闲状态，LED 恢复正常指示
- 配对期间 LED 快闪提示

### 键鼠配对与扫描策略

**扫描时机**：仅在配对模式（用户主动触发）时执行扫描，正常运行不扫描。

**扫描参数**（低占空比，减少射频压力）：
```
scan_interval: 100ms, scan_window: 30ms
```

**键鼠合一设备**：BLE Central 连接键鼠时，检测设备的 Service 列表。如果同一设备同时包含键盘和鼠标 HID Service，只建立 1 个连接，订阅两个 Report Characteristic。此时总连接数为 3（1 键鼠合一 + PC1 + PC2），进一步降低射频压力。

---

## 切换控制与硬件接口

### 按钮切换

- **GPIO 按钮**: 一个物理按钮连接到 GPIO，低电平触发（内部上拉）
- **消抖**: 软件消抖 50ms
- **行为**:
  - 单击：切换到另一台 PC，循环切换 PC1 ↔ PC2
  - 长按（>2s）：进入 BLE 配对模式

### LED 指示

| 状态 | LED1 | LED2 |
|------|------|------|
| 连接 PC1 激活 | 亮 | 灭 |
| 连接 PC2 激活 | 灭 | 亮 |
| 正在切换 | 交替闪烁 | 交替闪烁 |
| 配对模式 | 快闪 | 快闪 |
| 无 PC 连接 | 慢闪 | 慢闪 |

### Wi-Fi + BLE 共存策略

ESP32-S3 的 Wi-Fi 和 BLE 共享 2.4GHz 射频，时分复用。3-4 个 BLE 连接 + Wi-Fi 同时运行时，需要确保键鼠 HID 报告的实时性。

**共存策略**：
- 运行时通过 `esp_bt_controller_config_t` 配置 BLE 优先共存策略，确保 HID 报告不被 Wi-Fi 传输抢占
- 键鼠 HID 报告延迟目标：< 15ms（端到端，从键鼠按键到 PC 收到 Notification）
- Web 配置仅在用户主动访问时才产生流量，不持续占用射频
- Wi-Fi 功率可动态调整：配对完成后降低 Wi-Fi 发射功率以减少对 BLE 的干扰

**运行模式**：

| 模式 | Wi-Fi 状态 | 适用场景 |
|------|-----------|----------|
| 正常运行 | AP+STA 低功率 | 日常使用，Web 随时可访问 |
| 配对模式 | AP+STA 全功率 | 正在扫描/配对新设备 |
| 纯 BLE | Wi-Fi 关闭 | 用户通过设置关闭 Wi-Fi |

### 硬件接口

| 功能 | GPIO | 说明 |
|------|------|------|
| 切换按钮 | GPIO0 | 低电平触发，内部上拉。复用 BOOT 按钮，启动 2s 后才注册中断回调，避免误入下载模式 |
| LED1 (PC1) | GPIO2 | 低电平点亮（ESP32-S3-DevKitC 板载 LED） |
| LED2 (PC2) | GPIO1 | 低电平点亮，需外接 LED + 限流电阻 |

**供电**：USB 5V 供电，ESP32-S3 不支持电池模式（Wi-Fi + BLE 功耗较高）

---

## Web API 设计

### 认证机制

所有 `/api/*` 端点需要 Token 认证：

- 首次启动时生成随机 8 位 Token，存入 NVS
- Token 印在设备标签上，同时通过串口输出
- Web 页面首次访问时弹出输入框填写 Token
- 后续 API 请求通过 `Authorization: Bearer <token>` 头认证
- Token 可通过 `POST /api/settings` 重新生成
- 静态资源（`/`、`/index.html`）无需认证，但页面加载后无 Token 无法获取数据

### API 端点

| 端点 | 方法 | 认证 | 功能 |
|------|------|------|------|
| `/` | GET | 否 | 返回嵌入的 Web 前端页面（gzip） |
| `/api/status` | GET | 是 | 当前连接状态、激活 PC、固件版本（`firmware_version` 字段，CMake 构建时注入） |
| `/api/switch` | POST | 是 | 切换目标 PC |
| `/api/events` | GET | 是（query 参数） | SSE 实时事件推送（连接/断开/切换，通过 `?token=xxx` 认证） |
| `/api/scan` | POST | 是 | 触发 BLE 扫描（异步，持续 5s） |
| `/api/scan/results` | GET | 是 | 获取最近一次扫描结果 |
| `/api/pair/keyboard` | POST | 是 | 触发键盘配对（BLE Central 扫描并连接键盘） |
| `/api/pair/mouse` | POST | 是 | 触发鼠标配对（BLE Central 扫描并连接鼠标） |
| `/api/pair/pc` | POST | 是 | 触发 PC 配对模式（开始广播，等待 PC 连接） |
| `/api/devices` | GET | 是 | 已配对设备列表 |
| `/api/devices/{id}` | DELETE | 是 | 删除配对设备 |
| `/api/wifi` | POST | 是 | 配置 Wi-Fi |
| `/api/settings` | GET/POST | 是 | 设备命名、Token 管理等设置 |
| `/api/ota` | POST | 是 | 上传新固件（multipart/form-data） |

### SSE 事件格式

SSE 端点通过 URL query 参数认证（`/api/events?token=xxx`），因为浏览器 `EventSource` API 不支持自定义 HTTP Header。

```
GET /api/events?token=xxx
Content-Type: text/event-stream

event: switch
data: {"active_pc": 1}

event: connection
data: {"pc1": "connected", "pc2": "disconnected"}

event: device
data: {"keyboard": "connected", "mouse": "disconnected"}
```

前端使用 `EventSource` API 监听，无需轮询。Token 通过 query 参数传递，服务端验证后建立长连接。

---

## 软件架构

### 项目目录结构

```
ble-kvm/
├── main/
│   ├── main.c                  # 入口，初始化各模块
│   ├── ble_central.c/h         # BLE GAP Central，连接键鼠
│   ├── ble_peripheral.c/h      # BLE GAP Peripheral，HID Server
│   ├── hid_router.c/h          # HID 报告路由
│   ├── switch_manager.c/h      # 切换控制 + 按钮处理
│   ├── led_controller.c/h      # LED 状态指示
│   ├── web_server.c/h          # HTTP 服务器 + REST API
│   ├── config_manager.c/h      # NVS 配置管理
│   └── wifi_manager.c/h        # Wi-Fi AP/STA 管理
├── web/                        # React 前端项目（独立于 ESP-IDF 构建树）
│   ├── src/
│   │   ├── App.tsx             # 主组件
│   │   ├── components/        # UI 组件
│   │   ├── hooks/             # 自定义 Hooks
│   │   ├── api.ts             # API 请求封装
│   │   └── main.tsx           # 入口
│   ├── dist/                  # 构建输出（单文件 HTML，gzip 压缩）
│   ├── index.html             # HTML 模板
│   ├── vite.config.ts         # Vite 构建配置
│   ├── tsconfig.json
│   └── package.json
├── components/                 # 可选：复用组件
├── sdkconfig                   # ESP-IDF 配置
├── CMakeLists.txt              # 包含自定义命令：npm build → embed dist/
└── partitions.csv              # 分区表（预留 OTA）
```

### 切换并发控制

Switch Manager 是唯一的状态变更入口。按钮 GPIO 中断和 Web API `/api/switch` 都通过 FreeRTOS 消息队列向 Switch Manager 发送切换请求，避免竞态。

```
GPIO 中断 ──> 消息队列 ──> Switch Manager ──> 更新激活目标
Web API  ──> 消息队列 ──┘                  ──> 通知 HID Router
                                           ──> 更新 LED
```

### FreeRTOS 任务优先级

| 任务 | 优先级 | 栈大小 | 说明 |
|------|--------|--------|------|
| NimBLE Host | 高 (5) | 4KB | BLE 协议栈事件循环，必须高优先级保证 HID 实时性 |
| HID Router | 高 (4) | 2KB | 接收键鼠报告并转发，延迟敏感 |
| Switch Manager | 中 (3) | 2KB | 处理切换请求，消息队列驱动 |
| Web Server | 低 (2) | 4KB | HTTP 请求处理，非实时 |
| Wi-Fi Manager | 低 (2) | 2KB | Wi-Fi 事件处理 |
| LED Controller | 最低 (1) | 1KB | GPIO 闪烁控制，无实时要求 |

### 模块依赖关系

```
main.c
 ├── config_manager (NVS 读写，最先初始化)
 ├── wifi_manager (初始化 Wi-Fi)
 ├── web_server (启动 HTTP，依赖 config_manager + switch_manager)
 ├── ble_central (扫描/连接键鼠，依赖 config_manager)
 ├── ble_peripheral (HID Server，2 个连接，依赖 config_manager)
 ├── hid_router (依赖 ble_central + ble_peripheral)
 ├── switch_manager (依赖 hid_router + led_controller)
 └── led_controller (GPIO 控制)
```

### NVS 命名空间规划

| 命名空间 | 模块 | 存储内容 |
|----------|------|----------|
| `kvm_wifi` | wifi_manager | SSID, password, mode |
| `kvm_ble` | ble_central/peripheral | PC 身份地址, 键鼠 MAC, bonding 信息 |
| `kvm_config` | config_manager | 设备名称, auth_token, 激活 PC |
| `kvm_system` | main | 固件版本, 首次启动标志 |

### 关键 ESP-IDF 配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 4 | BLE 最大连接数（3-4 连接场景） |
| `CONFIG_BT_NIMBLE` | y | 使用 NimBLE 协议栈（更省内存） |
| `CONFIG_BT_ENABLED` | y | 启用蓝牙控制器 |
| `CONFIG_ESP_HTTP_SERVER` | y | HTTP 服务器组件 |

共存优先级在运行时通过 `esp_bt_controller_config_t` 配置，设置 `ble_mode_priority` 为 BLE 优先。

### 内存预算

ESP32-S3-WROOM-1-N8R8 有 512KB SRAM + 8MB PSRAM。各模块预估内存占用：

| 模块 | SRAM 占用 | 说明 |
|------|-----------|------|
| NimBLE 协议栈 | ~70KB | 3-4 连接 + GATT Server + Central |
| Wi-Fi 栈 | ~60KB | AP+STA 共存 |
| HTTP Server | ~25KB | 请求/响应缓冲区 |
| FreeRTOS + 系统开销 | ~40KB | 任务栈、堆管理 |
| Web 前端 | 0 (SRAM) | 单文件 HTML gzip 嵌入 Flash，SPI 映射读取 |
| HID 报告缓冲 | ~4KB | 键盘 + 鼠标双缓冲 |
| NVS + 配置 | ~8KB | 配置数据缓存 |
| **合计** | **~207KB** | 剩余 ~305KB SRAM 裕量充足 |

PSRAM 可用于 HTTP 响应大缓冲区和 OTA 下载缓冲，进一步释放 SRAM。

### React 前端构建策略

ESP32-S3 Flash 有限，React 构建产物需要严格控制体积：

**技术选型**：
- **React 19** + **TypeScript** + **Vite** 构建
- **CSS 方案**：CSS Modules（无运行时开销，不引入 CSS-in-JS 库）
- **不引入 UI 库**：自定义轻量组件，避免 antd/MUI 等重型库

**构建优化**：
- Vite 生产构建 + minify（esbuild）
- `import` 按需加载，tree-shaking 移除未使用代码
- 单页面不使用路由库，组件内条件渲染切换视图
- 使用 `vite-plugin-singlefile` 将 JS/CSS inline 到单个 HTML 文件
- 单文件 HTML gzip 压缩后通过 ESP-IDF EMBED 指令嵌入 Flash
- HTTP 响应只需一个 URI handler，设置 `Content-Type: text/html` + `Content-Encoding: gzip`
- CMake 自定义命令：`npm run build` → `gzip dist/index.html` → EMBED 到固件

**体积目标**：gzip 后 < 30KB（React ~12KB gzip + 业务代码 ~15KB gzip + CSS ~3KB gzip）

---

## 实现优先级

1. **Phase 1**: 基础框架 + BLE HID 单连接
   - 项目骨架搭建
   - BLE Peripheral 单个 HID 连接
   - 基本键盘/鼠标 HID 报告

2. **Phase 2**: 双 PC 连接 + 切换
   - BLE Peripheral 双连接
   - HID Router 实现
   - 按钮切换 + LED 指示

3. **Phase 3**: BLE 键鼠输入
   - BLE Central 扫描/连接键鼠
   - 完整 HID 报告转发

4. **Phase 4**: Web 配置界面
   - Wi-Fi AP/STA
   - HTTP REST API
   - Web 前端页面
   - NVS 配置持久化

5. **Phase 5**: 完善
   - OTA 升级
   - 错误处理优化
   - 性能调优

---

## 错误处理策略

| 错误场景 | 处理方式 |
|----------|----------|
| BLE 控制器初始化失败 | 串口输出错误，LED 双闪报错，仅 Web 功能可用 |
| Wi-Fi 初始化失败 | 降级为纯 BLE 模式，无 Web 配置 |
| NVS 损坏/格式化失败 | 恢复出厂设置，重新初始化 NVS |
| HTTP Server 启动失败 | 串口输出错误，BLE 功能不受影响 |
| 键鼠连接全部丢失 | LED 慢闪提示，保持 PC 连接，等待键鼠重连 |
| 两台 PC 都断连 | LED 双闪提示，保持键鼠连接，持续广播等待 PC 重连 |
| OTA 升级失败 | 回滚到旧分区，串口输出错误日志 |
| 内存不足 (OOM) | 释放 PSRAM 缓冲区，降低 HTTP Server 缓冲，串口告警 |
