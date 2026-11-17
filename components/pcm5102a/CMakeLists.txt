idf_component_register(SRCS "pcm5102a.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES esp_driver_i2s)
