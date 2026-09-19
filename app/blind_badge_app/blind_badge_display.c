/****************************************************************************
 * Contest 2026 team 024 - BlindBadge display output
 ****************************************************************************/

#include "blind_badge_display.h"

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
#  include "infra/network_manager.h"
#endif

#ifdef CONFIG_VIDEO_FB
#  include <nuttx/video/fb.h>
#endif

#define BLIND_BADGE_FB_PATH "/dev/fb0"
#define BLIND_BADGE_FONT_W 5
#define BLIND_BADGE_FONT_H 7
#define BLIND_BADGE_TEXT_SCALE 2
#define BLIND_BADGE_CJK_W 16
#define BLIND_BADGE_CJK_H 16
#define BLIND_BADGE_CJK_ADVANCE 17
#define BLIND_BADGE_LINE_SIZE 64

#ifdef CONFIG_VIDEO_FB
struct blind_badge_fb
{
  int fd;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  uint8_t *mem;
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((uint16_t)(r & 0xf8) << 8) |
         ((uint16_t)(g & 0xfc) << 3) |
         ((uint16_t)b >> 3);
}

static uint32_t rgb888(uint8_t r, uint8_t g, uint8_t b)
{
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static const uint8_t *font5x7(char ch)
{
  static const uint8_t blank[BLIND_BADGE_FONT_H] =
    {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
  static const uint8_t question[BLIND_BADGE_FONT_H] =
    {
      0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04
    };

  switch (ch)
    {
      case ' ':
        return blank;
      case '!':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
          return v;
        }
      case '-':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
          return v;
        }
      case '/':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
          return v;
        }
      case '0':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
          return v;
        }
      case '1':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
          return v;
        }
      case '2':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
          return v;
        }
      case '3':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
          return v;
        }
      case '4':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
          return v;
        }
      case '5':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e};
          return v;
        }
      case '6':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e};
          return v;
        }
      case '7':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
          return v;
        }
      case '8':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
          return v;
        }
      case '9':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c};
          return v;
        }
      case 'A':
      case 'a':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
          return v;
        }
      case 'B':
      case 'b':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e};
          return v;
        }
      case 'C':
      case 'c':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
          return v;
        }
      case 'D':
      case 'd':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
          return v;
        }
      case 'E':
      case 'e':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
          return v;
        }
      case 'F':
      case 'f':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
          return v;
        }
      case 'G':
      case 'g':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f};
          return v;
        }
      case 'H':
      case 'h':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
          return v;
        }
      case 'I':
      case 'i':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
          return v;
        }
      case 'J':
      case 'j':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e};
          return v;
        }
      case 'K':
      case 'k':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
          return v;
        }
      case 'L':
      case 'l':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
          return v;
        }
      case 'M':
      case 'm':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11};
          return v;
        }
      case 'N':
      case 'n':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
          return v;
        }
      case 'O':
      case 'o':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
          return v;
        }
      case 'P':
      case 'p':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
          return v;
        }
      case 'Q':
      case 'q':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d};
          return v;
        }
      case 'R':
      case 'r':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
          return v;
        }
      case 'S':
      case 's':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
          return v;
        }
      case 'T':
      case 't':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
          return v;
        }
      case 'U':
      case 'u':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
          return v;
        }
      case 'V':
      case 'v':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04};
          return v;
        }
      case 'W':
      case 'w':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a};
          return v;
        }
      case 'X':
      case 'x':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11};
          return v;
        }
      case 'Y':
      case 'y':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
          return v;
        }
      case 'Z':
      case 'z':
        {
          static const uint8_t v[BLIND_BADGE_FONT_H] =
            {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f};
          return v;
        }
      default:
        return question;
    }
}

struct blind_badge_cjk_glyph
{
  uint32_t codepoint;
  uint16_t rows[BLIND_BADGE_CJK_H];
};

static const struct blind_badge_cjk_glyph g_cjk_glyphs[] =
{
  {0x524d, {0x0000, 0x0810, 0x0810, 0x0430, 0x7ffe, 0x0000, 0x0004, 0x3f24, 0x2124, 0x3f24, 0x2124, 0x2124, 0x3f24, 0x2124, 0x2104, 0x2104}}, /* 前 */
  {0x65b9, {0x0000, 0x0180, 0x0180, 0x0180, 0x7ffe, 0x0200, 0x0200, 0x0200, 0x03f8, 0x0608, 0x0408, 0x0408, 0x0c08, 0x0808, 0x1818, 0x3010}}, /* 方 */
  {0x969c, {0x0000, 0x0020, 0x7ffe, 0x4c88, 0x4888, 0x4bfe, 0x5000, 0x49fc, 0x4904, 0x45fc, 0x4504, 0x45fc, 0x5820, 0x47fe, 0x4020, 0x4020}}, /* 障 */
  {0x788d, {0x0000, 0x0000, 0x7efc, 0x1084, 0x10fc, 0x3084, 0x20fc, 0x3c00, 0x6400, 0x65fe, 0x6408, 0x25fe, 0x2408, 0x24c8, 0x3c48, 0x2008}}, /* 碍 */
  {0x8bf7, {0x0000, 0x0020, 0x2020, 0x33fe, 0x1820, 0x03fc, 0x0020, 0x73fe, 0x1000, 0x11fc, 0x1104, 0x11fc, 0x1104, 0x1dfc, 0x1904, 0x1104}}, /* 请 */
  {0x5148, {0x0000, 0x0180, 0x0980, 0x1980, 0x1ffc, 0x1180, 0x2180, 0x2180, 0x0180, 0x7ffe, 0x0460, 0x0460, 0x0460, 0x0862, 0x1862, 0x703e}}, /* 先 */
  {0x505c, {0x0000, 0x0860, 0x0860, 0x1ffe, 0x1000, 0x33fc, 0x7204, 0x53fc, 0x1000, 0x17fe, 0x1402, 0x15fe, 0x1060, 0x1060, 0x1060, 0x1060}}, /* 停 */
  {0x4e0b, {0x0000, 0x0000, 0x0000, 0x0100, 0x0100, 0x0100, 0x01c0, 0x0170, 0x0118, 0x010c, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100}}, /* 下 */
  {0x786e, {0x0000, 0x0040, 0x7e40, 0x10f8, 0x1098, 0x3190, 0x21fe, 0x3d92, 0x25fe, 0x6592, 0x6592, 0x6592, 0x25fe, 0x3d02, 0x2102, 0x2302}}, /* 确 */
  {0x8ba4, {0x0000, 0x0020, 0x3020, 0x1820, 0x0820, 0x0020, 0x0020, 0x7820, 0x1820, 0x1830, 0x1870, 0x1850, 0x1a58, 0x1ec8, 0x198c, 0x1106}}, /* 认 */
  {0x6211, {0x0000, 0x0040, 0x1f58, 0x7c4c, 0x0c44, 0x0c40, 0x7ffe, 0x0c60, 0x0c64, 0x0c2c, 0x0f28, 0x7c38, 0x0c30, 0x0c72, 0x0cd2, 0x0f9a}}, /* 我 */
  {0x9700, {0x0000, 0x0000, 0x3ffc, 0x0180, 0x7ffe, 0x4182, 0x5ffa, 0x0180, 0x1c38, 0x7ffe, 0x0100, 0x0100, 0x3ffc, 0x2244, 0x2244, 0x2244}}, /* 需 */
  {0x8981, {0x0000, 0x0000, 0x0000, 0x7ffe, 0x0640, 0x0640, 0x3ffc, 0x2644, 0x2644, 0x3ffc, 0x0200, 0x7ffe, 0x0c10, 0x0820, 0x1f60, 0x01e0}}, /* 要 */
  {0x5e2e, {0x0000, 0x0c00, 0x0c7e, 0x7fc4, 0x0c4c, 0x7f48, 0x0c44, 0x7fc2, 0x185c, 0x31c0, 0x2180, 0x3ffc, 0x3184, 0x3184, 0x3184, 0x31bc}}, /* 帮 */
  {0x52a9, {0x0000, 0x0020, 0x3e20, 0x2220, 0x2220, 0x23fe, 0x3e22, 0x2222, 0x2222, 0x3e22, 0x2262, 0x2246, 0x2346, 0x3fc6, 0x7084, 0x0104}}, /* 助 */
  {0x8054, {0x0000, 0x7e84, 0x2448, 0x2458, 0x25fe, 0x3c20, 0x2420, 0x2420, 0x3dfe, 0x2420, 0x2430, 0x2670, 0x7c58, 0x04cc, 0x0586, 0x0502}}, /* 联 */
  {0x7cfb, {0x0000, 0x00fc, 0x3fc0, 0x0200, 0x0420, 0x3840, 0x0c80, 0x0710, 0x0608, 0x7ffc, 0x0182, 0x0990, 0x1998, 0x318c, 0x6186, 0x0180}}, /* 系 */
  {0x6216, {0x0000, 0x0040, 0x004c, 0x0044, 0x7ffe, 0x0040, 0x0040, 0x3f44, 0x236c, 0x2368, 0x2328, 0x3f30, 0x0030, 0x0372, 0x7ed2, 0x619a}}, /* 或 */
  {0x5f80, {0x0000, 0x0800, 0x18e0, 0x3030, 0x6010, 0x4bfe, 0x0820, 0x1020, 0x3020, 0x7020, 0x13fc, 0x1020, 0x1020, 0x1020, 0x1020, 0x17fe}}, /* 往 */
  {0x7684, {0x0000, 0x0000, 0x0860, 0x0840, 0x1040, 0x7e7e, 0x6282, 0x6282, 0x6382, 0x6242, 0x7e66, 0x6236, 0x6214, 0x6204, 0x6204, 0x7e04}}, /* 的 */
  {0x5f53, {0x0000, 0x0180, 0x2184, 0x1188, 0x1988, 0x0990, 0x0180, 0x3ffc, 0x0004, 0x0004, 0x0004, 0x1ffc, 0x0004, 0x0004, 0x0004, 0x3ffc}}, /* 当 */
  {0x4f4d, {0x0000, 0x0800, 0x0860, 0x0860, 0x1060, 0x37fe, 0x3000, 0x710c, 0x5108, 0x1108, 0x1108, 0x1198, 0x1090, 0x1090, 0x1010, 0x17fe}}, /* 位 */
  {0x7f6e, {0x0000, 0x0000, 0x3ffc, 0x2244, 0x3ffc, 0x0080, 0x7ffe, 0x0100, 0x27fc, 0x240c, 0x27fc, 0x27fc, 0x240c, 0x27fc, 0x2000, 0x3ffe}}, /* 置 */
  {0x5398, {0x0000, 0x0000, 0x0000, 0x3ffe, 0x2000, 0x2000, 0x27fc, 0x2444, 0x27fc, 0x2444, 0x2444, 0x27fc, 0x2040, 0x2ffc, 0x2040, 0x4040}}, /* 厘 */
  {0x7c73, {0x0000, 0x0180, 0x2184, 0x318c, 0x1188, 0x0998, 0x0990, 0x0180, 0x7ffe, 0x03c0, 0x07e0, 0x05a0, 0x0990, 0x118c, 0x6186, 0x4182}}, /* 米 */
  {0x6709, {0x0000, 0x0200, 0x0200, 0x0200, 0x7ffe, 0x0400, 0x0c00, 0x1ff8, 0x3808, 0x6ff8, 0x4808, 0x0808, 0x0ff8, 0x0808, 0x0808, 0x0808}}, /* 有 */
  {0x7acb, {0x0000, 0x0000, 0x0180, 0x0180, 0x0180, 0x0180, 0x7ffe, 0x0000, 0x0810, 0x0810, 0x0c10, 0x0430, 0x0420, 0x0420, 0x0460, 0x0040}}, /* 立 */
  {0x5373, {0x0000, 0x0000, 0x3f7e, 0x2146, 0x2146, 0x2146, 0x3f46, 0x2146, 0x2146, 0x3f46, 0x2046, 0x2446, 0x2246, 0x2e4c, 0x7940, 0x4140}}, /* 即 */
  {0x51cf, {0x0000, 0x0010, 0x0016, 0x6012, 0x37fe, 0x1410, 0x0410, 0x07f2, 0x0414, 0x15d4, 0x255c, 0x2558, 0x65d8, 0x4918, 0x482a, 0x184a}}, /* 减 */
  {0x901f, {0x0000, 0x0000, 0x0060, 0x6060, 0x37fe, 0x1860, 0x0060, 0x03fc, 0x0264, 0x7264, 0x13fc, 0x10f0, 0x11f8, 0x136c, 0x1666, 0x2c60}}, /* 速 */
  {0x5e76, {0x0000, 0x0810, 0x0810, 0x0430, 0x0420, 0x7ffe, 0x0420, 0x0420, 0x0420, 0x0420, 0x7ffe, 0x0c20, 0x0820, 0x0820, 0x1820, 0x3020}}, /* 并 */
  {0x7ed5, {0x0000, 0x0000, 0x1080, 0x1080, 0x107e, 0x33e0, 0x2044, 0x6868, 0x7832, 0x51d2, 0x100e, 0x2000, 0x7bfe, 0x4090, 0x0090, 0x7992}}, /* 绕 */
  {0x884c, {0x0000, 0x0800, 0x19fe, 0x3000, 0x6000, 0x4c00, 0x0800, 0x1bfe, 0x3008, 0x7008, 0x5008, 0x1008, 0x1008, 0x1008, 0x1008, 0x1008}}, /* 行 */
  {0x68c0, {0x0000, 0x1020, 0x1060, 0x1060, 0x1090, 0x7d88, 0x1304, 0x37fa, 0x3800, 0x3444, 0x5124, 0x5124, 0x5128, 0x1108, 0x1010, 0x17fe}}, /* 检 */
  {0x6d4b, {0x0000, 0x0002, 0x37c2, 0x144a, 0x054a, 0x054a, 0x454a, 0x354a, 0x054a, 0x054a, 0x054a, 0x154a, 0x354a, 0x2102, 0x2282, 0x6642}}, /* 测 */
  {0x5230, {0x0000, 0x7f86, 0x1826, 0x1226, 0x1326, 0x3126, 0x7fa6, 0x0ca6, 0x0c26, 0x7fa6, 0x0c26, 0x0c06, 0x0c06, 0x0f86, 0x7f06, 0x001c}}, /* 到 */
  {0x8f83, {0x0000, 0x1020, 0x1030, 0x7c10, 0x31fe, 0x2000, 0x284c, 0x2884, 0x6882, 0x7d4a, 0x0848, 0x0878, 0x1e30, 0x7830, 0x0878, 0x08c6}}, /* 较 */
  {0x8fdc, {0x0000, 0x0000, 0x63fc, 0x3000, 0x1000, 0x0000, 0x07fe, 0x7090, 0x1090, 0x1190, 0x1110, 0x1112, 0x1312, 0x161e, 0x3800, 0x47fe}}, /* 远 */
  {0x4fdd, {0x0000, 0x0800, 0x0bfc, 0x1a04, 0x1204, 0x3204, 0x33fc, 0x5020, 0x5020, 0x1020, 0x17fe, 0x1070, 0x10b8, 0x1128, 0x1226, 0x1422}}, /* 保 */
  {0x6301, {0x0000, 0x1020, 0x1020, 0x1020, 0x11fe, 0x7c20, 0x1020, 0x13fe, 0x1008, 0x1c08, 0x73fe, 0x1008, 0x1188, 0x1088, 0x1048, 0x1008}}, /* 持 */
  {0x6ce8, {0x0000, 0x0000, 0x2080, 0x30c0, 0x0870, 0x0020, 0x07fe, 0x6060, 0x3060, 0x0060, 0x0060, 0x03fc, 0x0860, 0x1060, 0x1060, 0x2060}}, /* 注 */
  {0x610f, {0x0000, 0x0180, 0x3ffc, 0x0c30, 0x0420, 0x7ffe, 0x0000, 0x1ff8, 0x1008, 0x1ff8, 0x1008, 0x1ff8, 0x0300, 0x1488, 0x3404, 0x6436}}, /* 意 */
  {0x53ef, {0x0000, 0x0000, 0x7ffe, 0x0008, 0x0008, 0x1f88, 0x1088, 0x1088, 0x1088, 0x1088, 0x1f88, 0x1008, 0x1008, 0x0008, 0x0008, 0x0078}}, /* 可 */
  {0x80fd, {0x0000, 0x1040, 0x1040, 0x124c, 0x2278, 0x7f40, 0x0142, 0x3e42, 0x227e, 0x2240, 0x3e40, 0x224c, 0x3e70, 0x2240, 0x2242, 0x2242}}, /* 能 */
  {0x53f0, {0x0000, 0x0200, 0x0200, 0x0600, 0x0430, 0x0808, 0x0ffc, 0x7e06, 0x0002, 0x0000, 0x1ff8, 0x1008, 0x1008, 0x1008, 0x1008, 0x1ff8}}, /* 台 */
  {0x9636, {0x0000, 0x0020, 0x7c20, 0x4470, 0x48d0, 0x4888, 0x5906, 0x4b02, 0x4c88, 0x4488, 0x4488, 0x4488, 0x5c88, 0x4088, 0x4188, 0x4308}}, /* 阶 */
  {0x4e00, {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x7ffe, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}}, /* 一 */
  {0x7528, {0x0000, 0x0000, 0x3ffc, 0x3184, 0x3184, 0x3ffc, 0x3184, 0x3184, 0x3184, 0x3ffc, 0x2184, 0x2184, 0x2184, 0x2184, 0x4184, 0x41bc}}, /* 用 */
  {0x624b, {0x0000, 0x0008, 0x01fc, 0x3f80, 0x0180, 0x0180, 0x3ffc, 0x0180, 0x0180, 0x0180, 0x7ffe, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180}}, /* 手 */
  {0x6756, {0x0000, 0x1018, 0x1018, 0x1018, 0x1018, 0x7dfe, 0x1010, 0x1910, 0x3490, 0x3690, 0x30d0, 0x5070, 0x5060, 0x1070, 0x10d8, 0x118e}}, /* 杖 */
  {0x811a, {0x0000, 0x0100, 0x791e, 0x4912, 0x4bd2, 0x4912, 0x7912, 0x4912, 0x4fd2, 0x4912, 0x7912, 0x4b52, 0x4a52, 0x4ad6, 0x4ff0, 0x4810}}, /* 脚 */
  {0x5c16, {0x0000, 0x0180, 0x0180, 0x0998, 0x118c, 0x3184, 0x6182, 0x0700, 0x0000, 0x0100, 0x7ffe, 0x0180, 0x02c0, 0x0660, 0x0c30, 0x781e}}, /* 尖 */
  {0x5c0f, {0x0000, 0x0180, 0x0180, 0x0180, 0x0180, 0x1188, 0x1188, 0x1184, 0x3184, 0x2186, 0x6182, 0x4182, 0x0182, 0x0180, 0x0180, 0x0700}}, /* 小 */
  {0x5fc3, {0x0000, 0x0000, 0x0000, 0x0380, 0x00c0, 0x0020, 0x0000, 0x0400, 0x2408, 0x240c, 0x2404, 0x2406, 0x6402, 0x4413, 0x4410, 0x0410}}, /* 心 */
  {0x7d27, {0x0000, 0x25fc, 0x248c, 0x2458, 0x2430, 0x25d8, 0x0706, 0x0c60, 0x1fc0, 0x0318, 0x0e08, 0x3ffc, 0x0080, 0x08b0, 0x308c, 0x2384}}, /* 紧 */
  {0x6025, {0x0000, 0x0000, 0x0400, 0x0c00, 0x0fe0, 0x1040, 0x3ff8, 0x4008, 0x0008, 0x1ff8, 0x0008, 0x3ff8, 0x0300, 0x14d8, 0x3424, 0x6426}}, /* 急 */
  {0x60c5, {0x0000, 0x0000, 0x3060, 0x3060, 0x37fe, 0x3860, 0x7bfc, 0x7860, 0x77fe, 0x7000, 0x33fc, 0x3204, 0x33fc, 0x3204, 0x33fc, 0x3204}}, /* 情 */
  {0x51b5, {0x0000, 0x0000, 0x0000, 0x03fc, 0x3204, 0x1204, 0x0204, 0x0204, 0x0204, 0x03fc, 0x1890, 0x1090, 0x2190, 0x6110, 0x4112, 0x0212}}, /* 况 */
  {0x9047, {0x0000, 0x0000, 0x63fc, 0x3244, 0x13fc, 0x0244, 0x03fc, 0x0040, 0x77fe, 0x1446, 0x1456, 0x15fe, 0x140e, 0x1406, 0x280c, 0x47fe}}, /* 遇 */
  {0x4e86, {0x0000, 0x0000, 0x3ffc, 0x0018, 0x0030, 0x0060, 0x01c0, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0f00}}, /* 了 */
  {0x89e6, {0x0000, 0x1010, 0x1010, 0x3c10, 0x2410, 0x2cfe, 0x7e92, 0x2a92, 0x3e92, 0x2a92, 0x2afe, 0x3e10, 0x2214, 0x4214, 0x4216, 0x42fe}}, /* 触 */
  {0x53d1, {0x0000, 0x0180, 0x1918, 0x1108, 0x1100, 0x3ffe, 0x0300, 0x0200, 0x07f8, 0x0608, 0x0f18, 0x0910, 0x18a0, 0x30c0, 0x61b0, 0x071e}}, /* 发 */
  {0x6c42, {0x0000, 0x0180, 0x0198, 0x0188, 0x7ffe, 0x0180, 0x2184, 0x118c, 0x19d8, 0x09f0, 0x01a0, 0x05b0, 0x0d98, 0x318c, 0x6186, 0x0182}}, /* 求 */
  {0x5efa, {0x0000, 0x7860, 0x13fc, 0x1064, 0x37fe, 0x2064, 0x7bfc, 0x4860, 0x0bfc, 0x6860, 0x2860, 0x37fe, 0x1060, 0x2c00, 0x63fe, 0x4000}}, /* 建 */
  {0x8bae, {0x0000, 0x2044, 0x1264, 0x1a24, 0x0324, 0x010c, 0x7108, 0x1188, 0x1098, 0x1090, 0x1070, 0x1060, 0x1470, 0x1998, 0x330e, 0x0602}}, /* 议 */
  {0x9001, {0x0000, 0x0000, 0x0000, 0x6108, 0x3108, 0x1890, 0x03fe, 0x0060, 0x0060, 0x77fe, 0x1060, 0x1070, 0x1090, 0x118c, 0x1606, 0x3c00}}, /* 送 */
  {0x3002, {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x3000, 0x4800, 0x4400, 0x4400, 0x4800}}, /* 。 */
  {0xff01, {0x0000, 0x0000, 0x0000, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x0000, 0x0000, 0x0180, 0x0180, 0x0000}}, /* ！ */
  {0xff0c, {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1800, 0x1800, 0x0800, 0x0800, 0x3000}}, /* ， */
};

static const struct blind_badge_cjk_glyph *find_cjk_glyph(uint32_t codepoint)
{
  size_t i;

  for (i = 0; i < sizeof(g_cjk_glyphs) / sizeof(g_cjk_glyphs[0]); i++)
    {
      if (g_cjk_glyphs[i].codepoint == codepoint)
        {
          return &g_cjk_glyphs[i];
        }
    }

  return NULL;
}

static void put_pixel(struct blind_badge_fb *fb, int x, int y,
                      uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t *row;
  uint32_t color32;
  uint16_t color16;

  if (x < 0 || y < 0 || x >= fb->vinfo.xres || y >= fb->vinfo.yres)
    {
      return;
    }

  row = fb->mem + y * fb->pinfo.stride;
  switch (fb->pinfo.bpp)
    {
      case 16:
        color16 = rgb565(r, g, b);
        ((uint16_t *)row)[x] = color16;
        break;
      case 24:
        row[x * 3 + 0] = b;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = r;
        break;
      case 32:
        color32 = rgb888(r, g, b);
        ((uint32_t *)row)[x] = color32;
        break;
      default:
        break;
    }
}

static void fill_rect(struct blind_badge_fb *fb, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
  int px;
  int py;

  for (py = y; py < y + h; py++)
    {
      for (px = x; px < x + w; px++)
        {
          put_pixel(fb, px, py, r, g, b);
        }
    }
}

static void draw_char(struct blind_badge_fb *fb, int x, int y, char ch,
                      uint8_t r, uint8_t g, uint8_t b)
{
  const uint8_t *glyph = font5x7(ch);
  int row;
  int col;
  int sx;
  int sy;

  for (row = 0; row < BLIND_BADGE_FONT_H; row++)
    {
      for (col = 0; col < BLIND_BADGE_FONT_W; col++)
        {
          if ((glyph[row] & (1 << (BLIND_BADGE_FONT_W - 1 - col))) != 0)
            {
              for (sy = 0; sy < BLIND_BADGE_TEXT_SCALE; sy++)
                {
                  for (sx = 0; sx < BLIND_BADGE_TEXT_SCALE; sx++)
                    {
                      put_pixel(fb,
                                x + col * BLIND_BADGE_TEXT_SCALE + sx,
                                y + row * BLIND_BADGE_TEXT_SCALE + sy,
                                r, g, b);
                    }
                }
            }
        }
    }
}

static void draw_cjk_char(struct blind_badge_fb *fb, int x, int y,
                          const struct blind_badge_cjk_glyph *glyph,
                          uint8_t r, uint8_t g, uint8_t b)
{
  int row;
  int col;

  for (row = 0; row < BLIND_BADGE_CJK_H; row++)
    {
      for (col = 0; col < BLIND_BADGE_CJK_W; col++)
        {
          if ((glyph->rows[row] & (1 << (BLIND_BADGE_CJK_W - 1 - col))) != 0)
            {
              put_pixel(fb, x + col, y + row, r, g, b);
            }
        }
    }
}

static int utf8_decode_one(const char *text, uint32_t *codepoint,
                           size_t *bytes)
{
  unsigned char c0;

  if (text == NULL || text[0] == '\0')
    {
      return 0;
    }

  c0 = (unsigned char)text[0];
  if ((c0 & 0x80) == 0)
    {
      *codepoint = c0;
      *bytes = 1;
      return 1;
    }

  if ((c0 & 0xe0) == 0xc0 &&
      ((unsigned char)text[1] & 0xc0) == 0x80)
    {
      *codepoint = ((uint32_t)(c0 & 0x1f) << 6) |
                   ((uint32_t)text[1] & 0x3f);
      *bytes = 2;
      return 1;
    }

  if ((c0 & 0xf0) == 0xe0 &&
      ((unsigned char)text[1] & 0xc0) == 0x80 &&
      ((unsigned char)text[2] & 0xc0) == 0x80)
    {
      *codepoint = ((uint32_t)(c0 & 0x0f) << 12) |
                   (((uint32_t)text[1] & 0x3f) << 6) |
                   ((uint32_t)text[2] & 0x3f);
      *bytes = 3;
      return 1;
    }

  *codepoint = '?';
  *bytes = 1;
  return 1;
}

static int draw_text(struct blind_badge_fb *fb, int x, int y,
                     const char *text, uint8_t r, uint8_t g, uint8_t b)
{
  int cursor = x;
  int missing_cjk = 0;

  while (text != NULL && *text != '\0')
    {
      const struct blind_badge_cjk_glyph *glyph;
      uint32_t codepoint;
      size_t bytes;

      if (cursor + BLIND_BADGE_FONT_W * BLIND_BADGE_TEXT_SCALE >=
          fb->vinfo.xres)
        {
          break;
        }

      if (!utf8_decode_one(text, &codepoint, &bytes))
        {
          break;
        }

      glyph = find_cjk_glyph(codepoint);
      if (glyph != NULL)
        {
          if (cursor + BLIND_BADGE_CJK_W >= fb->vinfo.xres)
            {
              break;
            }

          draw_cjk_char(fb, cursor, y, glyph, r, g, b);
          cursor += BLIND_BADGE_CJK_ADVANCE;
        }
      else
        {
          if (codepoint > 0x7f)
            {
              missing_cjk = 1;
              codepoint = '?';
            }

          draw_char(fb, cursor, y, (char)codepoint, r, g, b);
          cursor += (BLIND_BADGE_FONT_W + 1) * BLIND_BADGE_TEXT_SCALE;
        }

      text += bytes;
    }

  return missing_cjk;
}

static void update_screen(struct blind_badge_fb *fb)
{
  struct fb_area_s area;

  area.x = 0;
  area.y = 0;
  area.w = fb->vinfo.xres;
  area.h = fb->vinfo.yres;
  ioctl(fb->fd, FBIO_UPDATE, (unsigned long)((uintptr_t)&area));
}

static int fb_open(struct blind_badge_fb *fb)
{
  int ret;

  memset(fb, 0, sizeof(*fb));
  fb->fd = open(BLIND_BADGE_FB_PATH, O_RDWR);
  if (fb->fd < 0)
    {
      printf("[BlindBadge] lcd: open %s failed: %d\n",
             BLIND_BADGE_FB_PATH, errno);
      return -errno;
    }

  ret = ioctl(fb->fd, FBIOGET_VIDEOINFO,
              (unsigned long)((uintptr_t)&fb->vinfo));
  if (ret < 0)
    {
      printf("[BlindBadge] lcd: FBIOGET_VIDEOINFO failed: %d\n", errno);
      close(fb->fd);
      return -errno;
    }

  ret = ioctl(fb->fd, FBIOGET_PLANEINFO,
              (unsigned long)((uintptr_t)&fb->pinfo));
  if (ret < 0)
    {
      printf("[BlindBadge] lcd: FBIOGET_PLANEINFO failed: %d\n", errno);
      close(fb->fd);
      return -errno;
    }

  if (fb->pinfo.bpp != 16 && fb->pinfo.bpp != 24 && fb->pinfo.bpp != 32)
    {
      printf("[BlindBadge] lcd: unsupported bpp=%u\n", fb->pinfo.bpp);
      close(fb->fd);
      return -ENOTSUP;
    }

  fb->mem = mmap(NULL, fb->pinfo.fblen, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FILE, fb->fd, 0);
  if (fb->mem == MAP_FAILED)
    {
      printf("[BlindBadge] lcd: mmap failed: %d\n", errno);
      close(fb->fd);
      return -errno;
    }

  return 0;
}

static void fb_close(struct blind_badge_fb *fb)
{
  munmap(fb->mem, fb->pinfo.fblen);
  close(fb->fd);
}
#else
static int utf8_decode_one(const char *text, uint32_t *codepoint,
                           size_t *bytes)
{
  unsigned char c0;

  if (text == NULL || text[0] == '\0')
    {
      return 0;
    }

  c0 = (unsigned char)text[0];
  if ((c0 & 0x80) == 0)
    {
      *codepoint = c0;
      *bytes = 1;
      return 1;
    }

  if ((c0 & 0xe0) == 0xc0 &&
      ((unsigned char)text[1] & 0xc0) == 0x80)
    {
      *codepoint = ((uint32_t)(c0 & 0x1f) << 6) |
                   ((uint32_t)text[1] & 0x3f);
      *bytes = 2;
      return 1;
    }

  if ((c0 & 0xf0) == 0xe0 &&
      ((unsigned char)text[1] & 0xc0) == 0x80 &&
      ((unsigned char)text[2] & 0xc0) == 0x80)
    {
      *codepoint = ((uint32_t)(c0 & 0x0f) << 12) |
                   (((uint32_t)text[1] & 0x3f) << 6) |
                   ((uint32_t)text[2] & 0x3f);
      *bytes = 3;
      return 1;
    }

  *codepoint = '?';
  *bytes = 1;
  return 1;
}
#endif

static int display_show_lines_with_wifi(const char *line0,
                                        const char *line1,
                                        const char *line2,
                                        const char *line3,
                                        int wifi_connected)
{
#ifdef CONFIG_VIDEO_FB
  struct blind_badge_fb fb;
  int ret;

  ret = fb_open(&fb);
  if (ret < 0)
    {
      return ret;
    }

  fill_rect(&fb, 0, 0, fb.vinfo.xres, fb.vinfo.yres, 0, 0, 0);
  if (wifi_connected > 0)
    {
      fill_rect(&fb, 0, 0, fb.vinfo.xres, 28, 0, 128, 32);
    }
  else if (wifi_connected < 0)
    {
      fill_rect(&fb, 0, 0, fb.vinfo.xres, 28, 176, 112, 0);
    }
  else
    {
      fill_rect(&fb, 0, 0, fb.vinfo.xres, 28, 176, 0, 0);
    }

  draw_text(&fb, 8, 8, wifi_connected > 0 ? "WiFi OK" :
            (wifi_connected < 0 ? "WiFi ..." : "WiFi ERROR"),
            255, 255, 255);
  draw_text(&fb, 8, 44, line0 != NULL ? line0 : "BlindBadge",
            255, 255, 255);
  draw_text(&fb, 8, 74, line1 != NULL ? line1 : "", 255, 255, 255);
  draw_text(&fb, 8, 104, line2 != NULL ? line2 : "", 255, 255, 255);
  draw_text(&fb, 8, 134, line3 != NULL ? line3 : "", 255, 255, 255);

  update_screen(&fb);
  printf("[BlindBadge] lcd: updated %ux%u bpp=%u\n",
         fb.vinfo.xres, fb.vinfo.yres, fb.pinfo.bpp);
  fb_close(&fb);
  return 0;
#else
  printf("[BlindBadge] lcd: framebuffer is not enabled in this firmware.\n");
  return -ENODEV;
#endif
}

int blind_badge_display_show_lines(const char *line0,
                                   const char *line1,
                                   const char *line2,
                                   const char *line3)
{
  int connected = 0;

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  connected = network_is_connected() ? 1 : 0;
#endif
  return display_show_lines_with_wifi(line0, line1, line2, line3,
                                      connected);
}

int blind_badge_display_show_boot_status(int connected)
{
  if (connected > 0)
    {
      return display_show_lines_with_wifi("BlindBadge", "READY",
                                          "AI online", "", 1);
    }

  if (connected < 0)
    {
      return display_show_lines_with_wifi("BlindBadge", "CONNECTING",
                                          "", "", -1);
    }

  return display_show_lines_with_wifi("BlindBadge", "LOCAL FALLBACK",
                                      "AI offline", "", 0);
}

int blind_badge_display_lcd_test(void)
{
  return blind_badge_display_show_lines("BlindBadge", "LCD OK", "", "");
}

int blind_badge_display_lcd_test_zh(void)
{
  return blind_badge_display_show_lines("BlindBadge", "前方障碍", "", "");
}

static const char *lcd_severity_name(enum blind_badge_severity severity)
{
  switch (severity)
    {
      case BLIND_BADGE_SEVERITY_INFO:
        return "INFO";
      case BLIND_BADGE_SEVERITY_WARN:
        return "WARN";
      case BLIND_BADGE_SEVERITY_URGENT:
        return "DANGER";
      case BLIND_BADGE_SEVERITY_EMERGENCY:
        return "SOS";
      default:
        return "UNKNOWN";
    }
}

static const char *lcd_default_message(const struct blind_badge_event *event)
{
  if (event == NULL)
    {
      return "";
    }

  switch (event->type)
    {
      case BLIND_BADGE_EVENT_OBSTACLE:
        if (event->distance_cm < 50)
          {
            return "stop first";
          }
        else if (event->distance_cm < 100)
          {
            return "slow and avoid";
          }
        else
          {
            return "keep attention";
          }

      case BLIND_BADGE_EVENT_STEP_DOWN:
        return "check step";

      case BLIND_BADGE_EVENT_EMERGENCY:
        return "help triggered";

      case BLIND_BADGE_EVENT_STATUS:
        return "system ready";

      default:
        return "stop and check";
    }
}

static const char *copy_lcd_utf8_cells(char *dst, size_t dst_size,
                                       const char *src, int max_cells)
{
  size_t out = 0;
  int cells = 0;

  if (dst_size == 0)
    {
      return src;
    }

  dst[0] = '\0';
  if (src == NULL)
    {
      return NULL;
    }

  while (*src != '\0' && cells < max_cells)
    {
      uint32_t codepoint;
      size_t bytes;
      int char_cells;

      if (!utf8_decode_one(src, &codepoint, &bytes))
        {
          break;
        }

      char_cells = codepoint > 0x7f ? 2 : 1;
      if (cells + char_cells > max_cells || out + bytes >= dst_size)
        {
          break;
        }

      memcpy(&dst[out], src, bytes);
      out += bytes;
      cells += char_cells;
      src += bytes;
    }

  dst[out] = '\0';
  return src;
}

void ai_agent_display_ask_result(const char *response, int success)
{
  char line2[BLIND_BADGE_LINE_SIZE];
  char line3[BLIND_BADGE_LINE_SIZE];
  const char *next;

  next = copy_lcd_utf8_cells(line2, sizeof(line2),
                             response != NULL ? response : "", 24);
  copy_lcd_utf8_cells(line3, sizeof(line3), next, 24);
  blind_badge_display_show_lines("AI CHAT",
                                 success ? "AI OK" : "AI ERROR",
                                 line2, line3);
}

int blind_badge_display_show_event(const struct blind_badge_event *event,
                                   const char *message)
{
  char line1[BLIND_BADGE_LINE_SIZE];
  char line2[BLIND_BADGE_LINE_SIZE];
  const char *safe_message;

  if (event == NULL)
    {
      return blind_badge_display_show_lines("BlindBadge", "UNKNOWN",
                                           "", "stop and check");
    }

  safe_message = (message != NULL && message[0] != '\0') ?
                 message : lcd_default_message(event);

  snprintf(line1, sizeof(line1), "%s / %s",
           lcd_severity_name(event->severity),
           blind_badge_event_name(event->type));

  if (event->type == BLIND_BADGE_EVENT_OBSTACLE)
    {
      snprintf(line2, sizeof(line2), "%s %dcm",
               blind_badge_direction_name(event->direction),
               event->distance_cm);
    }
  else if (event->type == BLIND_BADGE_EVENT_STEP_DOWN)
    {
      snprintf(line2, sizeof(line2), "%s step",
               blind_badge_direction_name(event->direction));
    }
  else
    {
      snprintf(line2, sizeof(line2), "%s",
               blind_badge_direction_name(event->direction));
    }

  return blind_badge_display_show_lines("BlindBadge", line1, line2,
                                        safe_message);
}

int blind_badge_display_show_ai_event(const struct blind_badge_event *event,
                                      const char *ai_response,
                                      int used_fallback)
{
  char line1[BLIND_BADGE_LINE_SIZE];
  char line2[BLIND_BADGE_LINE_SIZE];
  char line3[BLIND_BADGE_LINE_SIZE];
  const char *next;

  if (event == NULL)
    {
      return blind_badge_display_show_lines("BlindBadge", "AI UNKNOWN",
                                           "", "stop and check");
    }

  snprintf(line1, sizeof(line1), "%s / %s",
           lcd_severity_name(event->severity),
           used_fallback ? "AI Fallback" : "AI OK");

  if (event->type == BLIND_BADGE_EVENT_OBSTACLE)
    {
      snprintf(line2, sizeof(line2), "%s %dcm",
               blind_badge_direction_name(event->direction),
               event->distance_cm);
    }
  else if (event->type == BLIND_BADGE_EVENT_STEP_DOWN)
    {
      snprintf(line2, sizeof(line2), "%s step",
               blind_badge_direction_name(event->direction));
    }
  else
    {
      snprintf(line2, sizeof(line2), "%s",
               blind_badge_direction_name(event->direction));
    }

  next = copy_lcd_utf8_cells(line2, sizeof(line2), ai_response, 24);
  copy_lcd_utf8_cells(line3, sizeof(line3), next, 24);
  printf("[BlindBadge] lcd_note: LCD shows AI/fallback text with limited built-in CJK glyphs.\n");

  return blind_badge_display_show_lines("BlindBadge", line1, line2, line3);
}

int blind_badge_display_show_ai_wait(const struct blind_badge_event *event)
{
  char line1[BLIND_BADGE_LINE_SIZE];
  char line2[BLIND_BADGE_LINE_SIZE];

  if (event == NULL)
    {
      return blind_badge_display_show_lines("BlindBadge", "AI WAIT",
                                           "", "stop and check");
    }

  snprintf(line1, sizeof(line1), "%s / AI WAIT",
           lcd_severity_name(event->severity));

  if (event->type == BLIND_BADGE_EVENT_OBSTACLE)
    {
      snprintf(line2, sizeof(line2), "%s %dcm",
               blind_badge_direction_name(event->direction),
               event->distance_cm);
    }
  else if (event->type == BLIND_BADGE_EVENT_STEP_DOWN)
    {
      snprintf(line2, sizeof(line2), "%s step",
               blind_badge_direction_name(event->direction));
    }
  else
    {
      snprintf(line2, sizeof(line2), "%s",
               blind_badge_direction_name(event->direction));
    }

  return blind_badge_display_show_lines("BlindBadge", line1, line2,
                                        lcd_default_message(event));
}
