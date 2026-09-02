# 项目概览

基于 ESP-IDF v5.5.3，目标平台 ESP32-S3，实现一个带触摸屏的播客播放器（Podcast Player）。

硬件参考：Leisound V1 板，集成 ES8156 DAC、GT911 触摸、SDMMC、LCD、Wi-Fi、电池管理等外设。

---

# 项目结构

```
NomadCast/
├── main/                   # ESP-IDF 入口 (app_main)
├── drivers/                # 芯片级驱动层
├── components/             # 硬件抽象组件层
├── sys/                    # 系统服务层
├── apps/                   # 应用层
├── board/                  # 板级配置
├── vendor/                 # 第三方库
├── debug/                  # 各模块独立测试程序
├── docs/                   # 文档与数据手册
├── refer/                  # 外部参考代码
├── tools/                  # 开发工具
└── scripts/                # 构建/烧录等脚本
```

---

# 分层架构

## 分层总览

```
┌──────────────────────────────────────┐
│  apps/         应用层                 │  UI 页面、业务逻辑、交互流程
├──────────────────────────────────────┤
│  sys/          系统服务层             │  事件总线、状态管理、UI 框架
├──────────────────────────────────────┤
│  components/   硬件抽象组件层          │  功能模块封装（独立可复用）
├──────────────────────────────────────┤
│  drivers/      芯片级驱动层           │  裸驱动：I2C/SPI/SDMMC 协议、寄存器操作
├──────────────────────────────────────┤
│  board/        板级配置               │  引脚定义、外设使能、硬件参数
└──────────────────────────────────────┘
```

**依赖方向**：`apps → sys → components → drivers → board`
上层可以调用下层，下层不感知上层。同层组件之间通过 sys 的事件总线解耦。

---

## drivers/ — 芯片级驱动层

**职责**：封装芯片的寄存器和通信协议，提供最底层的读写接口。不包含业务逻辑，不依赖 FreeRTOS 特性（除必要的延时和锁）。

**设计原则**：
- 每个驱动是独立的 ESP-IDF component（有自己的 `CMakeLists.txt`）
- 头文件暴露初始化、读写等最小 API
- 所有时序和寄存器操作内聚在 `.c` 中，调用者无需关心协议细节
- 驱动只对上层 component 暴露，app 层不应直接调用驱动

| 目录 | 芯片 | 协议 | 功能 | 状态 |
|------|------|------|------|------|
| `drivers/es8156/` | ES8156 | I2C + I2S | 音频 DAC 驱动，音量/静音/采样率控制 | 已实现 |
| `drivers/gt911/` | GT911 | I2C | 电容触摸控制器，读取坐标与手势 | 已实现 |
| `drivers/sdmcc/` | ESP32-S3 SDMMC | SDMMC 1-bit | SD 卡读写驱动 | 已有原型（见 debug/t_sd） |

**命名规范**：
- 文件名：`es8156.c / es8156.h`
- API 前缀：`es8156_init()`, `es8156_set_volume()`, `gt911_read_touch()`

---

## components/ — 硬件抽象组件层

**职责**：将 drivers 封装为有意义的硬件功能模块。每个 component 代表一个"功能单元"，提供更高层次的 API，内部管理状态和事件。

**设计原则**：
- 每个组件是独立的 ESP-IDF component
- 依赖对应的 driver（例如 `components/audio` 依赖 `drivers/es8156`）
- 通过事件回调或消息队列与上层通信（不直接调用上层代码）
- 可单独在 debug 目录下编写测试

| 目录 | 功能 | 依赖驱动 | 状态 |
|------|------|----------|------|
| `components/board_leisound/` | Leisound V1 板级硬件抽象：电源、I2C、ES8156、I2S、SD 卡初始化 | es8156 | 已实现 |
| `components/audio/` | 音频播放管理：解码、音量、播放控制 | es8156 | 待实现 |
| `components/battery/` | 电池电量检测、充放电状态 | ADC（内置） | 待实现 |
| `components/button/` | 物理按键扫描、消抖、长短按识别 | GPIO（内置） | 待实现 |
| `components/display/` | LCD 显示驱动与帧缓冲管理 | SPI/LCD 控制器（内置） | 待实现 |
| `components/flash/` | 内部 Flash 参数存储（NVS 封装） | NVS（内置） | 待实现 |
| `components/rtc/` | RTC 时钟管理、定时唤醒 | RTC（内置） | 待实现 |
| `components/storage/` | SD 卡文件系统挂载、文件读写 | sdmmc | 待实现 |
| `components/wifi/` | Wi-Fi 连接管理、网络状态 | Wi-Fi（内置） | 待实现 |

**命名规范**：
- 文件名：`audio_player.c / audio_player.h`
- API 前缀：`audio_play()`, `audio_pause()`, `battery_get_level()`

---

## sys/ — 系统服务层

**职责**：提供系统级的服务，连接 components 和 apps。负责跨模块的状态管理、事件分发、UI 框架和全局资源的生命周期。

**设计原则**：
- 系统层不直接访问 drivers，只通过 components 操作硬件
- 事件总线是 sys 的核心：components 发布事件，apps 订阅事件
- UI 框架统一管理所有界面的创建、切换和销毁

| 目录 | 功能 | 依赖 | 状态 |
|------|------|------|------|
| `sys/events/` | 事件总线：事件定义、发布/订阅、消息队列 | - | 待实现 |
| `sys/managers/` | 系统管理器：电源管理、任务调度、状态机 | components | 待实现 |
| `sys/ui_lvgl/` | LVGL 框架初始化、主题/样式、页面栈管理 | components/display | 待实现 |

**关键设计**：
- **事件总线**：使用 FreeRTOS 消息队列 + 事件循环，解耦 components 和 apps。例如 battery component 发布 `BATTERY_LOW` 事件，settings app 和 podcast app 各自订阅处理。
- **页面管理**：基于 LVGL 的页面栈，支持页面切换动画和返回栈。每个 app 注册自己的页面工厂函数。
- **状态机**：系统级状态（启动、待机、播放、设置、关机）由 managers 维护，apps 通过事件获取状态变更。

---

## apps/ — 应用层

**职责**：用户可见的功能页面，实现具体的 UI 和交互流程。每个 app 是一个独立的功能模块，拥有自己的 LVGL 页面和事件订阅。

**设计原则**：
- 每个 app 是一个独立的 ESP-IDF component
- 通过 sys/ui_lvgl 注册页面，通过 sys/events 获取系统事件
- app 之间不直接相互调用，通过事件总线通信
- 每个 app 有自己的初始化函数，由 main 统一调用注册

| 目录 | 功能 | 核心页面 | 状态 |
|------|------|----------|------|
| `apps/podcast/` | 播客播放器：列表浏览、播放控制、进度条、封面 | 播放页、列表页、详情页 | 待实现 |
| `apps/settings/` | 系统设置：Wi-Fi、音量、亮度、关于、时间 | 设置主页、Wi-Fi 页、关于页 | 待实现 |

**命名规范**：
- 文件名：`podcast_player.c / podcast_player.h`, `podcast_list.c / podcast_list.h`
- API 前缀：`podcast_app_init()`, `settings_app_init()`

---

# 入口与初始化

## main/ — ESP-IDF 入口

`main/NomadCast.c` 的 `app_main()` 是固件入口。初始化顺序：

```
1. board_init()       — 板级外设初始化（引脚、电源）
2. drivers 初始化       — ES8156, GT911, SDMMC
3. components 初始化    — audio, display, storage, wifi, battery, button, rtc
4. sys/events 初始化    — 事件总线启动
5. sys/managers 初始化  — 系统状态机启动
6. sys/ui_lvgl 初始化   — LVGL + 页面栈初始化
7. apps 初始化          — 各 app 注册页面，进入默认首页
```

---

# 板级配置

## board/

存放具体板子的引脚定义和外设配置，与代码逻辑分离，方便适配不同硬件版本。

| 文件 | 内容 |
|------|------|
| `components/board_leisound/board_leisound.h` | Leisound V1 板的引脚宏定义 + 初始化 API |
| `components/board_leisound/board_leisound.c` | 电源、I2C、ES8156、SD 卡初始化实现 |

---

# 测试与调试

## debug/

每个子目录是一个独立的 ESP-IDF 项目（有自己的 CMakeLists.txt），用于单独测试某个硬件模块。

| 目录 | 测试目标 | 状态 |
|------|----------|------|
| `debug/t_hello/` | 最简 Hello World，验证编译和烧录链路 | 通过 |
| `debug/t_sd/` | SD 卡读写测试（SDMMC 1-bit） | 通过 |
| `debug/t_display/` | LCD 显示测试 | 待验证 |
| `debug/t_display_touch/` | LCD + 触摸联合测试 | 待验证 |
| `debug/t_earphone/` | ES8156 耳放输出测试 | 待验证 |
| `debug/t_earphone2/` | ES8156 耳放输出测试（第二版原理图） | 通过 |
| `debug/t_speaker/` | 扬声器蜂鸣测试（I2S 正弦波输出） | 通过 |
| `debug/t_key/` | 物理按键扫描测试 | 待验证 |
| `debug/t_touch/` | GT911 触摸芯片测试 | 通过 |
| `debug/t_play/` | M4A 音频播放测试（SD 卡 + ESP-ADF 管线，支持 SD/HTTP 双源） | 编译通过 |

---

# 辅助目录

| 目录 | 用途 |
|------|------|
| `docs/` | 数据手册（GT911-GOODIX.pdf）、硬件原理图 |
| `refer/` | 外部参考代码，用于借鉴和对比 |
| `vendor/esp-adf/` | ESP-ADF 音频开发框架（需初始化 submodule: esp-adf-libs, esp-sr） |
| `vendor/esp-adf/components/audio_board/include/` | 自定义板级 stub（board.h, board_def.h, board_pins_config.c） |
| `tools/` | 开发辅助工具 |
| `scripts/` | 构建、烧录、打包等快捷脚本 |

---

# 开发约定

1. **命名**：文件名和函数名统一用小写+下划线。组件 API 前缀与组件名一致。
2. **错误处理**：所有初始化函数返回 `esp_err_t`，调用方必须检查返回值。
3. **日志**：使用 ESP-IDF 的 `ESP_LOGI`/`ESP_LOGW`/`ESP_LOGE` 宏，tag 用组件名。
4. **内存**：优先静态分配或从栈分配，少用堆。堆分配用 `malloc`/`free` 并在同模块内配对释放。
5. **新组件**：添加新目录时，需要创建 `CMakeLists.txt` 并通过 `idf_component_register` 注册，确保在根 `CMakeLists.txt` 的 `EXTRA_COMPONENT_DIRS` 中声明路径。
6. **驱动先行**：先写 driver → 再写 component → 再写 sys → 最后写 app。每个 driver/component 在 debug 下写独立测试。


# TODO
1. (https://getpodcast.xyz/)