idf_component_register(SRCS "main.c" "ui.c" "audio.c" "looper.c" "synth.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES cyd_display esp_timer music_out nvs_flash)
