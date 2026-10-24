#pragma once

// --- [屏幕 SPI 接口 (LVGL)] ---
#define SPI_CLK_PIN    12
#define SPI_MOSI_PIN   11
#define SPI_CS_PIN     10
#define SPI_DC_PIN     9
#define SPI_RST_PIN    14
#define SPI_BLK_PIN    13  // 屏幕背光控制

// --- [传感器与触摸 I2C 接口] ---
#define I2C_SDA_PIN    4
#define I2C_SCL_PIN    5
#define TOUCH_INT_PIN  6   // 触摸中断引脚

// --- [麦克风 INMP441 (I2S 输入)] ---
#define MIC_I2S_BCLK   42
#define MIC_I2S_WS     41
#define MIC_I2S_DATA   40

// --- [功放 MAX98357A (I2S 输出)] ---
#define AMP_I2S_BCLK   15
#define AMP_I2S_LRC    16
#define AMP_I2S_DIN    17

// --- [原生 USB (HID 模拟键盘)] ---
// 注意：ESP32-S3 底层自动接管 19 和 20 作为 USB，
// 通常不需要在代码里 `#define` 这两个引脚，只需配置好 USB 库即可。
#define USB_DN_PIN     19
#define USB_DP_PIN     20
