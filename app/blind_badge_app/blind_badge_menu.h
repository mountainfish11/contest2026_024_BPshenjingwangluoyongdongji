/****************************************************************************
 * Contest 2026 team 024 - BlindBadge menu model
 ****************************************************************************/

#ifndef BLIND_BADGE_MENU_H
#define BLIND_BADGE_MENU_H

enum blind_badge_menu_id
{
  BLIND_BADGE_MENU_VISION = 0,
  BLIND_BADGE_MENU_VOICE,
  BLIND_BADGE_MENU_HEALTH,
  BLIND_BADGE_MENU_ALERT,
  BLIND_BADGE_MENU_SETTINGS,
  BLIND_BADGE_MENU_TEST
};

int blind_badge_menu_show(void);
int blind_badge_menu_next(void);
enum blind_badge_menu_id blind_badge_menu_selected(void);

/* PLAY single click: start the preset service of the selected entry and
 * switch to its status page.  PLAY double click: stop the service and go
 * back to the menu list.
 */

int blind_badge_menu_enter(void);
int blind_badge_menu_leave(void);

/* Ask the button/menu thread to leave the current service on its next refresh.
 * This keeps display and service teardown out of the UART callback thread.
 */

void blind_badge_menu_request_leave(void);

/* Post a non-blocking emergency preemption request from a detector. */

void blind_badge_menu_request_alert(void);

/* Redraw the service page only when its state or detail line changed.
 * No-op while the menu list is visible.
 */

int blind_badge_menu_refresh(void);
int blind_badge_menu_is_service_page(void);

#endif /* BLIND_BADGE_MENU_H */
