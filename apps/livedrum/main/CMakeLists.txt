# LiveDrum's own files, plus DrumLoop's kits, WAV reader and SD card,
# compiled from that app's folder. Drum&Key compiles the ld_*.c files from
# here for its LIVE screen.
set(DRUMLOOP "${CMAKE_CURRENT_LIST_DIR}/../../drumloop/main")
idf_component_register(SRCS "main.c" "ld_engine.c" "ld_sounds.c" "ld_ui.c"
                            "${DRUMLOOP}/kit.c" "${DRUMLOOP}/sdcard.c" "${DRUMLOOP}/wav.c"
                       INCLUDE_DIRS "." "${DRUMLOOP}"
                       PRIV_REQUIRES airplay_sender cyd_display music_out esp_driver_sdspi esp_driver_spi
                                     esp_event esp_netif esp_timer esp_wifi fatfs nvs_flash sdmmc)
