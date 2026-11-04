############################################################################
# Contest 2026 team 024 blind badge app - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = blind_badge_app
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 16384
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_024_BLIND_BADGE_APP)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/src
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON

CSRCS = blind_badge_action.c blind_badge_ai.c blind_badge_display.c
CSRCS += blind_badge_event.c blind_badge_fall.c blind_badge_menu.c
CSRCS += blind_badge_policy.c blind_badge_service.c blind_badge_skill.c
CSRCS += blind_badge_state.c
CSRCS += blind_badge_vision.c blind_badge_vision_server.c qma7981.c
CSRCS += uart_link.c
MAINSRC = blind_badge_app_main.c

include $(APPDIR)/Application.mk
