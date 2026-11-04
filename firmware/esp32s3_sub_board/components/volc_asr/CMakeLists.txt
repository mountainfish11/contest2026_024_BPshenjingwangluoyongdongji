idf_component_register(
    SRCS "volc_asr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-tls esp_netif mbedtls nvs_flash json
)
