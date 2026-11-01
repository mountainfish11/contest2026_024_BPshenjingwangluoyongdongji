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
  BLIND_BADGE_SPEECH_MODE_SILENT
};

struct blind_badge_menu_item
{
  enum blind_badge_menu_id id;
  enum blind_badge_menu_icon icon;
  const char *label;
  const char *speech_flag;
};

static const struct blind_badge_menu_item g_menu_items[] =
{
  {
    BLIND_BADGE_MENU_VISION,
    BLIND_BADGE_MENU_ICON_CAMERA,
    "VISION",
    "MENU_VISION"
  },
  {
    BLIND_BADGE_MENU_VOICE,
    BLIND_BADGE_MENU_ICON_MICROPHONE,
    "VOICE",
    "MENU_VOICE"
  },
  {
    BLIND_BADGE_MENU_HEALTH,
    BLIND_BADGE_MENU_ICON_HEART,
    "HEALTH",
    "MENU_HEALTH"
  },
  {
    BLIND_BADGE_MENU_ALERT,
    BLIND_BADGE_MENU_ICON_BELL,
    "ALERT",
    "MENU_ALERT"
  },
  {
    BLIND_BADGE_MENU_SETTINGS,
    BLIND_BADGE_MENU_ICON_SETTINGS,
    "SETTINGS",
    "MENU_SETTINGS"
  }
};

static size_t g_menu_selected;
static enum blind_badge_menu_page g_menu_page;
static bool g_menu_redraw_pending;
static int g_menu_announced = -1;
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
    "SPEECH_MODE_SILENT"
  };
  bool connected = UART_LINK_IsConnected();

  if (!connected)
    {
      g_speech_link_was_connected = false;
      g_speech_mode_sent = -1;
      return -ENOTCONN;
    }

  if (!g_speech_link_was_connected)
    {
      g_menu_announced = -1;
      g_speech_mode_sent = -1;
      g_speech_link_was_connected = true;
    }

  if (g_speech_mode_sent == (int)mode)
    {
      return 0;
    }

  if (UART_LINK_SendFlag(flags[mode]) != 0)
    {
      return -EIO;
    }

  g_speech_mode_sent = (int)mode;
  return 0;
}

static void menu_announce_selected(void)
{
  /* Announce the current item once after every link establishment and once
   * after every real selection change.  LCD animation frames and ordinary
   * redraws must not enqueue duplicate speech.
   */

  if (menu_sync_speech_mode(BLIND_BADGE_SPEECH_MODE_MENU) < 0)
    {
      return;
    }

  if (g_menu_announced == (int)g_menu_selected)
    {
      return;
    }

  if (UART_LINK_SendFlag(g_menu_items[g_menu_selected].speech_flag) == 0)
    {
      g_menu_announced = (int)g_menu_selected;
    }
}

static int render_service_page(void)
{
  enum blind_badge_menu_id id = blind_badge_service_active();
  enum blind_badge_service_state state = blind_badge_service_state();
  const char *name = blind_badge_service_state_name(state);
  char detail[BLIND_BADGE_MENU_DETAIL_SIZE];

  (void)menu_sync_speech_mode(
    id == BLIND_BADGE_MENU_VISION ?
    BLIND_BADGE_SPEECH_MODE_VISION : BLIND_BADGE_SPEECH_MODE_SILENT);

  blind_badge_service_detail(detail, sizeof(detail));

  if (strcmp(g_menu_cached_state, name) == 0 &&
      strcmp(g_menu_cached_detail, detail) == 0)
    {
      return 0;
    }

  strlcpy(g_menu_cached_state, name, sizeof(g_menu_cached_state));
  strlcpy(g_menu_cached_detail, detail, sizeof(g_menu_cached_detail));

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
  g_menu_redraw_pending = false;

  ret = blind_badge_display_show_menu(labels, icons,
                                      sizeof(g_menu_items) /
                                      sizeof(g_menu_items[0]),
                                      g_menu_selected);
  menu_announce_selected();
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
  (void)menu_sync_speech_mode(
    selected == BLIND_BADGE_MENU_VISION ?
    BLIND_BADGE_SPEECH_MODE_VISION : BLIND_BADGE_SPEECH_MODE_SILENT);

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
      blind_badge_service_stop();
      g_menu_page = BLIND_BADGE_MENU_PAGE_LIST;
      g_menu_redraw_pending = true;
      g_menu_announced = -1;

      /* Camera sensor initialization cannot be cancelled in the middle of
       * an I2C transaction.  Defer the LCD transfer until that worker has
       * released the camera/DMA resources.
       */

      if (blind_badge_service_state() == BLIND_BADGE_SERVICE_STOPPING)
        {
          return 0;
        }
    }

  return blind_badge_menu_show();
}

int blind_badge_menu_refresh(void)
{
  if (g_menu_page != BLIND_BADGE_MENU_PAGE_SERVICE)
    {
      if (g_menu_redraw_pending &&
          blind_badge_service_state() != BLIND_BADGE_SERVICE_STOPPING)
        {
          return blind_badge_menu_show();
        }

      menu_announce_selected();
      return 0;
    }

  return render_service_page();
}

int blind_badge_menu_is_service_page(void)
{
  return g_menu_page == BLIND_BADGE_MENU_PAGE_SERVICE ? 1 : 0;
}
