/****************************************************************************
 * Contest 2026 team 024 - BlindBadge app
 ****************************************************************************/

#include "blind_badge_action.h"
#include "blind_badge_ai.h"
#include "blind_badge_display.h"
#include "blind_badge_event.h"
#include "blind_badge_fall.h"
#include "blind_badge_menu.h"
#include "blind_badge_policy.h"
#include "blind_badge_service.h"
#include "blind_badge_skill.h"
#include "blind_badge_state.h"
#include "blind_badge_vision.h"
#include "blind_badge_vision_server.h"
#include "qma7981.h"
#include "uart_link.h"

#include <nuttx/config.h>
#include <nuttx/sched.h>

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_ADC
#  include <nuttx/analog/adc.h>
#  include <nuttx/analog/ioctl.h>
#endif

#ifdef CONFIG_SYSTEM_NXRECORDER
#  include <nuttx/audio/audio.h>
#  include <system/nxrecorder.h>
#endif

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
#  include "channels/nsh_commands.h"
#  include "infra/network_manager.h"
#  include "voice/voice_asr.h"
#  include "voice/volc_asr.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/fs/ioctl.h>

#define BLIND_BADGE_TEXT_SIZE 512
#define BLIND_BADGE_WIFI_UI_TIMEOUT_SECONDS 60
#define BLIND_BADGE_WIFI_BOOT_RETRIES 3
#define BLIND_BADGE_MIC_DEVICE "/dev/audio/pcm_in0"
#define BLIND_BADGE_NXRECORDER_IDLE 0
#define BLIND_BADGE_MIC_DEFAULT_FILE "/tmp/mic.pcm"
#define BLIND_BADGE_MIC_DEFAULT_SECONDS 3
#define BLIND_BADGE_MIC_MAX_SECONDS 30
#define BLIND_BADGE_MIC_WARMUP_SAMPLES 1600
#define BLIND_BADGE_VOICE_AI_DEFAULT_SECONDS 3
#define BLIND_BADGE_VOICE_AI_MAX_SECONDS 4
#define BLIND_BADGE_VOICE_AI_PCM_FILE "/tmp/voice_ai_16bit.pcm"
#define BLIND_BADGE_BUTTON_DEVICE "/dev/buttons"
#define BLIND_BADGE_BUTTON_POLL_MS 20
#define BLIND_BADGE_BUTTON_DN_DOUBLE_MS 1200
#define BLIND_BADGE_BUTTON_WIFI_STACK_SIZE 8192
#define BLIND_BADGE_BUTTON_ADC_DEVICE "/dev/adc0"
#define BLIND_BADGE_BUTTON_DN_MIN_MV 900
#define BLIND_BADGE_BUTTON_DN_MAX_MV 1300
#define BLIND_BADGE_BUTTON_MENU_MIN_MV 2050
#define BLIND_BADGE_BUTTON_MENU_MAX_MV 2250
#define BLIND_BADGE_BUTTON_PLAY_MIN_MV 1750
#define BLIND_BADGE_BUTTON_PLAY_MAX_MV 1950
#define BLIND_BADGE_BUTTON_ADC_DEBOUNCE_SAMPLES 3
#define BLIND_BADGE_BUTTON_PLAY_DOUBLE_MS 600
#define BLIND_BADGE_WIFI_SD_BLOCK_DEVICE "/dev/mmcsd1"
#define BLIND_BADGE_WIFI_SD_MOUNT_POINT "/mnt/sdcard"
#define BLIND_BADGE_WIFI_SD_CONFIG "/mnt/sdcard/WIFI.CFG"
#define BLIND_BADGE_AI_SD_KEY "/mnt/sdcard/MIMO.KEY"
#define BLIND_BADGE_WIFI_SSID_SIZE 33
#define BLIND_BADGE_WIFI_PASSWORD_SIZE 64
#define BLIND_BADGE_AI_SD_STACK_SIZE 8192
#define BLIND_BADGE_VISION_DEFAULT_PORT 45681
#define BLIND_BADGE_VISION_HTTP_DEFAULT_PORT 80

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
  printf("  blind_badge_app state [STATE]\n");
  printf("  blind_badge_app button_service|button_stop|button_status\n");
  printf("  blind_badge_app menu_show|menu_next|menu_status\n");
  printf("  blind_badge_app play|back|service_status\n");
  printf("  blind_badge_app lcd_test [zh]\n");
  printf("  blind_badge_app mic_record [seconds] [16bit_pcm_file]\n");
  printf("  blind_badge_app mic_stats <16bit_pcm_file>\n");
  printf("  blind_badge_app mic_dump <16bit_pcm_file> [max_bytes]\n");
  printf("  blind_badge_app voice_asr [seconds] [--lcd]\n");
  printf("  blind_badge_app voice_ai [seconds] [--ai|--agent] [--lcd]\n");
  printf("  blind_badge_app voice_ai_text <recognized_text...> [--ai|--agent]\n");
  printf("  blind_badge_app install_skill\n");
  printf("  blind_badge_app fall_start [--ai|--agent] [--lcd]\n");
  printf("  blind_badge_app fall_stop\n");
  printf("  blind_badge_app fall_status\n");
  printf("  blind_badge_app fall_read\n");
  printf("  blind_badge_app accel_read [samples]\n");
  printf("  blind_badge_app vision_start <server_ipv4> [port]\n");
  printf("  blind_badge_app vision_once <server_ipv4> [port]\n");
  printf("  blind_badge_app vision_stop|vision_status\n");
  printf("  blind_badge_app vision_server [port]\n");
  printf("  blind_badge_app vision_server_stop|vision_server_status\n");
  printf("  blind_badge_app vision_camera_test\n");
  printf("  blind_badge_app vision_camera_bench [frames]\n");
  printf("  blind_badge_app uart_link_start|uart_link_stop|uart_link_status\n");
  printf("  blind_badge_app uart_link_send <UPPERCASE_FLAG>\n");
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

static int record_mic_raw_file(const char *path, long seconds, int bits)
{
#ifdef CONFIG_SYSTEM_NXRECORDER
  struct nxrecorder_s *recorder;
  uint32_t limit_bytes;
  uint32_t captured_bytes;
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

  limit_bytes = (seconds * 16000 + BLIND_BADGE_MIC_WARMUP_SAMPLES) *
                (bits / 8);
  recorder->record_limit_bytes = limit_bytes;
  recorder->record_bytes = 0;

  ret = nxrecorder_recordinternal(recorder, path, AUDIO_FMT_PCM,
                                  1, bits, 16000, 0);
  if (ret < 0)
    {
      printf("[BlindBadge] mic_record: start failed (%d).\n", ret);
      nxrecorder_release(recorder);
      return 1;
    }

  printf("[BlindBadge] mic_record: recording %ld second(s), "
         "mono 16000 Hz %d-bit raw PCM -> %s\n", seconds, bits, path);

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

  captured_bytes = recorder->record_bytes;
  nxrecorder_release(recorder);

  if (captured_bytes < limit_bytes)
    {
      printf("[BlindBadge] mic_record: incomplete capture; refusing ASR.\n");
      return 1;
    }

  printf("[BlindBadge] mic_record: complete.\n");
  return 0;
#else
  (void)path;
  (void)seconds;
  printf("[BlindBadge] mic_record: CONFIG_SYSTEM_NXRECORDER is disabled.\n");
  return 1;
#endif
}

static int record_mic_16bit_file(const char *path, long seconds)
{
  uint32_t samples[256];
  uint16_t pcm[256];
  size_t count;
  size_t total = 0;
  size_t i;
  FILE *input;
  FILE *output;
  char *raw_path;
  int ret;

  raw_path = malloc(strlen(path) + sizeof(".i2s32"));
  if (raw_path == NULL)
    {
      return 1;
    }

  sprintf(raw_path, "%s.i2s32", path);
  ret = record_mic_raw_file(raw_path, seconds, 32);
  if (ret != 0)
    {
      unlink(raw_path);
      free(raw_path);
      return ret;
    }

  input = fopen(raw_path, "rb");
  if (input != NULL &&
      fseek(input, BLIND_BADGE_MIC_WARMUP_SAMPLES * sizeof(uint32_t),
            SEEK_SET) != 0)
    {
      fclose(input);
      input = NULL;
    }

  output = input != NULL ? fopen(path, "wb") : NULL;
  if (output == NULL)
    {
      if (input != NULL)
        {
          fclose(input);
        }

      unlink(raw_path);
      free(raw_path);
      return 1;
    }

  /* Discard the microphone's first 100 ms to exclude its startup transient.
   * The microphone supplies MSB-aligned samples in 32-bit I2S slots.
   * Keep the signed high 16 bits for little-endian ASR PCM.
   */

  while ((count = fread(samples, sizeof(samples[0]), 256, input)) > 0)
    {
      for (i = 0; i < count; i++)
        {
          pcm[i] = (uint16_t)(samples[i] >> 16);
        }

      if (fwrite(pcm, sizeof(pcm[0]), count, output) != count)
        {
          ret = 1;
          break;
        }

      total += count;
    }

  if (ferror(input) || total != (size_t)seconds * 16000)
    {
      ret = 1;
    }

  fclose(input);
  if (fclose(output) != 0)
    {
      ret = 1;
    }

  unlink(raw_path);
  free(raw_path);
  if (ret != 0)
    {
      printf("[BlindBadge] mic_record: 32-to-16-bit conversion failed.\n");
      return ret;
    }

  printf("[BlindBadge] mic_record: converted %lu samples to 16-bit PCM.\n",
         (unsigned long)total);
  return print_mic_stats_limited(path, seconds * 16000);
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
  blind_badge_state_set(BLIND_BADGE_STATE_PROCESSING, "voice request");

  if (options->use_lcd)
    {
      blind_badge_display_show_lines("VOICE", "ASR TEXT",
                                     recognized_text, "AI thinking...");
    }

  ret = blind_badge_ask_ai(mode, prompt, fallback, response,
                           sizeof(response), &used_fallback);
  if (ret != 0)
    {
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "voice AI failed");
      return ret;
    }

  printf("[BlindBadge] ai_response: %s\n", response);
  blind_badge_state_set(BLIND_BADGE_STATE_RESULT,
                        used_fallback ? "local fallback" : "voice reply");
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

static int parse_voice_seconds(const char *command, int argc, char *argv[],
                               long *seconds)
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
          printf("[BlindBadge] error: %s accepts only one optional "
                 "seconds argument.\n", command);
          return -EINVAL;
        }

      errno = 0;
      value = strtol(argv[i], &end, 10);
      if (errno != 0 || *argv[i] == '\0' || *end != '\0' ||
          value < 1 || value > BLIND_BADGE_VOICE_AI_MAX_SECONDS)
        {
          printf("[BlindBadge] error: %s seconds must be 1..%d.\n",
                 command, BLIND_BADGE_VOICE_AI_MAX_SECONDS);
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

static int run_voice_asr_from_pcm_file(long seconds,
                                       const struct blind_badge_cli_options *options,
                                       char *asr_text, size_t asr_text_size)
{
  unsigned char *pcm = NULL;
  size_t pcm_len = 0;
  int ret;

  if (asr_text == NULL || asr_text_size == 0)
    {
      return 1;
    }

  if (voice_asr_get_backend() == NULL)
    {
      ret = volc_asr_register();
      if (ret < 0)
        {
          printf("[BlindBadge] voice_ai: cannot register ASR backend (%d).\n",
                 ret);
          blind_badge_state_set(BLIND_BADGE_STATE_ERROR,
                                "ASR backend unavailable");
          return 1;
        }
    }

  printf("[BlindBadge] voice_ai: ASR backend=%s.\n",
         voice_asr_get_backend());
  blind_badge_state_set(BLIND_BADGE_STATE_RECORDING, "microphone active");

  if (options->use_lcd)
    {
      blind_badge_display_show_lines("VOICE", "RECORDING",
                                     "Please speak now", "");
    }

  ret = record_mic_16bit_file(BLIND_BADGE_VOICE_AI_PCM_FILE, seconds);
  if (ret != 0)
    {
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "recording failed");
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

  blind_badge_state_set(BLIND_BADGE_STATE_PROCESSING, "speech recognition");

  ret = load_16bit_pcm(BLIND_BADGE_VOICE_AI_PCM_FILE, seconds,
                       &pcm, &pcm_len);
  if (ret < 0)
    {
      printf("[BlindBadge] voice_ai: PCM conversion failed (%d).\n", ret);
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "PCM conversion failed");
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
  ret = voice_asr_recognize(pcm, pcm_len, asr_text, asr_text_size);
  free(pcm);

  if (ret < 0)
    {
      printf("[BlindBadge] voice_ai: ASR failed (%d).\n", ret);
      if (ret == -ECONNREFUSED)
        {
          printf("[BlindBadge] voice_asr: cannot connect to ASR server; "
                 "check DNS, port 443, and proxy settings.\n");
        }
      else if (ret == -ENOENT)
        {
          printf("[BlindBadge] voice_asr: ASR credentials are not set.\n");
        }
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "ASR failed");
      if (options->use_lcd)
        {
          if (ret == -ENODATA)
            {
              blind_badge_display_show_lines("VOICE", "NO SPEECH",
                                             "Please try again", "");
            }
          else if (ret == -ECONNREFUSED || ret == -EIO)
            {
              blind_badge_display_show_lines("VOICE", "NETWORK ERROR",
                                             "Please try again", "");
            }
          else
            {
              blind_badge_display_show_lines("VOICE", "ASR ERROR",
                                             "check ASR key/net", "");
            }
        }
      return 1;
    }

  if (asr_text[0] == '\0')
    {
      printf("[BlindBadge] voice_ai: ASR returned empty text.\n");
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "ASR returned empty");
      if (options->use_lcd)
        {
          blind_badge_display_show_lines("VOICE", "ASR EMPTY",
                                         "Please retry", "");
        }
      return 1;
    }

  printf("[BlindBadge] asr_text: %s\n", asr_text);
  blind_badge_state_set(BLIND_BADGE_STATE_RESULT, "speech recognized");
  if (options->use_lcd)
    {
      blind_badge_display_show_lines("VOICE", "RECOGNIZED", asr_text, "");
    }

  return 0;
}
#endif

static int handle_voice_asr(int argc, char *argv[],
                            const struct blind_badge_cli_options *options)
{
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  char asr_text[BLIND_BADGE_TEXT_SIZE];
  long seconds;

  if (parse_voice_seconds("voice_asr", argc, argv, &seconds) < 0)
    {
      return 1;
    }

  printf("[BlindBadge] voice_asr: press accepted; speak for %ld second(s).\n",
         seconds);
  return run_voice_asr_from_pcm_file(seconds, options, asr_text,
                                     sizeof(asr_text));
#else
  (void)argc;
  (void)argv;
  (void)options;
  printf("[BlindBadge] voice_asr: CONFIG_EXAMPLES_AI_AGENT_VELA is disabled.\n");
  return 1;
#endif
}

static int handle_voice_ai(int argc, char *argv[],
                           const struct blind_badge_cli_options *options)
{
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  long seconds;
  char asr_text[BLIND_BADGE_TEXT_SIZE];
  int ret;

  ret = parse_voice_seconds("voice_ai", argc, argv, &seconds);
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
  ret = run_voice_asr_from_pcm_file(seconds, options, asr_text,
                                    sizeof(asr_text));
  if (ret != 0)
    {
      return ret;
    }

  return run_voice_ai_text(asr_text, options);
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

  if (event->type == BLIND_BADGE_EVENT_EMERGENCY)
    {
      blind_badge_state_set(BLIND_BADGE_STATE_EMERGENCY,
                            "emergency event");
    }
  else
    {
      blind_badge_state_set(BLIND_BADGE_STATE_PROCESSING,
                            blind_badge_event_name(event->type));
    }

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
  else
    {
      strncpy(ai_response, suggestion, sizeof(ai_response) - 1);
      ai_response[sizeof(ai_response) - 1] = '\0';
      print_event_ai_result(event, ai_response, 1, options->use_lcd);
    }

  if (event->type != BLIND_BADGE_EVENT_EMERGENCY)
    {
      blind_badge_state_set(BLIND_BADGE_STATE_RESULT,
                            blind_badge_event_name(event->type));
      if (options->use_lcd)
        {
          blind_badge_display_show_ai_event(event, ai_response,
                                            used_fallback ||
                                            options->ai_mode ==
                                            BLIND_BADGE_AI_OFF);
        }
    }
  return 0;
}

static void handle_uart_link_event(const struct uart_link_event *link_event,
                                   void *arg)
{
  struct blind_badge_cli_options options =
    {
      .ai_mode = BLIND_BADGE_AI_OFF,
#ifdef CONFIG_VIDEO_FB
      .use_lcd = 1,
#else
      .use_lcd = 0,
#endif
      .option_count = 0
    };
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_STATUS,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };

  (void)arg;
  if (link_event == NULL)
    {
      return;
    }

  switch (link_event->type)
    {
      case UART_LINK_EVENT_OBSTACLE:
        event.type = BLIND_BADGE_EVENT_OBSTACLE;
        event.distance_cm = link_event->distance_cm;
        if (blind_badge_parse_direction(link_event->direction,
                                        &event.direction) != 0)
          {
            printf("[BlindBadge] uart_event: invalid direction=%s\n",
                   link_event->direction);
            return;
          }
        break;

      case UART_LINK_EVENT_STEP_DOWN:
        event.type = BLIND_BADGE_EVENT_STEP_DOWN;
        if (blind_badge_parse_direction(link_event->direction,
                                        &event.direction) != 0)
          {
            printf("[BlindBadge] uart_event: invalid direction=%s\n",
                   link_event->direction);
            return;
          }
        break;

      case UART_LINK_EVENT_EMERGENCY:
        event.type = BLIND_BADGE_EVENT_EMERGENCY;
        event.severity = BLIND_BADGE_SEVERITY_EMERGENCY;
        break;

      case UART_LINK_EVENT_ASR_RESULT:
        (void)blind_badge_service_submit_asr(link_event->text);
        return;

      case UART_LINK_EVENT_VOICE_IDLE:
        blind_badge_service_voice_idle();
        return;

      case UART_LINK_EVENT_HEALTH_DONE:
        if (blind_badge_service_health_done(link_event->heart_rate_bpm) == 0)
          {
            blind_badge_menu_request_leave();
          }

        return;

      default:
        return;
    }

  printf("[BlindBadge] uart_event: confidence=%d source=%s\n",
         link_event->confidence,
         link_event->source[0] != '\0' ? link_event->source : "sub_board");
  run_event(&event, &options);
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
  int attempt;
  int connected = 0;

  if (argc != 2)
    {
      printf("[BlindBadge] error: boot_display takes no arguments.\n");
      return 1;
    }

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  for (attempt = 1; attempt <= BLIND_BADGE_WIFI_BOOT_RETRIES; attempt++)
    {
      connected = ai_agent_wifi_reconnect() == OK;
      if (connected)
        {
          break;
        }

      if (attempt < BLIND_BADGE_WIFI_BOOT_RETRIES)
        {
          printf("[BlindBadge] boot_display: WiFi attempt %d/%d failed; "
                 "retrying.\n",
                 attempt, BLIND_BADGE_WIFI_BOOT_RETRIES);
          sleep(1);
        }
    }
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
      blind_badge_state_set(BLIND_BADGE_STATE_OFFLINE, "WiFi timeout");
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

static volatile int g_button_service_running;
static pthread_mutex_t g_button_wifi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_sd_mount_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_button_wifi_running;
static bool g_wifi_sd_mounted;

static void fall_detect_callback(void *arg);

static void blind_badge_wifi_strip_line_end(char *line)
{
  size_t length = strlen(line);

  while (length > 0 &&
         (line[length - 1] == '\r' || line[length - 1] == '\n'))
    {
      line[--length] = '\0';
    }
}

static int blind_badge_wifi_mount_sd(void)
{
#if defined(CONFIG_ESP32S3_SDMMC) && defined(CONFIG_FS_FAT)
  struct stat file_stat;
  int probe_fd;
  int probe_ret = ERROR;
  int attempt;
  int ret = ERROR;

  pthread_mutex_lock(&g_sd_mount_lock);
  if (g_wifi_sd_mounted)
    {
      ret = OK;
      goto out;
    }

  mkdir("/mnt", 0755);
  mkdir(BLIND_BADGE_WIFI_SD_MOUNT_POINT, 0755);

  for (attempt = 0; attempt < 50; attempt++)
    {
      if (stat(BLIND_BADGE_WIFI_SD_BLOCK_DEVICE, &file_stat) == 0)
        {
          break;
        }

      usleep(100000);
    }

  if (attempt == 50)
    {
      goto out;
    }

  probe_fd = open(BLIND_BADGE_WIFI_SD_BLOCK_DEVICE, O_RDONLY);
  if (probe_fd < 0)
    {
      goto out;
    }

  /* The block node is registered even when the card's power-on probe has
   * not completed successfully.  Re-probe explicitly before FAT touches
   * the device.  This board has no card-detect signal, so each BIOC_PROBE
   * performs a real probe rather than trusting a stale insertion state.
   */

  for (attempt = 0; attempt < 10; attempt++)
    {
      probe_ret = ioctl(probe_fd, BIOC_PROBE, 0);
      if (probe_ret == OK)
        {
          break;
        }

      usleep(500000);
    }

  close(probe_fd);
  if (probe_ret != OK)
    {
      goto out;
    }

  if (mount(BLIND_BADGE_WIFI_SD_BLOCK_DEVICE,
            BLIND_BADGE_WIFI_SD_MOUNT_POINT, "vfat", 0, NULL) < 0 &&
      errno != EBUSY)
    {
      goto out;
    }

  g_wifi_sd_mounted = true;
  ret = OK;
out:
  pthread_mutex_unlock(&g_sd_mount_lock);
  return ret;
#else
  return ERROR;
#endif
}

static int blind_badge_wifi_load_sd_config(char *ssid, size_t ssid_size,
                                           char *password,
                                           size_t password_size)
{
  char line[128];
  bool have_ssid = false;
  bool have_password = false;
  FILE *file;

  if (blind_badge_wifi_mount_sd() != OK)
    {
      return ERROR;
    }

  file = fopen(BLIND_BADGE_WIFI_SD_CONFIG, "r");
  if (file == NULL)
    {
      return ERROR;
    }

  while (fgets(line, sizeof(line), file) != NULL)
    {
      char *entry = line;
      size_t value_length;

      blind_badge_wifi_strip_line_end(line);
      if (strlen(entry) >= 3 &&
          (unsigned char)entry[0] == 0xef &&
          (unsigned char)entry[1] == 0xbb &&
          (unsigned char)entry[2] == 0xbf)
        {
          entry += 3;
        }

      if (strncmp(entry, "SSID=", 5) == 0)
        {
          value_length = strlen(entry + 5);
          if (value_length == 0 || value_length >= ssid_size)
            {
              fclose(file);
              return ERROR;
            }

          memcpy(ssid, entry + 5, value_length + 1);
          have_ssid = true;
        }
      else if (strncmp(entry, "PASSWORD=", 9) == 0)
        {
          value_length = strlen(entry + 9);
          if (value_length >= password_size ||
              (value_length > 0 && value_length < 8))
            {
              fclose(file);
              return ERROR;
            }

          memcpy(password, entry + 9, value_length + 1);
          have_password = true;
        }
    }

  fclose(file);
  return have_ssid && have_password ? OK : ERROR;
}

static void *blind_badge_ai_sd_worker(void *arg)
{
  (void)arg;

  if (blind_badge_wifi_mount_sd() == OK &&
      blind_badge_ai_import_mimo(BLIND_BADGE_AI_SD_KEY) == 0)
    {
      printf("[BlindBadge] AI configuration: ready from SD\n");
    }
  else
    {
      printf("[BlindBadge] AI configuration: unavailable\n");
    }

  return NULL;
}

static void blind_badge_start_ai_sd_worker(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, BLIND_BADGE_AI_SD_STACK_SIZE);
  ret = pthread_create(&thread, &attr, blind_badge_ai_sd_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret == 0)
    {
      pthread_detach(thread);
    }
  else
    {
      printf("[BlindBadge] AI configuration: loader start failed\n");
    }
}

static void *button_wifi_connect_worker(void *arg)
{
  char ssid[BLIND_BADGE_WIFI_SSID_SIZE] = {0};
  char password[BLIND_BADGE_WIFI_PASSWORD_SIZE] = {0};
  int ret = ERROR;

  (void)arg;
  blind_badge_display_show_boot_status(-1);

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  if (network_is_connected())
    {
      ret = OK;
    }
  else if (blind_badge_wifi_load_sd_config(ssid, sizeof(ssid),
                                           password, sizeof(password)) == OK)
    {
      ret = network_wifi_connect_boot(NULL, ssid, password);
    }
#endif

  memset(password, 0, sizeof(password));
  memset(ssid, 0, sizeof(ssid));

  if (ret == OK)
    {
      UART_LINK_SetEventHandler(handle_uart_link_event, NULL);
      if (UART_LINK_Start() != 0)
        {
          printf("[BlindBadge] button.dn: link service start failed\n");
        }

      blind_badge_state_set_quiet(BLIND_BADGE_STATE_IDLE,
                                  "DN WiFi connected");
      blind_badge_display_show_boot_status(1);
    }
  else
    {
      blind_badge_state_set_quiet(BLIND_BADGE_STATE_OFFLINE,
                                  "DN WiFi connect failed");
      blind_badge_display_show_boot_status(0);
    }

  usleep(800000);
  blind_badge_menu_show();

  pthread_mutex_lock(&g_button_wifi_lock);
  g_button_wifi_running = 0;
  pthread_mutex_unlock(&g_button_wifi_lock);
  return NULL;
}

static int start_button_wifi_connect_worker(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  pthread_mutex_lock(&g_button_wifi_lock);
  if (g_button_wifi_running)
    {
      pthread_mutex_unlock(&g_button_wifi_lock);
      printf("[BlindBadge] button.dn: WiFi action already running\n");
      return -EBUSY;
    }

  g_button_wifi_running = 1;
  pthread_mutex_unlock(&g_button_wifi_lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, BLIND_BADGE_BUTTON_WIFI_STACK_SIZE);
  ret = pthread_create(&thread, &attr, button_wifi_connect_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_button_wifi_lock);
      g_button_wifi_running = 0;
      pthread_mutex_unlock(&g_button_wifi_lock);
      printf("[BlindBadge] button.dn: worker start failed: %d\n", ret);
      return -ret;
    }

  pthread_detach(thread);
  return 0;
}

static void handle_dn_short_press(void)
{
  (void)start_button_wifi_connect_worker();
}

static void handle_dn_double_press(void)
{
  int ret = ERROR;

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  ret = network_wifi_disconnect(NULL);
#endif
  if (ret == OK ||
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
      !network_is_connected()
#else
      false
#endif
      )
    {
      blind_badge_state_set_quiet(BLIND_BADGE_STATE_OFFLINE,
                                  "DN WiFi disconnected");
      blind_badge_menu_show();
    }
}

/* PLAY: single click enters the service page of the selected entry and
 * starts its preset task.  Double click stops that task and returns to the
 * menu list.
 */

static void handle_play_short_press(void)
{
  (void)blind_badge_menu_enter();
}

static void handle_play_double_press(void)
{
  (void)blind_badge_menu_leave();
}

#ifdef CONFIG_ADC
static int read_adc_millivolts(int fd, int *millivolts)
{
  struct adc_msg_s sample;
  ssize_t nread;

  if (ioctl(fd, ANIOC_TRIGGER, 0) < 0)
    {
      return -errno;
    }

  nread = read(fd, &sample, sizeof(sample));
  if (nread != sizeof(sample))
    {
      return nread < 0 ? -errno : -EIO;
    }

  *millivolts = sample.am_channel == 0 ? (int)sample.am_data : -1;
  return 0;
}

static bool adc_in_band(int millivolts, int low, int high)
{
  return millivolts >= low && millivolts <= high;
}
#endif

static uint64_t monotonic_milliseconds(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int handle_button_service(int argc)
{
#ifdef CONFIG_INPUT_BUTTONS
  btn_buttonset_t buttons = 0;
  uint64_t menu_refresh_ms = 0;
  int fd;
#ifdef CONFIG_ADC
  bool dn_pressed = false;
  bool dn_candidate = false;
  int dn_stable_samples = 0;
  int dn_pending_clicks = 0;
  uint64_t dn_click_deadline_ms = 0;
  bool menu_pressed = false;
  bool menu_candidate = false;
  int menu_stable_samples = 0;
  bool play_pressed = false;
  bool play_candidate = false;
  int play_stable_samples = 0;
  int play_pending_clicks = 0;
  uint64_t play_click_deadline_ms = 0;
  int adc_fd = -1;
#endif

  if (argc != 2)
    {
      printf("[BlindBadge] error: button_service takes no arguments.\n");
      return 1;
    }

  if (g_button_service_running)
    {
      printf("[BlindBadge] button_service: already running\n");
      return 0;
    }

  fd = open(BLIND_BADGE_BUTTON_DEVICE, O_RDONLY);
  if (fd < 0)
    {
      printf("[BlindBadge] button_service: open %s failed: errno=%d\n",
             BLIND_BADGE_BUTTON_DEVICE, errno);
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR,
                            "button device unavailable");
      return 1;
    }

#ifdef CONFIG_ADC
  adc_fd = open(BLIND_BADGE_BUTTON_ADC_DEVICE, O_RDONLY);
  if (adc_fd < 0)
    {
      printf("[BlindBadge] button_service: open %s failed: errno=%d; "
             "BOOT key remains available\n",
             BLIND_BADGE_BUTTON_ADC_DEVICE, errno);
    }
#endif

  g_button_service_running = 1;
  blind_badge_menu_show();
  blind_badge_start_ai_sd_worker();
  if (fall_detect_start("/dev/i2c0", NULL,
                        fall_detect_callback, NULL) < 0)
    {
      printf("[BlindBadge] passive alert: accelerometer unavailable\n");
    }

  while (g_button_service_running)
    {
      uint64_t now_ms = monotonic_milliseconds();
      ssize_t nread = read(fd, &buttons, sizeof(buttons));

      if (nread != sizeof(buttons))
        {
          printf("[BlindBadge] button_service: read failed: errno=%d\n",
                 errno);
          blind_badge_state_set(BLIND_BADGE_STATE_ERROR,
                                "button read failed");
          break;
        }

      /* The discrete BOOT key intentionally has no application action.
       * MENU, PLAY and DN- are the only runtime controls and are decoded
       * from the ADC ladder below.
       */

#ifdef CONFIG_ADC
      if (adc_fd >= 0)
        {
          bool dn_raw_pressed = false;
          bool menu_raw_pressed = false;
          bool play_raw_pressed = false;
          int millivolts = -1;
          int adc_ret;

          adc_ret = read_adc_millivolts(adc_fd, &millivolts);
          if (adc_ret == 0)
            {
              dn_raw_pressed =
                adc_in_band(millivolts, BLIND_BADGE_BUTTON_DN_MIN_MV,
                            BLIND_BADGE_BUTTON_DN_MAX_MV);
              menu_raw_pressed =
                adc_in_band(millivolts, BLIND_BADGE_BUTTON_MENU_MIN_MV,
                            BLIND_BADGE_BUTTON_MENU_MAX_MV);
              play_raw_pressed =
                adc_in_band(millivolts, BLIND_BADGE_BUTTON_PLAY_MIN_MV,
                            BLIND_BADGE_BUTTON_PLAY_MAX_MV);

              if (dn_raw_pressed == dn_candidate)
                {
                  if (dn_stable_samples <
                      BLIND_BADGE_BUTTON_ADC_DEBOUNCE_SAMPLES)
                    {
                      dn_stable_samples++;
                    }
                }
              else
                {
                  dn_candidate = dn_raw_pressed;
                  dn_stable_samples = 1;
                }

              if (dn_stable_samples >=
                  BLIND_BADGE_BUTTON_ADC_DEBOUNCE_SAMPLES &&
                  dn_candidate != dn_pressed)
                {
                  dn_pressed = dn_candidate;
                  if (!dn_pressed)
                    {
                      dn_pending_clicks++;
                      if (dn_pending_clicks >= 2)
                        {
                          dn_pending_clicks = 0;
                          handle_dn_double_press();
                        }
                      else
                        {
                          dn_click_deadline_ms =
                            now_ms + BLIND_BADGE_BUTTON_DN_DOUBLE_MS;
                        }
                    }
                }

              if (menu_raw_pressed == menu_candidate)
                {
                  if (menu_stable_samples <
                      BLIND_BADGE_BUTTON_ADC_DEBOUNCE_SAMPLES)
                    {
                      menu_stable_samples++;
                    }
                }
              else
                {
                  menu_candidate = menu_raw_pressed;
                  menu_stable_samples = 1;
                }

              if (menu_stable_samples >=
                  BLIND_BADGE_BUTTON_ADC_DEBOUNCE_SAMPLES &&
                  menu_candidate != menu_pressed)
                {
                  menu_pressed = menu_candidate;
                  if (!menu_pressed)
                    {
                      blind_badge_menu_next();
                    }
                }

              if (play_raw_pressed == play_candidate)
                {
                  if (play_stable_samples <
                      BLIND_BADGE_BUTTON_ADC_DEBOUNCE_SAMPLES)
                    {
                      play_stable_samples++;
                    }
                }
              else
                {
                  play_candidate = play_raw_pressed;
                  play_stable_samples = 1;
                }

              if (play_stable_samples >=
                  BLIND_BADGE_BUTTON_ADC_DEBOUNCE_SAMPLES &&
                  play_candidate != play_pressed)
                {
                  play_pressed = play_candidate;
                  if (!play_pressed)
                    {
                      play_pending_clicks++;
                      if (play_pending_clicks >= 2)
                        {
                          play_pending_clicks = 0;
                          handle_play_double_press();
                        }
                      else
                        {
                          play_click_deadline_ms =
                            now_ms + BLIND_BADGE_BUTTON_PLAY_DOUBLE_MS;
                        }
                    }
                }
            }
        }

      if (!dn_pressed && dn_pending_clicks == 1 &&
          now_ms >= dn_click_deadline_ms)
        {
          dn_pending_clicks = 0;
          handle_dn_short_press();
        }

      if (!play_pressed && play_pending_clicks == 1 &&
          now_ms >= play_click_deadline_ms)
        {
          play_pending_clicks = 0;
          handle_play_short_press();
        }
#endif

      /* refresh() is cached: this only redraws once when STARTING changes
       * to RUNNING/ERROR, then costs no framebuffer work while streaming.
       */

      if (now_ms >= menu_refresh_ms)
        {
          blind_badge_menu_refresh();
          menu_refresh_ms = now_ms + 250;
        }

      usleep(BLIND_BADGE_BUTTON_POLL_MS * 1000);
    }

#ifdef CONFIG_ADC
  if (adc_fd >= 0)
    {
      close(adc_fd);
    }
#endif
  (void)fall_detect_stop();
  close(fd);
  g_button_service_running = 0;
  printf("[BlindBadge] button_service: stopped\n");
  return 0;
#else
  (void)argc;
  printf("[BlindBadge] button_service: CONFIG_INPUT_BUTTONS is disabled.\n");
  return 1;
#endif
}

static int handle_state_command(int argc, char *argv[])
{
  enum blind_badge_state state;

  if (argc == 2)
    {
      return blind_badge_state_show() == 0 ? 0 : 1;
    }

  if (argc != 3 || blind_badge_state_parse(argv[2], &state) != 0)
    {
      printf("[BlindBadge] error: state needs one of IDLE, LISTENING, "
             "RECORDING, CAPTURING, PROCESSING, RESULT, OFFLINE, ERROR, "
             "EMERGENCY.\n");
      return 1;
    }

  return blind_badge_state_set(state, "manual command") == 0 ? 0 : 1;
}

/****************************************************************************
 * Fall detection
 ****************************************************************************/

static void fall_detect_callback(void *arg)
{
  (void)arg;
  printf("[BlindBadge] fall_detect: requesting service preemption\n");
  blind_badge_menu_request_alert();
}

static int handle_fall_start(int argc, char *argv[],
                             struct blind_badge_cli_options *options)
{
  int ret;

  ret = parse_options(argc, argv, options);
  if (ret < 0)
    {
      return ret;
    }

  ret = fall_detect_start("/dev/i2c0", NULL, fall_detect_callback, NULL);
  if (ret < 0)
    {
      printf("[BlindBadge] error: fall_detect_start failed (%d)\n", ret);
      return 1;
    }

  return 0;
}

static int handle_fall_stop(int argc)
{
  if (argc != 2)
    {
      printf("[BlindBadge] error: fall_stop takes no arguments.\n");
      return 1;
    }

  return fall_detect_stop() == 0 ? 0 : 1;
}

static int handle_fall_status(int argc)
{
  struct fall_detect_state st;
  static const char *phase_names[] =
  {
    "IDLE", "DOWNWARD", "IMPACT", "STILLNESS"
  };

  if (argc != 2)
    {
      printf("[BlindBadge] error: fall_status takes no arguments.\n");
      return 1;
    }

  fall_detect_get_state(&st);
  printf("[BlindBadge] fall_status: running=%d phase=%s "
         "magnitude=%.3fg x=%.3fg down_delta=%.3fg "
         "baseline_x=%.3fg trigger_delta=%.3fg "
         "fall_count=%d cooldown=%dms\n",
         st.running,
         st.phase < 4 ? phase_names[st.phase] : "UNKNOWN",
         (double)st.last_magnitude,
         (double)st.last_x,
         (double)st.last_downward_delta,
         (double)st.baseline_x,
         (double)st.trigger_delta, st.fall_count,
         st.cooldown_remaining_ms);
  return 0;
}

static int handle_accel_read(int argc, char *argv[], bool legacy)
{
  struct qma7981_dev dev;
  struct qma7981_sample sample;
  char *end;
  long samples = 1;
  long i;
  int ret;

  if (legacy)
    {
      if (argc != 2)
        {
          printf("[BlindBadge] error: fall_read takes no arguments.\n");
          return 1;
        }
    }
  else if (argc < 2 || argc > 3)
    {
      printf("[BlindBadge] error: accel_read [samples].\n");
      return 1;
    }
  else if (argc == 3)
    {
      errno = 0;
      samples = strtol(argv[2], &end, 10);
      if (errno != 0 || *end != '\0' || samples < 1 || samples > 100)
        {
          printf("[BlindBadge] error: samples must be 1..100.\n");
          return 1;
        }
    }

  ret = qma7981_init(&dev, "/dev/i2c0");
  if (ret < 0)
    {
      printf("[BlindBadge] error: QMA7981 init failed (%d)\n", ret);
      return 1;
    }

  for (i = 0; i < samples; i++)
    {
      ret = qma7981_read_sample(&dev, &sample);
      if (ret < 0)
        {
          printf("[BlindBadge] error: sample %ld read failed (%d)\n",
                 i + 1, ret);
          (void)qma7981_set_standby(&dev);
          qma7981_close(&dev);
          return 1;
        }

      printf("[BlindBadge] accel[%ld/%ld]: "
             "x=%+.3fg y=%+.3fg z=%+.3fg |a|=%.3fg\n",
             i + 1, samples,
             (double)sample.x, (double)sample.y,
             (double)sample.z, (double)sample.magnitude);

      if (i + 1 < samples)
        {
          usleep(50000);
        }
    }

  (void)qma7981_set_standby(&dev);
  qma7981_close(&dev);
  return 0;
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

  if (strcmp(argv[1], "state") == 0)
    {
      return handle_state_command(argc, argv);
    }

  if (strcmp(argv[1], "button_service") == 0)
    {
      return handle_button_service(argc);
    }

  if (strcmp(argv[1], "button_stop") == 0)
    {
      g_button_service_running = 0;
      printf("[BlindBadge] button_service: stop requested\n");
      return 0;
    }

  if (strcmp(argv[1], "button_status") == 0)
    {
      printf("[BlindBadge] button_service: %s\n",
             g_button_service_running ? "RUNNING" : "STOPPED");
      printf("[BlindBadge] state.current: %s\n",
             blind_badge_state_name(blind_badge_state_get()));
      return 0;
    }

  if (strcmp(argv[1], "menu_show") == 0)
    {
      return blind_badge_menu_show() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "menu_next") == 0)
    {
      return blind_badge_menu_next() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "menu_status") == 0)
    {
      printf("[BlindBadge] menu.selected=%d page=%s\n",
             (int)blind_badge_menu_selected(),
             blind_badge_menu_is_service_page() ? "service" : "list");
      return 0;
    }

  if (strcmp(argv[1], "play") == 0)
    {
      handle_play_short_press();
      return 0;
    }

  if (strcmp(argv[1], "back") == 0)
    {
      handle_play_double_press();
      return 0;
    }

  if (strcmp(argv[1], "service_status") == 0)
    {
      char detail[64];
      enum blind_badge_menu_id id = blind_badge_service_active();

      blind_badge_service_detail(detail, sizeof(detail));
      printf("[BlindBadge] service: %s state=%s detail=%s\n",
             blind_badge_service_label(id),
             blind_badge_service_state_name(blind_badge_service_state()),
             detail);
      return 0;
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

  if (strcmp(argv[1], "voice_asr") == 0)
    {
      return handle_voice_asr(argc, argv, &options);
    }

  if (strcmp(argv[1], "voice_ai") == 0)
    {
      return handle_voice_ai(argc, argv, &options);
    }

  if (strcmp(argv[1], "uart_link_start") == 0)
    {
      UART_LINK_SetEventHandler(handle_uart_link_event, NULL);
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

  if (strcmp(argv[1], "uart_link_send") == 0)
    {
      if (argc != 3)
        {
          printf("[BlindBadge] error: uart_link_send needs one flag.\n");
          return 1;
        }

      if (UART_LINK_SendFlag(argv[2]) != 0)
        {
          printf("[BlindBadge] uart_link_send: failed or disconnected.\n");
          return 1;
        }
      printf("[BlindBadge] uart_link_send: queued flag=%s\n", argv[2]);
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

  if (strcmp(argv[1], "fall_start") == 0)
    {
      return handle_fall_start(argc, argv, &options);
    }

  if (strcmp(argv[1], "fall_stop") == 0)
    {
      return handle_fall_stop(argc);
    }

  if (strcmp(argv[1], "fall_status") == 0)
    {
      return handle_fall_status(argc);
    }

  if (strcmp(argv[1], "fall_read") == 0)
    {
      return handle_accel_read(argc, argv, true);
    }

  if (strcmp(argv[1], "accel_read") == 0)
    {
      return handle_accel_read(argc, argv, false);
    }

  if (strcmp(argv[1], "vision_start") == 0 ||
      strcmp(argv[1], "vision_once") == 0)
    {
      long port = BLIND_BADGE_VISION_DEFAULT_PORT;
      char *end;
      int ret;

      if (argc < 3 || argc > 4)
        {
          printf("[BlindBadge] error: %s <server_ipv4> [port]\n",
                 argv[1]);
          return 1;
        }

      if (argc == 4)
        {
          errno = 0;
          port = strtol(argv[3], &end, 10);
          if (errno != 0 || end == argv[3] || *end != '\0' ||
              port <= 0 || port > 65535)
            {
              printf("[BlindBadge] error: invalid vision port.\n");
              return 1;
            }
        }

      if (strcmp(argv[1], "vision_start") == 0)
        {
          ret = blind_badge_vision_start(argv[2], (int)port);
        }
      else
        {
          ret = blind_badge_vision_once(argv[2], (int)port);
        }

      if (ret < 0)
        {
          printf("[BlindBadge] %s failed (%d).\n", argv[1], ret);
          return 1;
        }

      return 0;
    }

  if (strcmp(argv[1], "vision_stop") == 0)
    {
      return blind_badge_vision_stop() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "vision_status") == 0)
    {
      return blind_badge_vision_status() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "vision_server") == 0)
    {
      long port = BLIND_BADGE_VISION_HTTP_DEFAULT_PORT;
      char *end;

      if (argc > 3)
        {
          printf("[BlindBadge] error: vision_server [port]\n");
          return 1;
        }

      if (argc == 3)
        {
          errno = 0;
          port = strtol(argv[2], &end, 10);
          if (errno != 0 || end == argv[2] || *end != '\0' ||
              port <= 0 || port > 65535)
            {
              printf("[BlindBadge] error: invalid HTTP port.\n");
              return 1;
            }
        }

      return blind_badge_vision_server_run((int)port) == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "vision_server_stop") == 0)
    {
      return blind_badge_vision_server_stop() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "vision_server_status") == 0)
    {
      return blind_badge_vision_server_status() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "vision_camera_test") == 0)
    {
      return blind_badge_vision_camera_test() == 0 ? 0 : 1;
    }

  if (strcmp(argv[1], "vision_camera_bench") == 0)
    {
      long frames = 30;
      char *end;

      if (argc > 3)
        {
          printf("[BlindBadge] error: vision_camera_bench [frames]\n");
          return 1;
        }

      if (argc == 3)
        {
          errno = 0;
          frames = strtol(argv[2], &end, 10);
          if (errno != 0 || end == argv[2] || *end != '\0' ||
              frames < 1 || frames > 300)
            {
              printf("[BlindBadge] error: frames must be 1..300.\n");
              return 1;
            }
        }

      return blind_badge_vision_camera_bench((unsigned int)frames) == 0 ?
             0 : 1;
    }

  printf("[BlindBadge] error: unknown command: %s\n", argv[1]);
  print_help();
  return 1;
}
