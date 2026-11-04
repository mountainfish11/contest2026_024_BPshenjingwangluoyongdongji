idf_component_register(
    SRCS "m100m.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer
)
