/****************************************************************************
 * Contest 2026 team 024 - BlindBadge display output
 ****************************************************************************/

#ifndef BLIND_BADGE_DISPLAY_H
#define BLIND_BADGE_DISPLAY_H

#include "blind_badge_event.h"

#include <stddef.h>

enum blind_badge_menu_icon
{
  BLIND_BADGE_MENU_ICON_CAMERA = 0,
  BLIND_BADGE_MENU_ICON_MICROPHONE,
  BLIND_BADGE_MENU_ICON_HEART,
  BLIND_BADGE_MENU_ICON_BELL,
  BLIND_BADGE_MENU_ICON_SETTINGS,
  BLIND_BADGE_MENU_ICON_TEST
};

int blind_badge_display_lcd_test(void);
int blind_badge_display_lcd_test_zh(void);
int blind_badge_display_show_lines(const char *line0,
                                   const char *line1,
                                   const char *line2,
                                   const char *line3);
int blind_badge_display_show_boot_status(int connected);
int blind_badge_display_show_menu(
  const char *const labels[],
  const enum blind_badge_menu_icon icons[],
  size_t item_count, size_t selected);
int blind_badge_display_animate_menu_next(
  const char *const labels[],
  const enum blind_badge_menu_icon icons[],
  size_t item_count, size_t selected);
int blind_badge_display_show_service(const char *title,
                                     const char *state,
                                     const char *detail,
                                     const char *hint);
int blind_badge_display_show_passive_alert(const char *state,
                                           const char *detail);
int blind_badge_display_show_event(const struct blind_badge_event *event,
                                   const char *message);
int blind_badge_display_show_ai_event(const struct blind_badge_event *event,
                                      const char *ai_response,
                                      int used_fallback);
int blind_badge_display_show_ai_wait(const struct blind_badge_event *event);
void ai_agent_display_ask_result(const char *response, int success);

#endif /* BLIND_BADGE_DISPLAY_H */
