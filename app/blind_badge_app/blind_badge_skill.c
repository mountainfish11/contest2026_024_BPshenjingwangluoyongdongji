/****************************************************************************
 * Contest 2026 team 024 - BlindBadge Skill installer
 ****************************************************************************/

#include "blind_badge_skill.h"

#include "agent_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define BLIND_BADGE_SKILL_PATH AGENT_SKILLS_DIR "blind-badge.md"

static const char g_blind_badge_skill[] =
  "# BlindBadge Safety Reminder Skill\n"
  "\n"
  "BlindBadge is a wearable assistive badge for blind and low-vision users. "
  "This Skill turns sensor events into short Chinese safety reminders for "
  "voice playback.\n"
  "\n"
  "## When to use\n"
  "\n"
  "Use this Skill when the user mentions BlindBadge, 盲人辅助胸牌, "
  "obstacle_near, step_down, emergency, 障碍, 台阶, 下行台阶, 求助, 避障, "
  "or asks for a safety reminder based on a BlindBadge event.\n"
  "\n"
  "Expected event shape:\n"
  "\n"
  "```text\n"
  "BlindBadge event=<obstacle_near|step_down|emergency> "
  "distance_cm=<number> direction=<front|left|right>\n"
  "```\n"
  "\n"
  "## Output rules\n"
  "\n"
  "- Output exactly one Chinese sentence.\n"
  "- Keep the sentence short, clear, and low-interruption for voice playback.\n"
  "- Put the safest immediate action first.\n"
  "- Use calm wording; do not scare the user.\n"
  "- Do not provide a long explanation.\n"
  "- Do not promise absolute safety.\n"
  "- Do not say the environment is definitely safe.\n"
  "- Do not mention that this is a simulation unless the user asks.\n"
  "- Do not include Markdown, bullet points, labels, or extra commentary.\n"
  "- Prefer concrete direction words: 前方, 左侧, 右侧.\n"
  "- If confidence is low or information is incomplete, use cautious wording "
  "such as 可能 or 疑似.\n"
  "- If the input conflicts, prioritize the more dangerous signal.\n"
  "\n"
  "## Event rules\n"
  "\n"
  "For `obstacle_near`:\n"
  "\n"
  "- If `distance_cm` is below 50, tell the user to stop first and confirm.\n"
  "- If `distance_cm` is 50 to 99, tell the user to slow down and avoid the "
  "obstacle.\n"
  "- If `distance_cm` is 100 or greater, tell the user to keep attention "
  "without over-warning.\n"
  "- Include direction when present.\n"
  "- Avoid saying the path is blocked unless that was explicitly detected.\n"
  "\n"
  "For `step_down`:\n"
  "\n"
  "- Tell the user to stop or slow down first.\n"
  "- Mention that there may be a downward step.\n"
  "- Suggest confirming with a cane, foot, handrail, or nearby support.\n"
  "- Treat `step_down` as higher priority than a far obstacle.\n"
  "\n"
  "For `emergency`:\n"
  "\n"
  "- Generate a help message suitable for a contact or nearby helper.\n"
  "- Keep it calm, direct, and actionable.\n"
  "- Do not claim a precise location unless a location is provided.\n"
  "- If no location is provided, ask the helper to contact or come nearby to "
  "confirm.\n";

static int ensure_dir(const char *path)
{
  if (mkdir(path, 0755) == 0 || errno == EEXIST)
    {
      return 0;
    }

  return -1;
}

int blind_badge_install_skill(void)
{
  FILE *fp;
  size_t len;
  size_t written;

  if (ensure_dir("/data") != 0 ||
      ensure_dir(AGENT_DATA_DIR) != 0 ||
      ensure_dir(AGENT_SKILLS_DIR) != 0)
    {
      printf("[BlindBadge] skill_error: failed to create %s: %s\n",
             AGENT_SKILLS_DIR, strerror(errno));
      return 1;
    }

  fp = fopen(BLIND_BADGE_SKILL_PATH, "w");
  if (fp == NULL)
    {
      printf("[BlindBadge] skill_error: failed to open %s: %s\n",
             BLIND_BADGE_SKILL_PATH, strerror(errno));
      return 1;
    }

  len = strlen(g_blind_badge_skill);
  written = fwrite(g_blind_badge_skill, 1, len, fp);
  fclose(fp);

  if (written != len)
    {
      printf("[BlindBadge] skill_error: wrote %d of %d bytes\n",
             (int)written, (int)len);
      return 1;
    }

  printf("[BlindBadge] skill_installed: %s\n", BLIND_BADGE_SKILL_PATH);
  printf("[BlindBadge] skill_title: BlindBadge Safety Reminder Skill\n");
  printf("[BlindBadge] skill_verify: run ai_agent, then ask /skill\n");
  return 0;
}
