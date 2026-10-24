/****************************************************************************
 * Contest 2026 team 024 - BlindBadge server-assisted vision
 ****************************************************************************/

#ifndef BLIND_BADGE_VISION_H
#define BLIND_BADGE_VISION_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int blind_badge_vision_start(const char *host, int port);
int blind_badge_vision_once(const char *host, int port);
int blind_badge_vision_stop(void);
int blind_badge_vision_status(void);

#endif /* BLIND_BADGE_VISION_H */
