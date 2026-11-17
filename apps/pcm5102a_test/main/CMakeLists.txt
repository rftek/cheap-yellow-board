idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES cyd_display pcm5102a esp_driver_gpio esp_timer esp_rom)
