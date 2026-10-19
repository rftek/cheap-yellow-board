idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt cyd_display esp_event esp_netif esp_wifi lwip
                                     nvs_flash)
