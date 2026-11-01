# One backend per board, chosen by chip target. So far only the ESP32 Cheap
# Yellow Display has one.
if(IDF_TARGET STREQUAL "esp32")
    set(backend "cyd_display_esp32_cyd.c")
else()
    message(FATAL_ERROR "cyd_display has no board backend for ${IDF_TARGET}")
endif()

idf_component_register(SRCS "cyd_display_common.c" "${backend}"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi esp_rom
                                     nvs_flash)
