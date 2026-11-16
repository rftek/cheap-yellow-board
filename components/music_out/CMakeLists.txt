idf_component_register(SRCS "music_out.c" "music_out_stream.c"
                       INCLUDE_DIRS "include"
                       REQUIRES airplay_sender
                       PRIV_REQUIRES cyd_display esp_driver_gpio esp_event esp_netif esp_rom esp_timer esp_wifi
                                     nvs_flash pcm5102a)
