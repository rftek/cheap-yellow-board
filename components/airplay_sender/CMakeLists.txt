# AirPlay 1 (RAOP) audio sender. mDNS comes from the component manager
# (idf_component.yml); Curve25519 for /auth-setup from mbedTLS (PSA).
idf_component_register(SRCS "airplay_sender.c" "airplay_discover.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif freertos
                       PRIV_REQUIRES esp_hw_support esp_timer heap log lwip mbedtls)
