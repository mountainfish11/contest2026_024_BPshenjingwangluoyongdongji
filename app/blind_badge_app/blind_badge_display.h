/****************************************************************************
 * Contest 2026 team 024 - BlindBadge display output
 ****************************************************************************/

#ifndef BLIND_BADGE_DISPLAY_H
#define BLIND_BADGE_DISPLAY_H

#include "blind_badge_event.h"

int blind_badge_display_lcd_test(void);
int blind_badge_display_lcd_test_zh(void);
int blind_badge_display_show_lines(const char *line0,
                                   const char *line1,
                                   const char *line2,
                                   const char *line3);
int blind_badge_display_show_boot_status(int connected);
int blind_badge_display_show_event(const struct blind_badge_event *event,
                                   const char *message);
int blind_badge_display_show_ai_event(const struct blind_badge_event *event,
                                      const char *ai_response,
                                      int used_fallback);
int blind_badge_display_show_ai_wait(const struct blind_badge_event *event);
void ai_agent_display_ask_result(const char *response, int success);

#endif /* BLIND_BADGE_DISPLAY_H */
