idf_component_register(SRCS "cyd_display.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi esp_rom
                                     nvs_flash)
