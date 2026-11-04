/****************************************************************************
 * Contest 2026 team 024 - BlindBadge pull-based vision server
 ****************************************************************************/

#ifndef BLIND_BADGE_VISION_SERVER_H
#define BLIND_BADGE_VISION_SERVER_H

#include <stdbool.h>
#include <stddef.h>

struct blind_badge_vision_server_snapshot
{
  bool running;
  bool camera_ready;
  bool listening;
  bool stream_active;
  int port;
  int last_error;
  unsigned long frames;
  char address[16];
};

int blind_badge_vision_server_run(int port);
int blind_badge_vision_server_stop(void);
int blind_badge_vision_server_status(void);
int blind_badge_vision_server_prepare(void);
int blind_badge_vision_server_snapshot(
  struct blind_badge_vision_server_snapshot *snapshot);
int blind_badge_vision_camera_test(void);
int blind_badge_vision_camera_bench(unsigned int frames);

#endif /* BLIND_BADGE_VISION_SERVER_H */
