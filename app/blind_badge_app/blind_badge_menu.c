/****************************************************************************
 * Contest 2026 team 024 - BlindBadge menu model
 ****************************************************************************/

#include "blind_badge_menu.h"
#include "blind_badge_display.h"
#include "blind_badge_service.h"
#include "uart_link.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define BLIND_BADGE_MENU_STATE_SIZE 16
#define BLIND_BADGE_MENU_DETAIL_SIZE 40

enum blind_badge_menu_page
{
  BLIND_BADGE_MENU_PAGE_LIST = 0,
  BLIND_BADGE_MENU_PAGE_SERVICE
};

enum blind_badge_speech_mode
{
  BLIND_BADGE_SPEECH_MODE_MENU = 0,
  BLIND_BADGE_SPEECH_MODE_VISION,
  BLIND_BADGE_SPEECH_MODE_VOICE,
  BLIND_BADGE_SPEECH_MODE_HEALTH,
  BLIND_BADGE_SPEECH_MODE_ALERT,
  BLIND_BADGE_SPEECH_MODE_SILENT
};

struct blind_badge_menu_item
{
  enum blind_badge_menu_id id;
  enum blind_badge_menu_icon icon;
  const char *label;
};

static const struct blind_badge_menu_item g_menu_items[] =
{
  {
    BLIND_BADGE_MENU_VISION,
    BLIND_BADGE_MENU_ICON_CAMERA,
    "VISION"
  },
  {
    BLIND_BADGE_MENU_VOICE,
    BLIND_BADGE_MENU_ICON_MICROPHONE,
    "VOICE"
  },
  {
    BLIND_BADGE_MENU_HEALTH,
    BLIND_BADGE_MENU_ICON_HEART,
    "HEALTH"
  },
  {
    BLIND_BADGE_MENU_ALERT,
    BLIND_BADGE_MENU_ICON_BELL,
    "ALERT"
  },
  {
    BLIND_BADGE_MENU_SETTINGS,
    BLIND_BADGE_MENU_ICON_SETTINGS,
    "SETTINGS"
  },
  {
    BLIND_BADGE_MENU_TEST,
    BLIND_BADGE_MENU_ICON_TEST,
    "TEST"
  }
};

static size_t g_menu_selected;
static enum blind_badge_menu_page g_menu_page;
static bool g_menu_redraw_pending;
static volatile bool g_menu_leave_requested;
static volatile bool g_menu_alert_requested;
static bool g_menu_passive_alert;
static bool g_speech_link_was_connected;
static int g_speech_mode_sent = -1;
static char g_menu_cached_state[BLIND_BADGE_MENU_STATE_SIZE];
static char g_menu_cached_detail[BLIND_BADGE_MENU_DETAIL_SIZE];

static void menu_build_arrays(const char *labels[],
                              enum blind_badge_menu_icon icons[])
{
  size_t i;

  for (i = 0; i < sizeof(g_menu_items) / sizeof(g_menu_items[0]); i++)
    {
      labels[i] = g_menu_items[i].label;
      icons[i] = g_menu_items[i].icon;
    }
}

static int menu_sync_speech_mode(enum blind_badge_speech_mode mode)
{
  static const char *const flags[] =
  {
    "SPEECH_MODE_MENU",
    "SPEECH_MODE_VISION",
    "SPEECH_MODE_VOICE",
    "SPEECH_MODE_HEALTH",
    "SPEECH_MODE_ALERT",
    "SPEECH_MODE_SILENT"
  };
  char menu_flag[24];
  const char *flag = flags[mode];
  bool connected = UART_LINK_IsConnected();

  if (!connected)
    {
      g_speech_link_was_connected = false;
      g_speech_mode_sent = -1;
      return -ENOTCONN;
    }

  if (!g_speech_link_was_connected)
    {
      g_speech_mode_sent = -1;
      g_speech_link_was_connected = true;
    }

  if (g_speech_mode_sent == (int)mode)
    {
      return 0;
    }

  /* A relative NEXT event is sufficient only after both boards agree on the
   * current item.  Carry the absolute index in the one-time MENU mode sync;
   * ordinary button presses remain a single BUTTON_MENU_NEXT message.
   */

  if (mode == BLIND_BADGE_SPEECH_MODE_MENU)
    {
      snprintf(menu_flag, sizeof(menu_flag), "SPEECH_MODE_MENU_%u",
               (unsigned int)g_menu_selected);
      flag = menu_flag;
    }

  if (UART_LINK_SendFlag(flag) != 0)
    {
      return -EIO;
    }

  g_speech_mode_sent = (int)mode;
  return 1;
}

static void menu_announce_next(void)
{
  int sync_ret;

  /* Menu labels are fixed and circular.  Send only the MENU button state;
   * the sub-board advances its local pointer and speaks the next label.
   */

  sync_ret = menu_sync_speech_mode(BLIND_BADGE_SPEECH_MODE_MENU);
  if (sync_ret < 0)
    {
      return;
    }

  /* A fresh/reconnected link has just received the absolute current index
   * and must not advance a second time.
   */

  if (sync_ret == 0)
    {
      (void)UART_LINK_SendFlag("BUTTON_MENU_NEXT");
    }
}

static int render_service_page(void)
{
  enum blind_badge_menu_id id = blind_badge_service_active();
  enum blind_badge_service_state state = blind_badge_service_state();
  const char *name = blind_badge_service_state_name(state);
  char detail[BLIND_BADGE_MENU_DETAIL_SIZE];

  (void)menu_sync_speech_mode(
    id == BLIND_BADGE_MENU_VISION ? BLIND_BADGE_SPEECH_MODE_VISION :
    id == BLIND_BADGE_MENU_VOICE ? BLIND_BADGE_SPEECH_MODE_VOICE :
    id == BLIND_BADGE_MENU_HEALTH ? BLIND_BADGE_SPEECH_MODE_HEALTH :
    id == BLIND_BADGE_MENU_ALERT ? BLIND_BADGE_SPEECH_MODE_ALERT :
    BLIND_BADGE_SPEECH_MODE_SILENT);

  blind_badge_service_detail(detail, sizeof(detail));

  if (strcmp(g_menu_cached_state, name) == 0 &&
      strcmp(g_menu_cached_detail, detail) == 0)
    {
      return 0;
    }

  strlcpy(g_menu_cached_state, name, sizeof(g_menu_cached_state));
  strlcpy(g_menu_cached_detail, detail, sizeof(g_menu_cached_detail));

  if (id == BLIND_BADGE_MENU_ALERT && g_menu_passive_alert)
    {
      return blind_badge_display_show_passive_alert(name, detail);
    }

  return blind_badge_display_show_service(blind_badge_service_label(id),
                                          name, detail, "2x PLAY  BACK");
}

int blind_badge_menu_show(void)
{
  const char *labels[sizeof(g_menu_items) / sizeof(g_menu_items[0])];
  enum blind_badge_menu_icon
    icons[sizeof(g_menu_items) / sizeof(g_menu_items[0])];
  int ret;

  if (blind_badge_service_state() == BLIND_BADGE_SERVICE_STOPPING)
    {
      return -EBUSY;
    }

  menu_build_arrays(labels, icons);
  g_menu_page = BLIND_BADGE_MENU_PAGE_LIST;
  g_menu_passive_alert = false;
  g_menu_leave_requested = false;
  g_menu_redraw_pending = false;

  ret = blind_badge_display_show_menu(labels, icons,
                                      sizeof(g_menu_items) /
                                      sizeof(g_menu_items[0]),
                                      g_menu_selected);
  (void)menu_sync_speech_mode(BLIND_BADGE_SPEECH_MODE_MENU);
  return ret;
}

int blind_badge_menu_next(void)
{
  const char *labels[sizeof(g_menu_items) / sizeof(g_menu_items[0])];
  enum blind_badge_menu_icon
    icons[sizeof(g_menu_items) / sizeof(g_menu_items[0])];
  int ret;

  if (blind_badge_service_state() == BLIND_BADGE_SERVICE_STOPPING)
    {
      return -EBUSY;
    }

  /* While a service page is up the menu selection is locked; only the
   * PLAY double click leaves the page.
   */

  if (g_menu_page == BLIND_BADGE_MENU_PAGE_SERVICE)
    {
      return blind_badge_menu_refresh();
    }

  menu_build_arrays(labels, icons);

  ret = blind_badge_display_animate_menu_next(
          labels, icons,
          sizeof(g_menu_items) / sizeof(g_menu_items[0]),
          g_menu_selected);

  g_menu_selected++;
  if (g_menu_selected >= sizeof(g_menu_items) / sizeof(g_menu_items[0]))
    {
      g_menu_selected = 0;
    }

  menu_announce_next();
  return ret < 0 ? ret : blind_badge_menu_show();
}

enum blind_badge_menu_id blind_badge_menu_selected(void)
{
  return g_menu_items[g_menu_selected].id;
}

int blind_badge_menu_enter(void)
{
  enum blind_badge_menu_id selected;
  int ret;

  if (g_menu_page == BLIND_BADGE_MENU_PAGE_SERVICE)
    {
      return blind_badge_menu_refresh();
    }

  if (blind_badge_service_state() != BLIND_BADGE_SERVICE_IDLE)
    {
      return -EBUSY;
    }

  selected = blind_badge_menu_selected();
  g_menu_passive_alert = false;
  (void)menu_sync_speech_mode(
    selected == BLIND_BADGE_MENU_VISION ? BLIND_BADGE_SPEECH_MODE_VISION :
    selected == BLIND_BADGE_MENU_VOICE ? BLIND_BADGE_SPEECH_MODE_VOICE :
    selected == BLIND_BADGE_MENU_HEALTH ? BLIND_BADGE_SPEECH_MODE_HEALTH :
    selected == BLIND_BADGE_MENU_ALERT ? BLIND_BADGE_SPEECH_MODE_ALERT :
    BLIND_BADGE_SPEECH_MODE_SILENT);

  ret = blind_badge_service_start(selected);
  if (ret == -EBUSY)
    {
      return ret;
    }

  g_menu_page = BLIND_BADGE_MENU_PAGE_SERVICE;
  g_menu_cached_state[0] = '\0';
  g_menu_cached_detail[0] = '\0';

  /* Keep the page visible even when the service refuses to start so the
   * operator can read the reason and leave with a PLAY double click.
   */

  return render_service_page();
}

int blind_badge_menu_leave(void)
{
  if (g_menu_page == BLIND_BADGE_MENU_PAGE_SERVICE)
    {
      (void)blind_badge_service_stop();
      g_menu_page = BLIND_BADGE_MENU_PAGE_LIST;
      g_menu_redraw_pending = false;
      g_speech_mode_sent = -1;
      return blind_badge_menu_show();
    }

  return blind_badge_menu_show();
}

void blind_badge_menu_request_leave(void)
{
  g_menu_leave_requested = true;
}

void blind_badge_menu_request_alert(void)
{
  g_menu_alert_requested = true;
}

static int menu_start_emergency_alert(void)
{
  enum blind_badge_service_state state;
  size_t i;
  int ret;

  g_menu_alert_requested = false;
  g_menu_leave_requested = false;

  /* Reuse normal service teardown so camera, ASR, health sensing and AI
   * work release their resources before ALERT takes ownership.
   */

  state = blind_badge_service_state();
  if (state != BLIND_BADGE_SERVICE_IDLE)
    {
      (void)blind_badge_service_stop();
    }

  for (i = 0; i < sizeof(g_menu_items) / sizeof(g_menu_items[0]); i++)
    {
      if (g_menu_items[i].id == BLIND_BADGE_MENU_ALERT)
        {
          g_menu_selected = i;
          break;
        }
    }

  g_speech_mode_sent = -1;
  g_menu_passive_alert = true;
  (void)menu_sync_speech_mode(BLIND_BADGE_SPEECH_MODE_ALERT);
  ret = blind_badge_service_start(BLIND_BADGE_MENU_ALERT);

  g_menu_page = BLIND_BADGE_MENU_PAGE_SERVICE;
  g_menu_cached_state[0] = '\0';
  g_menu_cached_detail[0] = '\0';
  (void)render_service_page();

  printf("[BlindBadge] passive alert: active service preempted, "
         "ALERT result=%d\n", ret);
  return ret;
}

int blind_badge_menu_refresh(void)
{
  if (g_menu_alert_requested)
    {
      return menu_start_emergency_alert();
    }

  if (g_menu_page == BLIND_BADGE_MENU_PAGE_SERVICE &&
      g_menu_leave_requested)
    {
      g_menu_leave_requested = false;
      return blind_badge_menu_leave();
    }

  if (g_menu_page != BLIND_BADGE_MENU_PAGE_SERVICE)
    {
      if (g_menu_redraw_pending &&
          blind_badge_service_state() != BLIND_BADGE_SERVICE_STOPPING)
        {
          return blind_badge_menu_show();
        }

      (void)menu_sync_speech_mode(BLIND_BADGE_SPEECH_MODE_MENU);
      return 0;
    }

  return render_service_page();
}

int blind_badge_menu_is_service_page(void)
{
  return g_menu_page == BLIND_BADGE_MENU_PAGE_SERVICE ? 1 : 0;
}
