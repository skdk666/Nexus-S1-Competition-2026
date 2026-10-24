# Nexus S1 — PC 场景感知桌面终端

ESP32-S3 + ILI9341 320×240 TFT + LVGL + USB CDC/HID + Windows Agent。
电脑前台应用经规则分类为 NORMAL / CODING / BROWSER / MEDIA，设备显示对应操作页；旋钮导航、独立按钮确认，快捷操作通过 USB HID 发给前台应用。

## 本次交付状态（2026-10-24）

维护者已确认开发板真机上的 TFT 显示、PC 场景自动切换、旋转导航、独立按压、USB HID 快捷键，以及 Windows Agent 自动启动/CDC 通信可用。
这属于维护者提供的真机结果；本次发布整理只执行副本构建和离线测试，没有再次连接或烧录开发板。
不据此声称最终 PCB、全部应用键位、长期稳定性、传感器、媒体键或触摸均完成实物验收。

当前演示环境必须显式选 **encoder_preview**。保留原默认环境及触摸实验环境用于复核，它们不是同一运行配置。

- Standby 开机待机；在演示环境，收到首个合法且稳定约 800ms 的 PC status 后自动进入场景主页，每次开机只兜底解锁一次。
- Normal：系统工具和监控；Coding：命令面板、快速打开、终端、注释、Ctrl+D；Browser：标签页/地址栏/刷新/后退；Media：标准 Consumer 控制。
- Control 的四键固定为 Ctrl+C / Ctrl+V / Alt+Tab / Win+D，**Win+D 与 Ctrl+D 是不同动作**。
- CPU/RAM/CPU 温度经 Agent 上送；温度服务不可用发送 null 并显示 N/A，失联显示 Offline。
- 本地 Pomodoro、SHT30 接口、北京时间 UTC+8 时间/未同步占位均保留；天气没有真实 API，显示未配置占位。
- 核心 USB/UI 功能不依赖 WiFi/MQTT。触摸目前保留 RawDiagnostics，不能作为已可用触摸输入宣传。

## 硬件

ESP32-S3 DevKitC-1 N8（8MB Flash，无 PSRAM）、ILI9341 TFT、机械编码器、独立轻触按钮；SHT30 为环境传感器。
当前为开发板飞线，接线和共地见 [docs/WIRING.md](docs/WIRING.md)。原理图/PCB/BOM/Gerber 尚未取得，见 [hardware/README.md](hardware/README.md)。

## 目录

```text
Nexus_S1_Firmware/
  src/              当前固件、共享 LVGL UI、HID、编码器、触摸诊断、时间模块
  include/          LVGL 配置和无凭据模板
  pc_script/        Agent、当前用户启动/卸载、测试与诊断工具
  pc_sim/           共享 ui.cpp/ui.h 的 SDL2 模拟器
  test/             主机测试、模拟 GPIO/SPI/USB 适配层
  platformio.ini    固定依赖版本；encoder_preview 为演示环境
tools/              本地配置生成器
docs/               使用、协议、验证及导出说明
hardware/           设计资料缺项说明
licenses/           第三方许可证原文
SOURCE_MANIFEST.csv 文件及 SHA-256 清单（不对自身哈希）
```

## 构建与烧录

需要 Python 3.9+、PlatformIO Core 和能访问依赖源的网络；项目根目录执行：

```powershell
python -m pip install platformio
python tools/configure_local.py
python -m platformio run -d Nexus_S1_Firmware -e encoder_preview
```

生成器只新建被忽略的 local_secrets.h，生成随机 AP 密码且不打印密码，不覆盖已有配置。
也可复制 Nexus_S1_Firmware/include/local_secrets.h.example 为同目录 local_secrets.h 后自己设置 8～63 字节 AP 密码。MQTT 地址默认 broker.invalid，P1 未配置不影响 USB 核心演示。
不要上传 local_secrets.h、配置过的二进制或本机 Agent 日志。

烧录前停止 Agent 并关闭串口监视器，手动进入下载模式。以下 COM10/COM4 是本次演示端口示例，其他电脑应重新枚举：

```powershell
python Nexus_S1_Firmware/pc_script/agent_monitor.py --stop
python -m platformio run -d Nexus_S1_Firmware -e encoder_preview -t upload --upload-port COM10
# 仅需看诊断时使用；不要与 Agent 同时占用运行时 CDC
python -m platformio device monitor --port COM4 --baud 115200 --dtr 1 --rts 0
```

此环境会发送真实键盘/媒体报告；动作仅作用于电脑当前前台应用，适用键位取决于应用和用户配置。未连接 USB 时不会积压并重放按下动作。

## Windows Agent

```powershell
python -m pip install -r Nexus_S1_Firmware/pc_script/requirements.lock.txt
python Nexus_S1_Firmware/pc_script/agent.py --dry-run --no-temperature
```

手动通信示例（先关闭 monitor/其他串口持有者）：

```powershell
python Nexus_S1_Firmware/pc_script/agent.py --port COM4
```

可选的当前用户 Startup 安装，不需要管理员服务或计划任务：

```powershell
$pythonExe = (Get-Command python.exe -CommandType Application | Select-Object -First 1).Source
powershell -NoProfile -ExecutionPolicy Bypass -File Nexus_S1_Firmware/pc_script/install_agent.ps1 -Python $pythonExe
```

安装器验证依赖、目标 USB 身份和稳定序列号，再创建当前用户快捷方式；串口号变更通过绑定身份重新识别，不写死 COM4。
支持的配置为 Espressif 原生 USB CDC 303A:1001（CDC 子接口）及此前明确支持的 303A:0002；不应把任意同 VID/PID 的 ROM/JTAG 或 USB-UART 当作运行固件。
公开包不包含任何真实设备序列号或已 enrollment 的 config.json。项目和解释器安装后需保持路径稳定。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Nexus_S1_Firmware/pc_script/uninstall_agent.ps1 -Python $pythonExe
```

卸载只移除属于该工具的 Startup 快捷方式并停止其子进程，保留本机配置/日志供本人检查。
CPU 温度可选从本机 LHM HTTP 服务读取；没有服务时使用 null，CPU/RAM/status 仍发送。不会附送或自动安装 LHM。

## 测试与模拟器

Agent 测试不打开真实串口、不启动真实 Windows Startup：

```powershell
python -m unittest discover -s Nexus_S1_Firmware/pc_script -p "test_agent*.py" -v
```

主机端 C++/LVGL 测试需 MinGW GCC，先设置 NEXUS_HOST_CXX_DIR 为自己编译器 bin 目录：

```powershell
python -m platformio pkg install -d Nexus_S1_Firmware -e esp32-s3-devkitc-1
python Nexus_S1_Firmware/pc_script/check_firmware_link.py
python Nexus_S1_Firmware/pc_script/test_encoder.py
python Nexus_S1_Firmware/pc_script/test_ui_navigation.py
```

模拟器另需 SDL2 开发头文件、导入库和 DLL，必要时设置 SDL2_ROOT；它复用固件 UI，只记录宏意图，不向 Windows 发送快捷键：

```powershell
python Nexus_S1_Firmware/pc_sim/build.py --run
```

所有测试及模拟器输入均为软件证据，不等于开发板/最终 PCB 验收。见 [docs/VALIDATION.md](docs/VALIDATION.md)。

## 开源与来源

项目自身源码采用 MIT；第三方按各自许可证，见 LICENSE、AUTHORS.md 与 THIRD_PARTY_NOTICES.md。
本包来自当前工作树且包含当时未提交源码，不带旧 Git 历史和旧比赛私有资料。配置外置/路径去个人化等发布差异见 docs/EXPORT_NOTES.md。
硬件设计文件尚缺，本包不能当作完整 PCB 制造资料。
