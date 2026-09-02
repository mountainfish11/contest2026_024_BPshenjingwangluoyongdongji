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

MAINSRC = blind_badge_app_main.c

include $(APPDIR)/Application.mk
