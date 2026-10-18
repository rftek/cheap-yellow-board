idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt esp_driver_gpio esp_driver_spi
                                     esp_event esp_rom esp_wifi nvs_flash)
