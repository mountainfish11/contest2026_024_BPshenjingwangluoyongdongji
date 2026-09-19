/****************************************************************************
 * Contest 2026 team 024 - BlindBadge app
 ****************************************************************************/

#include "blind_badge_action.h"
#include "blind_badge_ai.h"
#include "blind_badge_display.h"
#include "blind_badge_event.h"
#include "blind_badge_policy.h"
#include "blind_badge_skill.h"
#include "uart_link.h"

#include <nuttx/config.h>

#ifdef CONFIG_SYSTEM_NXRECORDER
#  include <nuttx/audio/audio.h>
#  include <system/nxrecorder.h>
#endif

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
#  include "channels/nsh_commands.h"
#  include "infra/network_manager.h"
#  include "voice/voice_asr.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#define BLIND_BADGE_TEXT_SIZE 512
#define BLIND_BADGE_WIFI_UI_TIMEOUT_SECONDS 12
#define BLIND_BADGE_MIC_DEVICE "/dev/audio/pcm_in0"
#define BLIND_BADGE_NXRECORDER_IDLE 0
#define BLIND_BADGE_MIC_DEFAULT_FILE "/tmp/mic.pcm"
#define BLIND_BADGE_MIC_DEFAULT_SECONDS 3
#define BLIND_BADGE_MIC_MAX_SECONDS 30
#define BLIND_BADGE_VOICE_AI_DEFAULT_SECONDS 3
#define BLIND_BADGE_VOICE_AI_MAX_SECONDS 4
#define BLIND_BADGE_VOICE_AI_PCM_FILE "/tmp/voice_ai_16bit.pcm"

struct blind_badge_cli_options
{
  enum blind_badge_ai_mode ai_mode;
  int use_lcd;
  int option_count;
};

static void print_help(void)
{
  printf("BlindBadge usage:\n");
  printf("  blind_badge_app help\n");
  printf("  blind_badge_app status [--lcd]\n");
  printf("  blind_badge_app obstacle <distance_cm> <front|left|right> [--ai|--agent] [--lcd]\n");
  printf("  blind_badge_app step_down <front|left|right> [--ai|--agent] [--lcd]\n");
  printf("  blind_badge_app emergency [--ai|--agent] [--lcd]\n");
  printf("  blind_badge_app demo [--ai|--agent] [--lcd]\n");
  printf("  blind_badge_app boot_display\n");
  printf("  blind_badge_app boot_timeout\n");
  printf("  blind_badge_app lcd_test [zh]\n");
  printf("  blind_badge_app mic_record [seconds] [16bit_pcm_file]\n");
  printf("  blind_badge_app mic_stats <16bit_pcm_file>\n");
  printf("  blind_badge_app mic_dump <16bit_pcm_file> [max_bytes]\n");
  printf("  blind_badge_app voice_ai [seconds] [--ai|--agent] [--lcd]\n");
  printf("  blind_badge_app voice_ai_text <recognized_text...> [--ai|--agent]\n");
  printf("  blind_badge_app install_skill\n");
  printf("  blind_badge_app uart_link_start|uart_link_stop|uart_link_status\n");
  printf("  --ai: call ai_agent LLM router directly and print ai_response\n");
  printf("  --agent: send ai_prompt to running ai_agent and wait for ai_response\n");
  printf("  --lcd: enable LCD explicitly (default with framebuffer builds)\n");
}

static int print_mic_stats_limited(const char *path, size_t max_samples)
{
  int16_t samples[256];
  int32_t minimum = INT32_MAX;
  int32_t maximum = INT32_MIN;
  uint32_t peak = 0;
  size_t nonzero = 0;
  size_t total = 0;
  FILE *stream;
  size_t count;
  size_t i;

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      printf("[BlindBadge] error: cannot open %s.\n", path);
      return 1;
    }

  while ((count = fread(samples, sizeof(samples[0]),
                          sizeof(samples) / sizeof(samples[0]), stream)) > 0)
    {
      for (i = 0; i < count; i++)
        {
          uint32_t magnitude;
          int32_t sample = samples[i];

          if (sample < minimum)
            {
              minimum = sample;
            }

          if (sample > maximum)
            {
              maximum = sample;
            }

          if (sample != 0)
            {
              nonzero++;
            }

          magnitude = sample < 0 ? (uint32_t)(-(int64_t)sample) :
                                   (uint32_t)sample;
          if (magnitude > peak)
            {
              peak = magnitude;
            }

          total++;
          if (max_samples > 0 && total >= max_samples)
            {
              break;
            }
        }

      if (max_samples > 0 && total >= max_samples)
        {
          break;
        }
    }

  fclose(stream);

  if (total == 0)
    {
      printf("[BlindBadge] mic_stats: empty file\n");
      return 1;
    }

  printf("[BlindBadge] mic_stats: samples=%zu nonzero=%zu "
         "min=%ld max=%ld peak=%lu\n",
         total, nonzero, (long)minimum, (long)maximum,
         (unsigned long)peak);
  return nonzero > 0 && peak > 0 ? 0 : 1;
}

static int print_mic_stats(const char *path)
{
  return print_mic_stats_limited(path, 0);
}

static int handle_mic_stats(int argc, char *argv[])
{
  if (argc != 3)
    {
      printf("[BlindBadge] error: mic_stats needs one PCM file.\n");
      return 1;
    }

  return print_mic_stats(argv[2]);
}

static int dump_mic_pcm_hex(const char *path, size_t max_bytes)
{
  unsigned char buffer[32];
  FILE *stream;
  size_t dumped = 0;
  size_t count;
  size_t i;

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      printf("[BlindBadge] error: cannot open %s.\n", path);
      return 1;
    }

  printf("[BlindBadge] mic_dump_begin: path=%s format=hex max_bytes=%lu\n",
         path, (unsigned long)max_bytes);

  while ((count = fread(buffer, 1, sizeof(buffer), stream)) > 0)
    {
      if (max_bytes > 0 && dumped + count > max_bytes)
        {
          count = max_bytes - dumped;
        }

      for (i = 0; i < count; i++)
        {
          printf("%02x", buffer[i]);
        }

      printf("\n");
      dumped += count;

      if (max_bytes > 0 && dumped >= max_bytes)
        {
          break;
        }
    }

  if (ferror(stream))
    {
      printf("[BlindBadge] mic_dump: read error after %lu byte(s).\n",
             (unsigned long)dumped);
      fclose(stream);
      return 1;
    }

  fclose(stream);
  printf("[BlindBadge] mic_dump_end: bytes=%lu\n", (unsigned long)dumped);
  return dumped > 0 ? 0 : 1;
}

static int handle_mic_dump(int argc, char *argv[])
{
  size_t max_bytes = 0;
  char *end;
  long value;

  if (argc != 3 && argc != 4)
    {
      printf("[BlindBadge] error: mic_dump needs a PCM file and optional max_bytes.\n");
      return 1;
    }

  if (argc == 4)
    {
      errno = 0;
      value = strtol(argv[3], &end, 10);
      if (errno != 0 || *argv[3] == '\0' || *end != '\0' || value < 1)
        {
          printf("[BlindBadge] error: max_bytes must be positive.\n");
          return 1;
        }

      max_bytes = (size_t)value;
    }

  return dump_mic_pcm_hex(argv[2], max_bytes);
}

static int record_mic_16bit_file(const char *path, long seconds)
{
#ifdef CONFIG_SYSTEM_NXRECORDER
  struct nxrecorder_s *recorder;
  uint32_t limit_bytes;
  int elapsed_ms = 0;
  int timeout_ms;
  int ret;

  recorder = nxrecorder_create();
  if (recorder == NULL)
    {
      printf("[BlindBadge] mic_record: cannot create recorder.\n");
      return 1;
    }

  ret = nxrecorder_setdevice(recorder, BLIND_BADGE_MIC_DEVICE);
  if (ret != OK)
    {
      printf("[BlindBadge] mic_record: device %s unavailable (%d).\n",
             BLIND_BADGE_MIC_DEVICE, ret);
      nxrecorder_release(recorder);
      return 1;
    }

  limit_bytes = seconds * 16000 * 2;
  recorder->record_limit_bytes = limit_bytes;
  recorder->record_bytes = 0;

  ret = nxrecorder_recordinternal(recorder, path, AUDIO_FMT_PCM,
                                  1, 16, 16000, 0);
  if (ret < 0)
    {
      printf("[BlindBadge] mic_record: start failed (%d).\n", ret);
      nxrecorder_release(recorder);
      return 1;
    }

  printf("[BlindBadge] mic_record: recording %ld second(s), "
         "mono 16000 Hz 16-bit raw PCM -> %s\n", seconds, path);

  printf("[BlindBadge] mic_record: waiting for %lu byte(s).\n",
         (unsigned long)limit_bytes);

  timeout_ms = (int)seconds * 1000 + 5000;
  while (elapsed_ms < timeout_ms)
    {
      if (recorder->record_bytes >= limit_bytes)
        {
          break;
        }

      usleep(10000);
      elapsed_ms += 10;
    }

  elapsed_ms = 0;
  while (recorder->state != BLIND_BADGE_NXRECORDER_IDLE &&
         recorder->record_bytes >= limit_bytes &&
         elapsed_ms < 2000)
    {
      usleep(10000);
      elapsed_ms += 10;
    }

  if (recorder->state != BLIND_BADGE_NXRECORDER_IDLE)
    {
      ret = nxrecorder_stop(recorder);
      if (ret < 0)
        {
          printf("[BlindBadge] mic_record: stop failed (%d).\n", ret);
          nxrecorder_release(recorder);
          return 1;
        }
    }

  printf("[BlindBadge] mic_record: captured %lu/%lu byte(s), state=%d.\n",
         (unsigned long)recorder->record_bytes,
         (unsigned long)limit_bytes,
         recorder->state);

  nxrecorder_release(recorder);

  printf("[BlindBadge] mic_record: complete.\n");
  return print_mic_stats_limited(path, seconds * 16000);
#else
  (void)path;
  (void)seconds;
  printf("[BlindBadge] mic_record: CONFIG_SYSTEM_NXRECORDER is disabled.\n");
  return 1;
#endif
}

static int handle_mic_record(int argc, char *argv[])
{
  const char *path = BLIND_BADGE_MIC_DEFAULT_FILE;
  long seconds = BLIND_BADGE_MIC_DEFAULT_SECONDS;
  char *end;

  if (argc > 4)
    {
      printf("[BlindBadge] error: mic_record accepts optional seconds and file.\n");
      return 1;
    }

  if (argc >= 3)
    {
      errno = 0;
      seconds = strtol(argv[2], &end, 10);
      if (errno != 0 || *argv[2] == '\0' || *end != '\0' || seconds < 1 ||
          seconds > BLIND_BADGE_MIC_MAX_SECONDS)
        {
          printf("[BlindBadge] error: seconds must be 1..%d.\n",
                 BLIND_BADGE_MIC_MAX_SECONDS);
          return 1;
        }
    }

  if (argc == 4)
    {
      path = argv[3];
    }

  return record_mic_16bit_file(path, seconds);
}

static int append_arg_text(char *dst, size_t dst_size,
                           int argc, char *argv[], int first_arg,
                           const struct blind_badge_cli_options *options)
{
  int text_argc = argc - first_arg - options->option_count;
  size_t used = 0;
  int i;

  if (text_argc <= 0)
    {
      return -EINVAL;
    }

  dst[0] = '\0';
  for (i = first_arg; i < argc; i++)
    {
      if (strcmp(argv[i], "--ai") == 0 ||
          strcmp(argv[i], "--agent") == 0 ||
          strcmp(argv[i], "--lcd") == 0)
        {
          continue;
        }

      if (used > 0)
        {
          if (used + 1 >= dst_size)
            {
              return -ENOSPC;
            }

          dst[used++] = ' ';
          dst[used] = '\0';
        }

      if (used + strlen(argv[i]) >= dst_size)
        {
          return -ENOSPC;
        }

      strlcat(dst, argv[i], dst_size);
      used = strlen(dst);
    }

  return dst[0] != '\0' ? OK : -EINVAL;
}

static void sanitize_ai_text(char *dst, size_t dst_size, const char *src)
{
  size_t out = 0;

  while (*src != '\0' && out + 1 < dst_size)
    {
      if (*src == '"' || *src == '\\')
        {
          dst[out++] = '\'';
        }
      else if (*src == '\r' || *src == '\n')
        {
          dst[out++] = ' ';
        }
      else
        {
          dst[out++] = *src;
        }

      src++;
    }

  dst[out] = '\0';
}

static int run_voice_ai_text(const char *recognized_text,
                             const struct blind_badge_cli_options *options)
{
  enum blind_badge_ai_mode mode = options->ai_mode;
  char safe_text[256];
  char prompt[BLIND_BADGE_TEXT_SIZE];
  char response[BLIND_BADGE_TEXT_SIZE];
  const char *fallback = "我听到了，请再说一遍需要我帮什么。";
  int used_fallback = 0;
  int ret;

  if (mode == BLIND_BADGE_AI_OFF)
    {
      mode = BLIND_BADGE_AI_DIRECT;
    }

  sanitize_ai_text(safe_text, sizeof(safe_text), recognized_text);
  snprintf(prompt, sizeof(prompt),
           "用户语音识别文本：%s。请作为 BlindBadge 随身助手，"
           "用一句简短中文回复，适合显示在小屏幕上。",
           safe_text);

  printf("[BlindBadge] asr_text: %s\n", recognized_text);
  printf("[BlindBadge] ai_prompt: %s\n", prompt);

  if (options->use_lcd)
    {
      blind_badge_display_show_lines("VOICE", "ASR TEXT",
                                     recognized_text, "AI thinking...");
    }

  ret = blind_badge_ask_ai(mode, prompt, fallback, response,
                           sizeof(response), &used_fallback);
  if (ret != 0)
    {
      return ret;
    }

  printf("[BlindBadge] ai_response: %s\n", response);
  if (options->use_lcd)
    {
      ai_agent_display_ask_result(response, used_fallback ? 0 : 1);
    }

  return 0;
}

static int handle_voice_ai_text(int argc, char *argv[],
                                const struct blind_badge_cli_options *options)
{
  char recognized_text[BLIND_BADGE_TEXT_SIZE];
  int ret;

  ret = append_arg_text(recognized_text, sizeof(recognized_text),
                        argc, argv, 2, options);
  if (ret < 0)
    {
      printf("[BlindBadge] error: voice_ai_text needs recognized text.\n");
      return 1;
    }

  return run_voice_ai_text(recognized_text, options);
}

static int parse_voice_ai_seconds(int argc, char *argv[], long *seconds)
{
  int value_seen = 0;
  int i;

  *seconds = BLIND_BADGE_VOICE_AI_DEFAULT_SECONDS;

  for (i = 2; i < argc; i++)
    {
      char *end;
      long value;

      if (strcmp(argv[i], "--ai") == 0 ||
          strcmp(argv[i], "--agent") == 0 ||
          strcmp(argv[i], "--lcd") == 0)
        {
          continue;
        }

      if (value_seen)
        {
          printf("[BlindBadge] error: voice_ai accepts only one optional "
                 "seconds argument.\n");
          return -EINVAL;
        }

      errno = 0;
      value = strtol(argv[i], &end, 10);
      if (errno != 0 || *argv[i] == '\0' || *end != '\0' ||
          value < 1 || value > BLIND_BADGE_VOICE_AI_MAX_SECONDS)
        {
          printf("[BlindBadge] error: voice_ai seconds must be 1..%d.\n",
                 BLIND_BADGE_VOICE_AI_MAX_SECONDS);
          return -EINVAL;
        }

      *seconds = value;
      value_seen = 1;
    }

  return OK;
}

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
static int load_16bit_pcm(const char *path, long seconds,
                          unsigned char **pcm_out,
                          size_t *pcm_len_out)
{
  size_t max_samples = (size_t)seconds * 16000;
  size_t pcm_cap = max_samples * sizeof(int16_t);
  int16_t samples[256];
  int16_t *pcm16;
  FILE *stream;
  size_t total = 0;
  size_t count;
  int32_t peak16 = 0;

  *pcm_out = NULL;
  *pcm_len_out = 0;

  pcm16 = malloc(pcm_cap);
  if (pcm16 == NULL)
    {
      return -ENOMEM;
    }

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      free(pcm16);
      return -errno;
    }

  while (total < max_samples &&
         (count = fread(samples, sizeof(samples[0]),
                        sizeof(samples) / sizeof(samples[0]), stream)) > 0)
    {
      size_t i;

      for (i = 0; i < count && total < max_samples; i++)
        {
          int16_t sample = samples[i];
          int32_t magnitude = sample < 0 ? -(int32_t)sample :
                                           (int32_t)sample;

          pcm16[total++] = sample;
          if (magnitude > peak16)
            {
              peak16 = magnitude;
            }
        }
    }

  fclose(stream);

  if (total == 0)
    {
      free(pcm16);
      return -ENODATA;
    }

  printf("[BlindBadge] voice_ai: loaded %zu samples as %zu bytes "
         "16-bit PCM, peak16=%ld.\n",
         total, total * sizeof(int16_t), (long)peak16);

  *pcm_out = (unsigned char *)pcm16;
  *pcm_len_out = total * sizeof(int16_t);
  return OK;
}

static int run_voice_ai_from_pcm_file(long seconds,
                                      const struct blind_badge_cli_options *options)
{
  unsigned char *pcm = NULL;
  size_t pcm_len = 0;
  char asr_text[BLIND_BADGE_TEXT_SIZE];
  int ret;

  if (options->use_lcd)
    {
      blind_badge_display_show_lines("VOICE", "RECORDING",
                                     "Please speak now", "");
    }

  ret = record_mic_16bit_file(BLIND_BADGE_VOICE_AI_PCM_FILE, seconds);
  if (ret != 0)
    {
      if (options->use_lcd)
        {
          blind_badge_display_show_lines("VOICE", "REC ERROR",
                                         "mic failed", "");
        }
      return 1;
    }

  if (options->use_lcd)
    {
      blind_badge_display_show_lines("VOICE", "RECOGNIZING",
                                     "ASR running", "");
    }

  ret = load_16bit_pcm(BLIND_BADGE_VOICE_AI_PCM_FILE, seconds,
                       &pcm, &pcm_len);
  if (ret < 0)
    {
      printf("[BlindBadge] voice_ai: PCM conversion failed (%d).\n", ret);
      if (options->use_lcd)
        {
          blind_badge_display_show_lines("VOICE", "PCM ERROR",
                                         "convert failed", "");
        }
      return 1;
    }

  asr_text[0] = '\0';
  printf("[BlindBadge] voice_ai: calling ASR backend with %zu bytes.\n",
         pcm_len);
  ret = voice_asr_recognize(pcm, pcm_len, asr_text, sizeof(asr_text));
  free(pcm);

  if (ret < 0)
    {
      printf("[BlindBadge] voice_ai: ASR failed (%d).\n", ret);
      if (options->use_lcd)
        {
          blind_badge_display_show_lines("VOICE", "ASR ERROR",
                                         "check ASR key/net", "");
        }
      return 1;
    }

  if (asr_text[0] == '\0')
    {
      printf("[BlindBadge] voice_ai: ASR returned empty text.\n");
      if (options->use_lcd)
        {
          blind_badge_display_show_lines("VOICE", "ASR EMPTY",
                                         "Please retry", "");
        }
      return 1;
    }

  return run_voice_ai_text(asr_text, options);
}
#endif

static int handle_voice_ai(int argc, char *argv[],
                           const struct blind_badge_cli_options *options)
{
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  long seconds;
  int ret;

  ret = parse_voice_ai_seconds(argc, argv, &seconds);
  if (ret < 0)
    {
      return 1;
    }

  if (options->use_lcd)
    {
      blind_badge_display_show_lines("VOICE", "RECORDING",
                                     "Please speak now", "");
    }

  printf("[BlindBadge] voice_ai: recording %ld second(s), "
         "then ASR -> AI -> LCD.\n", seconds);
  printf("[BlindBadge] voice_ai: using ESP32-S3-EYE 16-bit PCM capture.\n");
  return run_voice_ai_from_pcm_file(seconds, options);
#else
  (void)argc;
  (void)argv;
  (void)options;
  printf("[BlindBadge] voice_ai: CONFIG_EXAMPLES_AI_AGENT_VELA is disabled.\n");
  return 1;
#endif
}

static int parse_options(int argc, char *argv[],
                         struct blind_badge_cli_options *options)
{
  int i;

  options->ai_mode = BLIND_BADGE_AI_OFF;
#ifdef CONFIG_VIDEO_FB
  options->use_lcd = 1;
#else
  options->use_lcd = 0;
#endif
  options->option_count = 0;

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "--ai") == 0)
        {
          options->ai_mode = BLIND_BADGE_AI_DIRECT;
          options->option_count++;
        }
      else if (strcmp(argv[i], "--agent") == 0)
        {
          options->ai_mode = BLIND_BADGE_AI_AGENT;
          options->option_count++;
        }
      else if (strcmp(argv[i], "--lcd") == 0)
        {
          options->use_lcd = 1;
          options->option_count++;
        }
    }

  return 0;
}

static void print_event_header(const struct blind_badge_event *event,
                               const char *suggestion,
                               const char *ai_prompt)
{
  printf("[BlindBadge] event: %s\n", blind_badge_event_name(event->type));

  if (event->type == BLIND_BADGE_EVENT_STATUS)
    {
      printf("[BlindBadge] status: running\n");
      printf("[BlindBadge] mode: qemu_event_simulation\n");
      printf("[BlindBadge] features: obstacle, step_down, emergency, demo\n");
    }

  if (event->type == BLIND_BADGE_EVENT_OBSTACLE)
    {
      printf("[BlindBadge] distance_cm: %d\n", event->distance_cm);
    }

  if (event->type == BLIND_BADGE_EVENT_OBSTACLE ||
      event->type == BLIND_BADGE_EVENT_STEP_DOWN)
    {
      printf("[BlindBadge] direction: %s\n",
             blind_badge_direction_name(event->direction));
    }

  printf("[BlindBadge] suggestion: %s\n", suggestion);
  printf("[BlindBadge] ai_prompt: %s\n", ai_prompt);
}

static void print_event_ai_result(const struct blind_badge_event *event,
                                  const char *ai_response,
                                  int used_fallback,
                                  int use_lcd)
{
  if (ai_response != NULL && ai_response[0] != '\0')
    {
      printf("[BlindBadge] ai_response: %s\n", ai_response);
      blind_badge_print_actions(event, ai_response, used_fallback);
      if (use_lcd)
        {
          blind_badge_display_show_ai_event(event, ai_response,
                                           used_fallback);
        }
    }
}

static int run_event(struct blind_badge_event *event,
                     const struct blind_badge_cli_options *options)
{
  char suggestion[BLIND_BADGE_TEXT_SIZE];
  char ai_prompt[BLIND_BADGE_TEXT_SIZE];
  char ai_response[BLIND_BADGE_TEXT_SIZE];
  int used_fallback = 0;

  blind_badge_apply_policy(event);
  blind_badge_build_local_suggestion(event, suggestion, sizeof(suggestion));
  blind_badge_build_ai_prompt(event, ai_prompt, sizeof(ai_prompt));
  print_event_header(event, suggestion, ai_prompt);

  if (options->use_lcd)
    {
      blind_badge_display_show_event(event, NULL);
    }

  ai_response[0] = '\0';
  if (options->ai_mode != BLIND_BADGE_AI_OFF)
    {
      if (options->use_lcd)
        {
          blind_badge_display_show_ai_wait(event);
        }

      blind_badge_ask_ai(options->ai_mode, ai_prompt, suggestion, ai_response,
                         sizeof(ai_response), &used_fallback);
      print_event_ai_result(event, ai_response, used_fallback,
                            options->use_lcd);
    }
  return 0;
}

static int handle_status(int argc, const struct blind_badge_cli_options *options)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_STATUS,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };

  if (argc != 2 + options->option_count)
    {
      printf("[BlindBadge] error: status takes no arguments except options.\n");
      print_help();
      return 1;
    }

  return run_event(&event, options);
}

static int handle_boot_display(int argc)
{
  int connected = 0;

  if (argc != 2)
    {
      printf("[BlindBadge] error: boot_display takes no arguments.\n");
      return 1;
    }

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  connected = ai_agent_wifi_reconnect() == OK;
#endif

  blind_badge_display_show_boot_status(connected ? 1 : 0);
  printf("[BlindBadge] boot_display: WiFi %s%s\n",
         connected ? "OK" : "ERROR",
         connected ? "" : ", fallback active");

  return 0;
}

static int handle_boot_timeout(int argc)
{
  int connected = 0;
  struct timespec now;
  struct timespec deadline;

  if (argc != 2)
    {
      printf("[BlindBadge] error: boot_timeout takes no arguments.\n");
      return 1;
    }

  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += BLIND_BADGE_WIFI_UI_TIMEOUT_SECONDS;

  for (;;)
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
        {
          break;
        }

      usleep(100000);
    }

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  connected = network_is_connected() ? 1 : 0;
#endif

  if (!connected)
    {
      blind_badge_display_show_boot_status(0);
      printf("[BlindBadge] boot_timeout: WiFi ERROR after %ds, "
             "fallback active\n",
             BLIND_BADGE_WIFI_UI_TIMEOUT_SECONDS);
    }

  return 0;
}

static int handle_obstacle(int argc, char *argv[],
                           const struct blind_badge_cli_options *options)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_OBSTACLE,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };
  int expected_argc = 4 + options->option_count;

  if (argc != expected_argc)
    {
      printf("[BlindBadge] error: obstacle needs distance and direction.\n");
      print_help();
      return 1;
    }

  event.distance_cm = atoi(argv[2]);
  if (event.distance_cm <= 0)
    {
      printf("[BlindBadge] error: invalid distance_cm.\n");
      return 1;
    }

  if (blind_badge_parse_direction(argv[3], &event.direction) != 0)
    {
      printf("[BlindBadge] error: invalid direction.\n");
      print_help();
      return 1;
    }

  return run_event(&event, options);
}

static int handle_step_down(int argc, char *argv[],
                            const struct blind_badge_cli_options *options)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_STEP_DOWN,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };
  int expected_argc = 3 + options->option_count;

  if (argc != expected_argc)
    {
      printf("[BlindBadge] error: step_down needs direction.\n");
      print_help();
      return 1;
    }

  if (blind_badge_parse_direction(argv[2], &event.direction) != 0)
    {
      printf("[BlindBadge] error: invalid direction.\n");
      print_help();
      return 1;
    }

  return run_event(&event, options);
}

static int handle_emergency(int argc,
                            const struct blind_badge_cli_options *options)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_EMERGENCY,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_EMERGENCY,
      .distance_cm = 0
    };

  if (argc != 2 + options->option_count)
    {
      printf("[BlindBadge] error: emergency takes no arguments except options.\n");
      print_help();
      return 1;
    }

  return run_event(&event, options);
}

static int demo_run_obstacle(int distance_cm,
                             const struct blind_badge_cli_options *options)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_OBSTACLE,
      .direction = BLIND_BADGE_DIR_FRONT,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = distance_cm
    };

  return run_event(&event, options);
}

static int demo_run_step_down(const struct blind_badge_cli_options *options)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_STEP_DOWN,
      .direction = BLIND_BADGE_DIR_FRONT,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };

  return run_event(&event, options);
}

static int demo_run_emergency(const struct blind_badge_cli_options *options)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_EMERGENCY,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_EMERGENCY,
      .distance_cm = 0
    };

  return run_event(&event, options);
}

static int handle_demo(int argc, const struct blind_badge_cli_options *options)
{
  int expected_argc = 2 + options->option_count;
  int ret = 0;

  if (argc != expected_argc)
    {
      printf("[BlindBadge] error: demo takes no arguments except --ai or --agent.\n");
      print_help();
      return 1;
    }

  printf("[BlindBadge] demo: proactive_hazard_alert\n");
  printf("[BlindBadge] demo_stage: obstacle approaching from 120cm to 40cm\n");

  if (demo_run_obstacle(120, options) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: obstacle warning threshold 80cm\n");
  if (demo_run_obstacle(80, options) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: urgent obstacle threshold 40cm\n");
  if (demo_run_obstacle(40, options) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: downward step detected\n");
  if (demo_run_step_down(options) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: emergency help message\n");
  if (demo_run_emergency(options) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo: completed\n");
  return ret;
}

int main(int argc, char *argv[])
{
  struct blind_badge_cli_options options;

  parse_options(argc, argv, &options);

  if (argc < 2)
    {
      printf("BlindBadge started.\n");
      print_help();
      return 0;
    }

  if (strcmp(argv[1], "help") == 0)
    {
      print_help();
      return 0;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      return handle_status(argc, &options);
    }

  if (strcmp(argv[1], "boot_display") == 0)
    {
      return handle_boot_display(argc);
    }

  if (strcmp(argv[1], "boot_timeout") == 0)
    {
      return handle_boot_timeout(argc);
    }

  if (strcmp(argv[1], "obstacle") == 0)
    {
      return handle_obstacle(argc, argv, &options);
    }

  if (strcmp(argv[1], "step_down") == 0)
    {
      return handle_step_down(argc, argv, &options);
    }

  if (strcmp(argv[1], "emergency") == 0)
    {
      return handle_emergency(argc, &options);
    }

  if (strcmp(argv[1], "demo") == 0)
    {
      return handle_demo(argc, &options);
    }

  if (strcmp(argv[1], "lcd_test") == 0)
    {
      if (argc == 2)
        {
          return blind_badge_display_lcd_test() == 0 ? 0 : 1;
        }

      if (argc == 3 && strcmp(argv[2], "zh") == 0)
        {
          return blind_badge_display_lcd_test_zh() == 0 ? 0 : 1;
        }

      if (argc != 2)
        {
          printf("[BlindBadge] error: lcd_test only accepts optional zh.\n");
          print_help();
          return 1;
        }
    }

  if (strcmp(argv[1], "mic_stats") == 0)
    {
      return handle_mic_stats(argc, argv);
    }

  if (strcmp(argv[1], "mic_dump") == 0)
    {
      return handle_mic_dump(argc, argv);
    }

  if (strcmp(argv[1], "mic_record") == 0)
    {
      return handle_mic_record(argc, argv);
    }

  if (strcmp(argv[1], "voice_ai_text") == 0)
    {
      return handle_voice_ai_text(argc, argv, &options);
    }

  if (strcmp(argv[1], "voice_ai") == 0)
    {
      return handle_voice_ai(argc, argv, &options);
    }

  if (strcmp(argv[1], "uart_link_start") == 0)
    {
      if (UART_LINK_Start() != 0)
        {
          return 1;
        }

      /* Keep the NSH background process alive with its server thread. */
      return UART_LINK_Wait() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "uart_link_stop") == 0)
    {
      return UART_LINK_Stop() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "uart_link_status") == 0)
    {
      printf("[UART_LINK] %s\n", UART_LINK_IsConnected() ? "CONNECTED" : "DISCONNECTED");
      return 0;
    }

  if (strcmp(argv[1], "install_skill") == 0)
    {
      if (argc != 2)
        {
          printf("[BlindBadge] error: install_skill takes no arguments.\n");
          print_help();
          return 1;
        }

      return blind_badge_install_skill();
    }

  printf("[BlindBadge] error: unknown command: %s\n", argv[1]);
  print_help();
  return 1;
}
