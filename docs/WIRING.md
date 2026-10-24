# 开发板飞线接线

平台为 ESP32-S3 DevKitC-1 N8（8MB Flash、无 PSRAM），不是最终 PCB。

| 信号 | GPIO/连接 | 说明 |
|---|---|---|
| TFT SCLK / Touch T_CLK | 12 | 共用 SPI 时钟 |
| TFT SDI / Touch T_DIN | 11 | 共用 MOSI |
| TFT CS | 10 | 与 Touch CS 独立 |
| TFT DC / RST | 9 / 14 | 保持已工作的显示接线 |
| XPT2046 T_CS / T_DO | 7 / 8 | 仍为 RAW 诊断，T_IRQ 未连接 |
| 编码器 A / B | 1 / 2 | INPUT_PULLUP，4 个有效正交边沿/步，方向可配置 |
| 编码器公共端 C | GND | C 不是按压输出，不接 GPIO21 |
| 独立轻触按钮信号 | 21 | INPUT_PULLUP，按下接 GND，30ms 消抖 |
| 独立按钮另一端 | GND | 与开发板共地 |
| SHT30 SDA / SCL | 4 / 5 | I2C，默认地址 0x44，传感器验收另行确认 |
| 原生 USB D- / D+ | 19 / 20 | 保留原生 USB，不作其他外设 GPIO |

背光配置保留 GPIO13，但当前模块实际供电/背光接法应按已工作的实线确认；此前使用过 3V3 直接点亮，不因此声称 PWM 已完成。不要为匹配文档改动正常背光。
3.3V 逻辑和共地；不能把带 5V 上拉的模块输出直接接 ESP32 输入。
board_config.h 中音频等宏属于历史预留，不表示驱动已启用。GPIO6 的旧 TOUCH_INT_PIN 不是 XPT2046 的完整配置。

普通短按确认只生成一次动作；长按约 800ms 返回，松开不会额外确认。普通返回保留 Pomodoro，只有明确 EXIT 停止。
encoder_preview 保持正常 TFT 刷新、触摸 RAW、不提交触摸点击，并保留开机首个稳定场景自动进入兜底。
