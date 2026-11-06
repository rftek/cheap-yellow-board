# One backend per board, chosen by chip target.
if(IDF_TARGET STREQUAL "esp32")
    # ESP32 Cheap Yellow Display
    set(backend "cyd_display_esp32_cyd.c")
    set(backend_requires esp_driver_gpio esp_driver_ledc esp_driver_spi esp_rom nvs_flash)
elseif(IDF_TARGET STREQUAL "esp32s3")
    # Waveshare ESP32-S3-Touch-LCD-7
    set(backend "cyd_display_esp32s3_lcd7.c")
    set(backend_requires esp_driver_gpio esp_driver_i2c esp_lcd esp_rom nvs_flash)
else()
    message(FATAL_ERROR "cyd_display has no board backend for ${IDF_TARGET}")
endif()

idf_component_register(SRCS "cyd_display_common.c" "${backend}"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${backend_requires})
