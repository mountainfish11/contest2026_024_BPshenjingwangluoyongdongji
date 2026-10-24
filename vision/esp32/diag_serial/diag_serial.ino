/*
 * ============================================================================
 *  串口 + 相机 自检 sketch  （先烧这个，30 秒定位问题）
 * ============================================================================
 *
 * 为什么需要它
 * ------------
 * 原始示例代码从没调用过 Serial.begin()，所有输出都是 ESP_LOGI/ESP_LOGE。
 * 而在 ESP32-S3 上：
 *   - ROM 引导横幅  -> 走 USB Serial/JTAG（板子的 USB 口）
 *   - ESP_LOGI 日志 -> 走 UART0 的 GPIO43/44（USB 口上收不到）
 * 所以只会看到 "ESP-ROM:esp32s3-20210327" 然后就没了。
 *
 * 这个 sketch 做三件事：
 *   1. 用 Serial 打印（配合 工具->USB CDC On Boot = Enable，就打到 USB 口上）
 *   2. 打印芯片信息、PSRAM 大小、可用内存
 *   3. 依次试 QVGA 和 VGA 两种相机配置，把成功/失败和错误码都打出来
 *
 * 【Arduino IDE 设置】
 *   工具 -> Board            ESP32S3 Dev Module
 *   工具 -> PSRAM            OPI PSRAM        <== 关键
 *   工具 -> USB CDC On Boot  Enable           <== 关键
 *   工具 -> Flash Mode       QIO 80MHz
 *   工具 -> 串口              选出现 ROM 横幅的那个 COM 口
 *
 * 【预期输出】
 *   如果一切正常，你会看到：
 *     ===== 串口正常 =====
 *     Chip  : ESP32-S3
 *     PSRAM : 8 MB
 *     FreeDRAM: xxx KB
 *     [1/2] QVGA + fb_count=1 ... OK
 *     [2/2] VGA  + fb_count=2 ... OK   <== 这个 OK 才能烧主固件
 *     心跳 1 秒一次 ...（说明程序在跑）
 *
 *   如果 PSRAM 显示 0 MB，或 VGA 那行报错，就别急着烧主固件，
 *   先把 工具->PSRAM 改成 OPI PSRAM 重新烧。
 * ============================================================================
 */

#include <stdarg.h>   // va_list（logf 要用）
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

// ===================== 摄像头引脚（官方 ESP32-S3-EYE） =====================
#define PWDN  -1
#define RESET -1
#define XCLK  15
#define SIOD   4
#define SIOC   5
#define D0    11
#define D1     9
#define D2     8
#define D3    10
#define D4    12
#define D5    18
#define D6    17
#define D7    16
#define VSYNC  6
#define HREF   7
#define PCLK  13

static const char *TAG = "selftest";

// Serial 在 工具->USB CDC On Boot=Enable 时就是板子的 USB 口
static void logf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  ESP_LOGI(TAG, "%s", buf);   // 同时保留 UART0 通道
}

// ===================== 用指定配置试开一次相机 =====================
static esp_err_t tryCamera(framesize_t size, int quality, int fbCount, const char *label) {
  camera_config_t c;
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = D0; c.pin_d1 = D1; c.pin_d2 = D2; c.pin_d3 = D3;
  c.pin_d4 = D4; c.pin_d5 = D5; c.pin_d6 = D6; c.pin_d7 = D7;
  c.pin_xclk = XCLK; c.pin_pclk = PCLK; c.pin_vsync = VSYNC;
  c.pin_href = HREF; c.pin_sccb_sda = SIOD; c.pin_sccb_scl = SIOC;
  c.pin_pwdn = PWDN; c.pin_reset = RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = size;
  c.jpeg_quality = quality;
  c.fb_count     = fbCount;
  c.grab_mode    = CAMERA_GRAB_LATEST;
  c.fb_location   = (ESP.getPsramSize() > 0) ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&c);
  if (err == ESP_OK) {
    sensor_t *s = esp_camera_sensor_get();
    logf("  [%s] ... OK   (sensor PID=0x%02X, fb_location=%s)",
         label, s ? s->id.PID : 0,
         c.fb_location == CAMERA_FB_IN_PSRAM ? "PSRAM" : "DRAM");
    // 抓一帧看看真的能出图
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      logf("        实测首帧: %ux%u, %u 字节", fb->width, fb->height, (unsigned)fb->len);
      esp_camera_fb_return(fb);
    } else {
      logf("        !! 初始化成功但抓不到帧（内存不足？）");
    }
  } else {
    logf("  [%s] ... 失败  错误码 0x%04X (%s)", label, err, esp_err_to_name(err));
  }
  return err;
}

void setup() {
  Serial.begin(115200);
  delay(1000);                     // 等 USB CDC 枚举

  logf("");
  logf("=====================================================");
  logf("  ESP32-S3-EYE 串口 + 相机 自检");
  logf("  看到这一行 = 日志通道已经通了");
  logf("=====================================================");

  logf("Chip     : %s  rev%d  %d 核",
       ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  logf("CPU      : %d MHz", getCpuFrequencyMhz());
  logf("Flash    : %u KB", (unsigned)(ESP.getFlashChipSize() / 1024));
  logf("PSRAM    : %u MB   <== 这里必须是 8 或 2，是 0 就要去开 OPI PSRAM",
       (unsigned)(ESP.getPsramSize() / 1048576));
  logf("FreeDRAM : %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  logf("FreePSRAM: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  logf("重启原因 : %d", (int)esp_reset_reason());
  logf("");

  if (ESP.getPsramSize() == 0) {
    logf("!!! PSRAM = 0，VGA 模式跑不了。");
    logf("!!! 去 Arduino IDE: 工具 -> PSRAM -> OPI PSRAM，然后重新烧录");
    logf("");
  }

  // ---- 依次试两种配置，用来看清楚 VGA 到底行不行
  logf("开始测试相机配置：");
  esp_err_t e1 = tryCamera(FRAMESIZE_QVGA, 12, 1, "1/2 QVGA 320x240 fb=1");

  if (e1 == ESP_OK) {
    esp_camera_deinit();
    delay(300);
  }

  esp_err_t e2 = tryCamera(FRAMESIZE_VGA, 10, 2, "2/2 VGA  640x480 fb=2");

  logf("");
  if (e1 == ESP_OK && e2 == ESP_OK) {
    logf(">>> 结论：两种都能跑。可以烧主固件 camera_stream_download_vga 了 <<<");
  } else if (e1 == ESP_OK && e2 != ESP_OK) {
    logf(">>> 结论：QVGA 可以，VGA 不行 <<<");
    logf("    最常见原因：PSRAM 没开（工具->PSRAM->OPI PSRAM）");
    logf("    次要原因：fb_count=2 内存不够，把主固件改成 fb_count=1 再试");
  } else {
    logf(">>> 结论：连 QVGA 都失败 <<<");
    logf("    检查：Board 是否选 ESP32S3 Dev Module、排线是否插好、供电是否够");
    logf("    （USB 线太差 / 只供电不传输的数据线也会这样）");
  }

  if (e2 == ESP_OK) {
    esp_camera_deinit();
  }
  logf("");
  logf("接下来每秒打一次心跳，证明程序没卡死也没在重启：");
}

void loop() {
  static unsigned long last = 0;
  static int n = 0;
  if (millis() - last > 1000) {
    last = millis();
    n++;
    logf("心跳 %d 秒   uptime=%lus   FreeDRAM=%uKB   FreePSRAM=%uKB",
         n, millis() / 1000,
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  }
  delay(50);
}
