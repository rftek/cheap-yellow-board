# Drum&Key's own files, plus the engines of DrumLoop and Keyboard compiled
# from their apps' folders, so a fix there reaches all three.
set(DRUMLOOP "${CMAKE_CURRENT_LIST_DIR}/../../drumloop/main")
set(KEYBOARD "${CMAKE_CURRENT_LIST_DIR}/../../keyboard/main")
idf_component_register(SRCS "main.c" "dk_audio.c" "dk_ui.c" "drums_ui.c" "keys_ui.c"
                            "${DRUMLOOP}/engine.c" "${DRUMLOOP}/kit.c" "${DRUMLOOP}/export.c"
                            "${DRUMLOOP}/sdcard.c" "${DRUMLOOP}/wav.c"
                            "${KEYBOARD}/looper.c" "${KEYBOARD}/synth.c"
                       INCLUDE_DIRS "." "${DRUMLOOP}" "${KEYBOARD}"
                       PRIV_REQUIRES airplay_sender cyd_display music_out esp_driver_sdspi esp_driver_spi
                                     esp_event esp_netif esp_timer esp_wifi fatfs nvs_flash sdmmc)
