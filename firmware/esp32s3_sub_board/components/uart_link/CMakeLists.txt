idf_component_register(
    SRCS "uart_link.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi esp_netif lwip
)
