idf_component_register(SRCS "main.c" "ui.c" "output.c" "audio.c" "engine.c" "kit.c" "export.c"
                            "sdcard.c" "wav.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES airplay_sender cyd_display esp_driver_sdspi esp_driver_spi
                                     esp_event esp_netif esp_timer esp_wifi fatfs nvs_flash sdmmc)
