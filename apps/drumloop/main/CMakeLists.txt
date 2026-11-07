idf_component_register(SRCS "main.c" "ui.c" "audio.c" "engine.c" "kit.c" "export.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES airplay_sender cyd_display esp_event esp_netif esp_wifi
                                     nvs_flash)
