idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES airplay_sender cyd_display esp_event esp_netif esp_wifi
                                     nvs_flash)
