# 第三方来源与许可证

本包不分发 PlatformIO 包缓存、Arduino/ESP-IDF SDK、库源文件副本、编译器、SDL DLL 或固件二进制。
下列组件由依赖管理器或开发者从上游安装。项目使用它们，不宣称拥有它们的原创权。
可核对的依赖许可证原文保存在 licenses/third_party/；具体文件中的版权及例外条款优先。

| 组件 | 本次验证版本 | 许可证/上游 |
|---|---|---|
| PlatformIO Espressif32 平台 | 6.13.0 | 构建工具；https://github.com/platformio/platform-espressif32 |
| Arduino-ESP32（USB、SPI、Wire、WiFi 等） | Arduino 2.0.17 | LGPL-2.1，SDK 内其他组件各自许可；https://github.com/espressif/arduino-esp32/tree/2.0.17 |
| ESP-IDF / TinyUSB | 上述 SDK 所带版本 | ESP-IDF Apache-2.0、TinyUSB MIT，其他子组件遵循各自 LICENSE；https://github.com/espressif/esp-idf / https://github.com/hathach/tinyusb |
| LVGL | 8.3.11 | MIT；https://github.com/lvgl/lvgl/tree/v8.3.11 |
| TFT_eSPI | 2.5.43 | 上游 license.txt 包含 FreeBSD、MIT 与 Adafruit 派生代码声明；https://github.com/Bodmer/TFT_eSPI |
| WiFiManager | 2.0.17 | MIT；https://github.com/tzapu/WiFiManager |
| Adafruit SHT31 Library | 2.2.2 | BSD-3-Clause；https://github.com/adafruit/Adafruit_SHT31 |
| Adafruit BusIO | 1.17.4 | MIT；https://github.com/adafruit/Adafruit_BusIO |
| ArduinoJson | 7.4.3 | MIT；https://github.com/bblanchon/ArduinoJson |
| PubSubClient | 2.8 | MIT；https://github.com/knolleary/pubsubclient |
| psutil / pyserial / PyGetWindow / PyRect | 见 requirements.lock.txt | BSD 系列；https://github.com/giampaolo/psutil / https://github.com/pyserial/pyserial / https://github.com/asweigart/PyGetWindow / https://github.com/asweigart/PyRect |
| Requests 及其传递依赖 | 见 requirements.lock.txt | Requests Apache-2.0，其他包见所附许可证；https://github.com/psf/requests |
| SDL2（仅 PC 模拟器） | 开发者另行安装 | zlib；https://github.com/libsdl-org/SDL/tree/SDL2 |

其中 certifi 的 CA 集合有独立 MPL-2.0 条款，不能简单归类为项目 MIT；保留其上游许可证。
后续若分发链接 SDK 的二进制，须另行保留适用版权、源码/重链接材料及其他许可证要求；本次源码 ZIP 不含二进制。
旧 Web 中的 Chart.js 及来源不明的历史资料没有随包分发，因为它们不属于当前 USB 演示链路。
