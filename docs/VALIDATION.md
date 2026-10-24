# 发布副本验证（2026-10-24）

以下结果必须与维护者的实物反馈区分。本轮没有连接串口、操作开发板或发送真实 Windows HID。

## 本轮实际执行

| 检查 | 命令（源码包根目录） | 结果 |
|---|---|---|
| 独立副本固件 | `platformio run -d Nexus_S1_Firmware -e encoder_preview` | 成功；全新项目构建目录，Espressif32 6.13.0 / Arduino 2.0.17 / LVGL 8.3.11 |
| Agent 与启动绑定 | `python -m unittest discover -s Nexus_S1_Firmware/pc_script -p "test_agent*.py" -v` | 27/27；模拟端口、前台窗口、依赖失败；Startup 仅写测试临时目录 |
| 默认 CDC/LVGL | `python Nexus_S1_Firmware/pc_script/check_firmware_link.py` | 实际解析器与真实 LVGL，模拟 USB；默认 21 种动作、释放、分帧、Offline/恢复 |
| 编码器/HID | `python Nexus_S1_Firmware/pc_script/test_encoder.py` | 实际适配层+LVGL，22 种动作；开启/关闭 HID、焦点先移动、30ms 释放、长短按、CDC 并行；模拟 GPIO/USB |
| 导航 | `python Nexus_S1_Firmware/pc_script/test_ui_navigation.py` | 四场景、防抖、Standby、父级、离线、重复与回绕 |
| 触摸/诊断 | `test_xpt2046.py`、`test_touch_live.py`、`test_touch_spi_diag.py` | 主机合成数据回归；不代表 XPT2046 通信或校准通过 |
| 时间/日志采集器 | `test_ambient_clock.py`、`test_collect_xpt2046_ab.py` | 合成时间/日志；采集器 9/9，不连接串口 |
| SDL2 模拟器 | `python Nexus_S1_Firmware/pc_sim/build.py --test --build-name pc_sim_release` | 构建及交互回归通过；真实共享 LVGL UI、合成场景，非实物验收 |

实际日志保留在维护者交付目录的私有 validation/，不随公开源码包分发。
构建 RAM 167,748 / 327,680 字节（51.2%），Flash 1,300,185 / 3,342,336 字节（38.9%）。
存在原有 ARDUINO_USB_MODE 重复定义和 TFT_eSPI 内置 TOUCH_CS 未配置警告；项目使用自己的触摸诊断适配层。
本次没有修改这些硬件/USB 配置。没有失败构建或编译错误。
首次并行启动 SPI 诊断主机测试时，共享 LVGL 对象尚未生成，前置检查曾失败；等待 CDC/LVGL 回归生成后按顺序复跑通过，没有为此修改源代码或删减断言。

## 仍须保留的边界

维护者报告已真机验证：TFT、PC 场景切换、旋转、独立轻触按钮、USB HID 快捷键，以及 Agent 自动启动/CDC。
导出副本仅更换本地配置/去个人化/固定依赖，不把这次主机构建当作再次实物验收。
最终 PCB、全量应用键位/媒体键、长期插拔并发、SHT30 接线读数、真实触摸与校准仍需各自证据；天气无真实服务。
源码 ZIP 不含本次生成的本地秘密配置、已配置固件或依赖缓存，开发者须按 README 先生成自己的配置。
