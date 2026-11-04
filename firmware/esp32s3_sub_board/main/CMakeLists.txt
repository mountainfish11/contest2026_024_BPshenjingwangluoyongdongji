idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES atgm336h m100m max30102 vibration_motor wifi uart_link
             speech_link speech_output volc_asr esp_driver_uart
)
