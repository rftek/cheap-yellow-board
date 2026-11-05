#include "core/bus_HAL.h"
#include "core/powerSave.h"
#include "core/utils.h"
#include <Arduino.h>
#include <Wire.h>
#include <interface.h>

// =============================================================================
//  Waveshare ESP32-S3-Touch-LCD-7 interface
//  - Display:   800x480 RGB panel (Arduino_GFX, see pins_arduino.h)
//  - Expander:  CH422G on I2C 8/9: touch reset, backlight (on/off only),
//               LCD reset, TF card CS, native USB/CAN select
//  - Touch:     GT911 on the same I2C bus, INT=4, reset through the expander
// =============================================================================

#include "TouchDrvGT911.hpp"
TouchDrvGT911 touch;
static bool touchReady = false;

// The expander's outputs are written as one byte, so keep the levels here.
// EXIO5 stays high so driving the expander doesn't move the native USB port
// over to CAN; EXIO4 (TF card CS) idles high.
static uint8_t expanderLevels = EXIO_LCD_RESET | EXIO_SD_CS | EXIO_USB_SELECT;
static SemaphoreHandle_t expanderMutex = nullptr;

static void expanderWrite(uint8_t set, uint8_t clear) {
    if (expanderMutex) xSemaphoreTake(expanderMutex, portMAX_DELAY);
    expanderLevels = (expanderLevels | set) & ~clear;
    Wire1.beginTransmission(CH422G_OUTPUT_ADDRESS);
    Wire1.write(expanderLevels);
    Wire1.endTransmission();
    if (expanderMutex) xSemaphoreGive(expanderMutex);
}

// Called by lib/HAL/sd_card/sd_diskio.cpp when the SD card's CS isn't a GPIO.
void sdExternalCS(bool high) {
    if (high) expanderWrite(EXIO_SD_CS, 0);
    else expanderWrite(0, EXIO_SD_CS);
}

/***************************************************************************************
** Function name: _setup_gpio()
** Brings up I2C, the CH422G (backlight off) and the GT911, as Waveshare's demo does.
***************************************************************************************/
void _setup_gpio() {
    bruceConfig.colorInverted = 0;
    expanderMutex = xSemaphoreCreateMutex();

    setSysI2CBus(&Wire1);
    Wire1.begin(SYS_I2C_SDA, SYS_I2C_SCL, 400000);

    Wire1.beginTransmission(CH422G_MODE_ADDRESS);
    Wire1.write(CH422G_MODE_OUTPUT);
    if (Wire1.endTransmission() != 0) Serial.println("CH422G expander not found");

    // GT911 reset: INT low while reset is released selects address 0x5D.
    pinMode(BOARD_TOUCH_INT, OUTPUT);
    expanderWrite(0, EXIO_TOUCH_RESET | EXIO_BACKLIGHT);
    delay(100);
    digitalWrite(BOARD_TOUCH_INT, LOW);
    delay(100);
    expanderWrite(EXIO_TOUCH_RESET, 0);
    delay(200);
    pinMode(BOARD_TOUCH_INT, INPUT);

    touch.setPins(-1, BOARD_TOUCH_INT);
    touchReady = touch.begin(Wire1, GT911_SLAVE_ADDRESS_L, SYS_I2C_SDA, SYS_I2C_SCL);
    Serial.println(touchReady ? "GT911 touch started" : "Failed to find GT911 touch");
}

/***************************************************************************************
** Function name: _pre_storage_gpio()
** The panel has been cleared by now, so the backlight can come on.
***************************************************************************************/
void _pre_storage_gpio() { expanderWrite(EXIO_BACKLIGHT, 0); }

void _post_setup_gpio() {}

int getBattery() { return 100; }

/*********************************************************************
** Function: _setBrightness
** The backlight is only switched on or off (EXIO2).
**********************************************************************/
void _setBrightness(uint8_t brightval) {
    if (brightval > 0) expanderWrite(EXIO_BACKLIGHT, 0);
    else expanderWrite(0, EXIO_BACKLIGHT);
}

/*********************************************************************
** Function: InputHandler (GT911 capacitive)
** The GT911 reports panel coordinates (800x480, Bruce's rotation 1);
** map them for the other rotations.
**********************************************************************/
void InputHandler(void) {
    static long d_tmp = 0;
    if (!touchReady) return;
    if (millis() - d_tmp > 200 || LongPress) {
        static unsigned long tm = millis();
        static bool lastTouchState = false;
        static unsigned long lastTouchTime = 0;
        int16_t rx = 0, ry = 0;

        uint8_t touched = touch.getPoint(&rx, &ry);
        bool currentTouchState = touched > 0;

        if (currentTouchState && !lastTouchState && (millis() - lastTouchTime) > 100) {
            lastTouchTime = millis();
        } else if (!currentTouchState || lastTouchState) {
            touched = 0;
        }
        lastTouchState = currentTouchState;

        if (((millis() - tm) > 190 || LongPress) && touched) {
            tm = millis();
            if (!wakeUpScreen()) AnyKeyPress = true;
            else goto END;

            switch (bruceConfigPins.rotation) {
                case 3:
                    touchPoint.x = TFT_RGB_NATIVE_WIDTH - 1 - rx;
                    touchPoint.y = TFT_RGB_NATIVE_HEIGHT - 1 - ry;
                    break;
                case 0:
                    touchPoint.x = TFT_RGB_NATIVE_HEIGHT - 1 - ry;
                    touchPoint.y = rx;
                    break;
                case 2:
                    touchPoint.x = ry;
                    touchPoint.y = TFT_RGB_NATIVE_WIDTH - 1 - rx;
                    break;
                default:
                    touchPoint.x = rx;
                    touchPoint.y = ry;
                    break;
            }
            touchPoint.pressed = true;
            touchHeatMap(touchPoint);
        END:
            d_tmp = millis();
        }
    }
}

void powerOff() {}

void checkReboot() {}
