#ifndef __BLIND_BADGE_UART_LINK_H
#define __BLIND_BADGE_UART_LINK_H

/* UART_LINK is the common application protocol name. The current transport
 * is Wi-Fi TCP/UDP because the ESP32-S3-EYE exposes no usable expansion UART. */

int UART_LINK_Start(void);
int UART_LINK_Wait(void);
int UART_LINK_Stop(void);
int UART_LINK_IsConnected(void);
int UART_LINK_SendFlag(const char *flag);

enum uart_link_event_type
{
  UART_LINK_EVENT_OBSTACLE = 0,
  UART_LINK_EVENT_STEP_DOWN,
  UART_LINK_EVENT_EMERGENCY
};

struct uart_link_event
{
  enum uart_link_event_type type;
  int distance_cm;
  int confidence;
  char direction[16];
  char source[16];
};

typedef void (*uart_link_event_handler_t)(const struct uart_link_event *event,
                                          void *arg);

void UART_LINK_SetEventHandler(uart_link_event_handler_t handler, void *arg);

#endif
