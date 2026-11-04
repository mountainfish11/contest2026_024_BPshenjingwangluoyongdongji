idf_component_register(
    SRCS "vibration_motor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer
)
