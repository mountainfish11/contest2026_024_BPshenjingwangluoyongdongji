#ifndef __BLIND_BADGE_UART_LINK_H
#define __BLIND_BADGE_UART_LINK_H

/* UART_LINK is the common application protocol name. The current transport
 * is Wi-Fi TCP/UDP because the ESP32-S3-EYE exposes no usable expansion UART. */

int UART_LINK_Start(void);
int UART_LINK_Wait(void);
int UART_LINK_Stop(void);
int UART_LINK_IsConnected(void);

#endif
