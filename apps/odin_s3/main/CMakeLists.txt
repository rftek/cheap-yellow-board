# wavetables.bin (Odin 2's wavetables, from tools/odin_convert.py) is
# embedded in the image as _binary_wavetables_bin_start.
idf_component_register(SRCS "main.c" "ui.c" "audio.c" "engine.c" "filters.c" "fx.c" "presets.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "wavetables.bin"
                       PRIV_REQUIRES cyd_display esp_timer music_out nvs_flash)
