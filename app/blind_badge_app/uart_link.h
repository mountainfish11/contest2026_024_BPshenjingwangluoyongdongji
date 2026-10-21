#ifndef __BLIND_BADGE_UART_LINK_H
#define __BLIND_BADGE_UART_LINK_H

#include <stddef.h>
#include <stdint.h>

int UART_LINK_Start(void);
int UART_LINK_Wait(void);
int UART_LINK_Stop(void);
int UART_LINK_IsConnected(void);
int UART_LINK_SendFlag(const char *flag);

/* Send PCM data to sub-board. Returns bytes sent or -1 on error. */
int UART_LINK_SendPCM(const uint8_t *pcm, size_t pcm_len);

enum uart_link_event_type
{
  UART_LINK_EVENT_OBSTACLE = 0,
  UART_LINK_EVENT_STEP_DOWN,
  UART_LINK_EVENT_EMERGENCY,
  UART_LINK_EVENT_ASR_RESULT
};

struct uart_link_event
{
  enum uart_link_event_type type;
  int distance_cm;
  int confidence;
  char direction[16];
  char source[16];
  char text[96];
};

typedef void (*uart_link_event_handler_t)(const struct uart_link_event *event,
                                          void *arg);

void UART_LINK_SetEventHandler(uart_link_event_handler_t handler, void *arg);

#endif
