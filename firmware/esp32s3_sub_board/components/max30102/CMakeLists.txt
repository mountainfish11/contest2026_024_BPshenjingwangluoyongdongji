idf_component_register(
    SRCS "max30102.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_timer
)
