file(TO_CMAKE_PATH "$ENV{USERPROFILE}" CYD_USER_PROFILE)
set(CYD_PRIVATE_DIR "${CYD_USER_PROFILE}/.cyd_private")
if(NOT EXISTS "${CYD_PRIVATE_DIR}/wifi_credentials.h")
    message(FATAL_ERROR
        "Create ${CYD_PRIVATE_DIR}/wifi_credentials.h from wifi_credentials.example.h")
endif()

idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "." "${CYD_PRIVATE_DIR}"
                       PRIV_REQUIRES cyd_display esp_event esp_http_client esp_netif
                                     esp_rom esp_timer esp_wifi mbedtls nvs_flash)
