# Contest 2026 team 024 blind badge app
if(CONFIG_LVX_USE_DEMO_CONTEST2026_024_BLIND_BADGE_APP)
  nuttx_add_application(
    NAME blind_badge_app
    SRCS
      blind_badge_app_main.c
      blind_badge_action.c
      blind_badge_ai.c
      blind_badge_display.c
      blind_badge_event.c
      blind_badge_policy.c
      blind_badge_skill.c
      uart_link.c
    INCLUDE_DIRECTORIES
      ${NUTTX_APPS_DIR}/packages/ai_agent/include
      ${NUTTX_APPS_DIR}/packages/ai_agent/src
      ${NUTTX_APPS_DIR}/netutils/cjson/cJSON
    STACKSIZE 16384)
endif()
