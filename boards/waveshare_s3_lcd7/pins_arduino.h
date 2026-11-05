#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include "soc/soc_caps.h"
#include <stdint.h>

#ifndef DEVICE_NAME
#define DEVICE_NAME "Waveshare S3 LCD 7"
#endif

// =============================================================================
//  Waveshare ESP32-S3-Touch-LCD-7
//  ESP32-S3-WROOM-1-N16R8, 800x480 16-bit RGB panel (Arduino_GFX backend),
//  GT911 touch and CH422G I/O expander on I2C GPIO8/9. Pins and panel
//  timings follow Waveshare's ESP-IDF demo (09_lvgl_v9_demo).
// =============================================================================

static const uint8_t TX = 43;
static const uint8_t RX = 44;

// I2C: GT911, CH422G and the I2C header share this bus.
static const uint8_t SDA = 8;
static const uint8_t SCL = 9;
#define GROVE_SDA 8
#define GROVE_SCL 9

// TF card SPI (its CS is EXIO4 on the CH422G).
static const uint8_t SS = 255;
static const uint8_t MOSI = 11;
static const uint8_t MISO = 13;
static const uint8_t SCK = 12;

// ---- Display: Arduino_GFX RGB panel ----
#define USE_ARDUINO_GFX 1
#define RGB_PANEL 1
#define TFT_DATABUS_N 3
#define TFT_DISPLAY_DRIVER_N 49 // Arduino_RGB_Display
// The panel scans out 800x480 natively (Bruce's rotation 1); TFT_WIDTH/TFT_HEIGHT
// (480/800, set in the .ini) follow Bruce's portrait convention.
#define TFT_RGB_NATIVE_WIDTH 800
#define TFT_RGB_NATIVE_HEIGHT 480
#define TFT_ROTATION_OFFSET 3
#define TFT_RGB_BOUNCE_BUFFER_PX (800 * 10)

#define TFT_DE 5
#define TFT_VSYNC 3
#define TFT_HSYNC 46
#define TFT_PCLK 7
// esp_lcd data order D0-D15 = B0-B4, G0-G5, R0-R4 (the panel's B3-B7, G2-G7, R3-R7)
#define TFT_B0 14
#define TFT_B1 38
#define TFT_B2 18
#define TFT_B3 17
#define TFT_B4 10
#define TFT_G0 39
#define TFT_G1 0
#define TFT_G2 45
#define TFT_G3 48
#define TFT_G4 47
#define TFT_G5 21
#define TFT_R0 1
#define TFT_R1 2
#define TFT_R2 42
#define TFT_R3 41
#define TFT_R4 40
#define TFT_HSYNC_POL 0
#define TFT_HSYNC_FRONT_PORCH 8
#define TFT_HSYNC_PULSE_WIDTH 4
#define TFT_HSYNC_BACK_PORCH 8
#define TFT_VSYNC_POL 0
#define TFT_VSYNC_FRONT_PORCH 8
#define TFT_VSYNC_PULSE_WIDTH 4
#define TFT_VSYNC_BACK_PORCH 8
#define TFT_PCLK_ACTIVE_NEG 1
#define TFT_PREF_SPEED 16000000
#define TFT_RST -1

// ---- CH422G I/O expander ----
// A byte written to the mode address makes EXIO0-7 outputs; the levels of all
// eight are then written as one byte to the output address.
#define CH422G_MODE_ADDRESS 0x24
#define CH422G_MODE_OUTPUT 0x01
#define CH422G_OUTPUT_ADDRESS 0x38
#define EXIO_TOUCH_RESET (1 << 1) // low holds the GT911 in reset
#define EXIO_BACKLIGHT (1 << 2)
#define EXIO_LCD_RESET (1 << 3)
#define EXIO_SD_CS (1 << 4)
#define EXIO_USB_SELECT (1 << 5) // kept high (its pulled-up default)

#endif /* Pins_Arduino_h */
