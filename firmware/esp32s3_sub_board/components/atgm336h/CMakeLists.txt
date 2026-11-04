idf_component_register(
    SRCS "atgm336h.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_uart esp_timer
)
