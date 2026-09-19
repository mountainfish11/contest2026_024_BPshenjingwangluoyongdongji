/****************************************************************************
 * Contest 2026 team 024 - BlindBadge local safety policy
 ****************************************************************************/

#include "blind_badge_policy.h"

#include <stdio.h>
#include <string.h>

static int is_sentence_end(char c)
{
  return c == '.' || c == '!' || c == '?' || c == '\n';
}

static size_t utf8_prefix_len(const char *s, size_t len)
{
  size_t i = 0;
  size_t last_good = 0;

  while (i < len)
    {
      unsigned char c = (unsigned char)s[i];
      size_t need;

      if ((c & 0x80) == 0)
        {
          need = 1;
        }
      else if ((c & 0xe0) == 0xc0)
        {
          need = 2;
        }
      else if ((c & 0xf0) == 0xe0)
        {
          need = 3;
        }
      else if ((c & 0xf8) == 0xf0)
        {
          need = 4;
        }
      else
        {
          break;
        }

      if (i + need > len)
        {
          break;
        }

      for (size_t j = 1; j < need; j++)
        {
          if (((unsigned char)s[i + j] & 0xc0) != 0x80)
            {
              return last_good;
            }
        }

      i += need;
      last_good = i;
    }

  return last_good;
}

static void copy_short_sentence(const char *input, char *buf, size_t buf_size)
{
  size_t i;
  size_t max_copy;

  if (buf_size == 0)
    {
      return;
    }

  if (input == NULL || input[0] == '\0')
    {
      buf[0] = '\0';
      return;
    }

  max_copy = buf_size - 1;
  if (max_copy > 120)
    {
      max_copy = 120;
    }

  for (i = 0; input[i] != '\0' && i < max_copy; i++)
    {
      buf[i] = input[i];
      if (is_sentence_end(input[i]))
        {
          i++;
          break;
        }

      if ((unsigned char)input[i] == 0xe3 &&
          (unsigned char)input[i + 1] == 0x80 &&
          ((unsigned char)input[i + 2] == 0x82 ||
           (unsigned char)input[i + 2] == 0x81 ||
           (unsigned char)input[i + 2] == 0x9b))
        {
          if (i + 2 < max_copy)
            {
              buf[i + 1] = input[i + 1];
              buf[i + 2] = input[i + 2];
              i += 3;
            }
          else
            {
              i++;
            }
          break;
        }
    }

  i = utf8_prefix_len(buf, i);

  buf[i] = '\0';
}

void blind_badge_apply_policy(struct blind_badge_event *event)
{
  if (event == NULL)
    {
      return;
    }

  switch (event->type)
    {
      case BLIND_BADGE_EVENT_OBSTACLE:
        if (event->distance_cm < 50)
          {
            event->severity = BLIND_BADGE_SEVERITY_URGENT;
          }
        else if (event->distance_cm < 100)
          {
            event->severity = BLIND_BADGE_SEVERITY_WARN;
          }
        else
          {
            event->severity = BLIND_BADGE_SEVERITY_INFO;
          }
        break;

      case BLIND_BADGE_EVENT_STEP_DOWN:
        event->severity = BLIND_BADGE_SEVERITY_URGENT;
        break;

      case BLIND_BADGE_EVENT_EMERGENCY:
        event->severity = BLIND_BADGE_SEVERITY_EMERGENCY;
        break;

      default:
        event->severity = BLIND_BADGE_SEVERITY_INFO;
        break;
    }
}

void blind_badge_build_local_suggestion(const struct blind_badge_event *event,
                                        char *buf, size_t buf_size)
{
  if (event == NULL || buf == NULL || buf_size == 0)
    {
      return;
    }

  switch (event->type)
    {
      case BLIND_BADGE_EVENT_STATUS:
        snprintf(buf, buf_size,
                 "系统正在运行，可继续模拟障碍、台阶或求助事件。");
        break;

      case BLIND_BADGE_EVENT_OBSTACLE:
        if (event->distance_cm < 50)
          {
            snprintf(buf, buf_size, "%s%d厘米有障碍，请立即停下确认。",
                     blind_badge_direction_zh(event->direction),
                     event->distance_cm);
          }
        else if (event->distance_cm < 100)
          {
            snprintf(buf, buf_size, "%s%d厘米有障碍，请减速并绕行。",
                     blind_badge_direction_zh(event->direction),
                     event->distance_cm);
          }
        else
          {
            snprintf(buf, buf_size, "%s检测到较远障碍，请保持注意。",
                     blind_badge_direction_zh(event->direction));
          }
        break;

      case BLIND_BADGE_EVENT_STEP_DOWN:
        snprintf(buf, buf_size,
                 "%s可能有下行台阶，请停一下，用手杖或脚尖确认。",
                 blind_badge_direction_zh(event->direction));
        break;

      case BLIND_BADGE_EVENT_EMERGENCY:
        snprintf(buf, buf_size,
                 "已触发求助。建议发送：我需要帮助，请联系我或前往我的当前位置。");
        break;

      default:
        snprintf(buf, buf_size, "检测到未知事件，请停下确认。");
        break;
    }
}

void blind_badge_build_ai_prompt(const struct blind_badge_event *event,
                                 char *buf, size_t buf_size)
{
  const char *action = "请保持注意";

  if (event == NULL || buf == NULL || buf_size == 0)
    {
      return;
    }

  switch (event->type)
    {
      case BLIND_BADGE_EVENT_STATUS:
        snprintf(buf, buf_size,
                 "BlindBadge event=status。你是盲人辅助胸牌。"
                 "设备状态正常，请生成一句简短的系统就绪提示。");
        break;

      case BLIND_BADGE_EVENT_OBSTACLE:
        if (event->distance_cm < 50)
          {
            action = "请先停下确认";
          }
        else if (event->distance_cm < 100)
          {
            action = "请减速并绕行";
          }

        snprintf(buf, buf_size,
                 "BlindBadge event=obstacle_near distance_cm=%d direction=%s。"
                 "你是盲人辅助胸牌。检测到%s%d厘米有障碍，请生成一句简短安全提醒。"
                 "必须包含动作：%s。只输出一句提醒，优先安全，不长篇解释，不承诺绝对安全。",
                 event->distance_cm,
                 blind_badge_direction_name(event->direction),
                 blind_badge_direction_zh(event->direction),
                 event->distance_cm,
                 action);
        break;

      case BLIND_BADGE_EVENT_STEP_DOWN:
        snprintf(buf, buf_size,
                 "BlindBadge event=step_down direction=%s。"
                 "你是盲人辅助胸牌。检测到%s可能有下行台阶，请生成一句简短安全提醒。"
                 "必须提醒停下或放慢并确认。只输出一句提醒，优先安全，不长篇解释，不承诺绝对安全。",
                 blind_badge_direction_name(event->direction),
                 blind_badge_direction_zh(event->direction));
        break;

      case BLIND_BADGE_EVENT_EMERGENCY:
        snprintf(buf, buf_size,
                 "BlindBadge event=emergency。"
                 "你是盲人辅助胸牌。用户触发了求助按钮，请生成一句适合发给紧急联系人的求助信息。"
                 "只输出一句简短求助信息，不长篇解释，不承诺绝对安全。");
        break;

      default:
        snprintf(buf, buf_size,
                 "BlindBadge event=unknown。请输出一句简短安全提醒。");
        break;
    }
}

void blind_badge_fallback_response(const char *local_suggestion,
                                   char *buf, size_t buf_size)
{
  const char *help_message;

  if (buf == NULL || buf_size == 0)
    {
      return;
    }

  help_message = strstr(local_suggestion, "建议发送：");
  if (help_message != NULL)
    {
      copy_short_sentence(help_message + strlen("建议发送："), buf, buf_size);
      return;
    }

  copy_short_sentence(local_suggestion, buf, buf_size);
}

void blind_badge_normalize_ai_response(const char *input,
                                       const char *local_suggestion,
                                       char *buf, size_t buf_size)
{
  if (input == NULL || input[0] == '\0')
    {
      blind_badge_fallback_response(local_suggestion, buf, buf_size);
      return;
    }

  copy_short_sentence(input, buf, buf_size);
  if (buf[0] == '\0')
    {
      blind_badge_fallback_response(local_suggestion, buf, buf_size);
    }
}
