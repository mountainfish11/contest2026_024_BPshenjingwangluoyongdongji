/*
 * ============================================================================
 *  ESP32-S3 串口监视 / 桥接  (uart_monitor)
 * ============================================================================
 *
 * 功能
 * ----
 *  1. 从 UART1（指定引脚）接收数据，打印到 USB 串口监视器
 *  2. 把串口监视器里敲入的字符转发给 UART1  —— 相当于一个 USB↔UART 透明桥
 *  3. 三种显示模式，可在运行时切换：
 *       r = 原始透传（收到什么打什么，适合二进制/大量文本）
 *       l = 按行显示 + 毫秒时间戳（适合抓另一个设备的日志）
 *       x = 十六进制 + ASCII（适合调试二进制协议）
 *  4. 每秒打印一次统计：累计字节、速率
 *
 * ★★ ESP32-S3 串口的关键坑（这段一定要看）★★
 * ----------------------------------------------------------------------------
 *  ESP32-S3 有**两条完全独立**的串口通道：
 *
 *    ① USB CDC（Type-C 口）          <- 就是本 sketch 里的 Serial
 *    ② UART0（GPIO43=TX / GPIO44=RX）<- ESP_LOGI/ESP_LOGE 默认打到这
 *
 *  原厂示例代码只用 ESP_LOGI 打日志、从不调用 Serial.begin()，所以插 USB
 *  只能看到 ROM 横幅 "ESP-ROM:esp32s3-..."，应用日志一个都看不到——因为
 *  ESP_LOGx 走的是 UART0 的引脚，而那两个引脚没接到 USB 上。
 *
 *  本 sketch 的做法：所有输出走 Serial（USB CDC），并且显式 Serial.begin()。
 *  如果串口监视器什么都没显示，先回 Arduino IDE 检查：
 *      工具 -> USB CDC On Boot -> Enable
 *
 * ★★ 引脚要改 ★★
 * ----------------------------------------------------------------------------
 *  下面 UART_RX_PIN / UART_TX_PIN 默认 17 / 18，这是通用 S3 开发板的常用口。
 *
 *  如果你用的是 **ESP32-S3-EYE**，GPIO16/17/18 被摄像头 D7/D6/D5 占用，
 *  必须改成别的空闲脚，例如：
 *      #define UART_RX_PIN  2      // EYE 上一般空闲
 *      #define UART_TX_PIN  1
 *  （GPIO19/20 是 USB D-/D+，绝对不能用；GPIO26~37 是 Flash/PSRAM，也别用）
 *
 *  接线（和外部设备交叉）：
 *      外部设备 TX  --->  ESP32 UART_RX_PIN
 *      外部设备 RX  <---  ESP32 UART_TX_PIN
 *      外部设备 GND ---   ESP32 GND          （必须共地！）
 *
 *  注意：信号电平是 3.3V。外设如果是 5V TTL 或 RS-232，要做电平转换，
 *        否则会烧引脚。
 *
 * 烧录设置（Arduino IDE）
 * ----------------------
 *  工具 -> Board            ESP32S3 Dev Module
 *  工具 -> PSRAM            OPI PSRAM（只用到串口的话不设也行）
 *  工具 -> USB CDC On Boot  Enable      <== 必须，否则监视器看不到输出
 *  工具 -> Flash Mode       QIO 80MHz
 * ============================================================================
 */

#include <Arduino.h>

// ======================== 可调参数 ========================
//: 串口监视器波特率（USB CDC，改这里不影响外设）
#define MONITOR_BAUD 115200
//: 与外部设备通信的波特率
#define UART_BAUD 115200
//: UART1 引脚（见文件头"引脚要改"）
#define UART_RX_PIN 17
#define UART_TX_PIN 18

//: 是否把监视器里输入的字符转发给 UART1（1=透明桥，0=只收不发）
#define ENABLE_MONITOR_TO_UART 1
//: 按行模式下单行最大长度，超过就强制输出（防止无换行的数据把内存撑爆）
#define LINE_MAX 512
//: 统计信息打印间隔（毫秒）
#define STATS_INTERVAL_MS 1000

// ======================== 显示模式 ========================
enum ShowMode : uint8_t {
  MODE_RAW = 0,   // 原始透传
  MODE_LINE,      // 按行 + 时间戳
  MODE_HEX,       // 十六进制 + ASCII
};
static ShowMode g_mode = MODE_LINE;

// ======================== 运行期状态 ========================
static char g_line[LINE_MAX];
static size_t g_line_len = 0;

static uint32_t g_rx_bytes = 0;       // 从 UART1 收到的累计字节
static uint32_t g_tx_bytes = 0;       // 转发给 UART1 的累计字节
static uint32_t g_lines = 0;          // 按行模式下输出的行数
static uint32_t g_overflow = 0;       // 单行超长被迫截断的次数

static uint32_t g_stat_last_ms = 0;
static uint32_t g_stat_last_rx = 0;

static const char *modeName(ShowMode m) {
  switch (m) {
    case MODE_RAW:  return "原始透传";
    case MODE_LINE: return "按行+时间戳";
    case MODE_HEX:  return "十六进制";
    default:        return "?";
  }
}

static void printHelp() {
  Serial.println();
  Serial.println(F("---------------- 命令 ----------------"));
  Serial.println(F("  r  原始透传（收到什么打什么）"));
  Serial.println(F("  l  按行显示 + 毫秒时间戳"));
  Serial.println(F("  x  十六进制 + ASCII"));
  Serial.println(F("  s  清空统计"));
  Serial.println(F("  ?  显示本帮助"));
  Serial.println(F("  其它字符会被原样转发给 UART1"));
  Serial.println(F("--------------------------------------"));
}

// ======================== 十六进制输出 ========================
// 按标准的 16 字节一行输出：  [时间戳] 48 65 6C 6C 6F ...  |Hello...........|
static uint8_t g_hexbuf[16];
static size_t g_hex_len = 0;

static void hexFlushRow() {
  if (g_hex_len == 0) return;

  Serial.printf("[%8lu] ", (unsigned long)millis());

  // 十六进制列（不足 16 字节补空格对齐）
  for (size_t i = 0; i < 16; i++) {
    if (i < g_hex_len) {
      Serial.printf("%02X ", g_hexbuf[i]);
    } else {
      Serial.print(F("   "));
    }
    if (i == 7) Serial.print(' ');   // 中间多一个空格，便于肉眼分组
  }

  // ASCII 列
  Serial.print(F(" |"));
  for (size_t i = 0; i < g_hex_len; i++) {
    uint8_t b = g_hexbuf[i];
    Serial.print((b >= 0x20 && b <= 0x7E) ? (char)b : '.');
  }
  Serial.println('|');

  g_hex_len = 0;
}

static void hexAppend(uint8_t b) {
  g_hexbuf[g_hex_len++] = b;
  if (g_hex_len >= 16) hexFlushRow();
}

// 把没满一行的一起刷出去（切模式、打统计前调用）
static void hexFlushAll() {
  hexFlushRow();
}

// ======================== 按行输出 ========================
static void lineFlush() {
  if (g_line_len == 0) return;
  // 去掉行尾的 \r
  while (g_line_len > 0 && (g_line[g_line_len - 1] == '\r' ||
                            g_line[g_line_len - 1] == '\n')) {
    g_line_len--;
  }
  if (g_line_len > 0) {
    Serial.printf("[%8lu] ", (unsigned long)millis());
    Serial.write((const uint8_t *)g_line, g_line_len);
    Serial.println();
    g_lines++;
  }
  g_line_len = 0;
}

// ======================== 处理来自 UART1 的字节 ========================
static void handleRxByte(uint8_t b) {
  g_rx_bytes++;

  switch (g_mode) {
    case MODE_RAW:
      Serial.write(b);
      break;

    case MODE_HEX:
      hexAppend(b);
      break;

    case MODE_LINE:
    default:
      if (b == '\n') {
        lineFlush();
      } else {
        if (g_line_len < LINE_MAX - 1) {
          g_line[g_line_len++] = (char)b;
        } else {
          // 超长且没换行：强制输出，避免内存撑爆/数据丢失
          g_overflow++;
          lineFlush();
          g_line[g_line_len++] = (char)b;
        }
      }
      break;
  }
}

// ======================== 处理来自监视器的输入 ========================
static void handleMonitorInput() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;

    switch (c) {
      case 'r': case 'R':
        hexFlushAll();
        lineFlush();
        g_mode = MODE_RAW;
        Serial.printf("\r\n[切换到] %s\r\n", modeName(g_mode));
        return;
      case 'l': case 'L':
        hexFlushAll();
        g_mode = MODE_LINE;
        g_line_len = 0;
        Serial.printf("\r\n[切换到] %s\r\n", modeName(g_mode));
        return;
      case 'x': case 'X':
        lineFlush();
        g_mode = MODE_HEX;
        Serial.printf("\r\n[切换到] %s\r\n", modeName(g_mode));
        return;
      case 's': case 'S':
        g_rx_bytes = g_tx_bytes = g_lines = g_overflow = 0;
        g_stat_last_rx = 0;
        Serial.println(F("\r\n[统计已清零]"));
        return;
      case '?':
        printHelp();
        return;
      default:
        break;
    }

#if ENABLE_MONITOR_TO_UART
    // 其它字符原样转发给 UART1
    Serial1.write((uint8_t)c);
    g_tx_bytes++;
#endif
  }
}

// ======================== 统计 ========================
static void printStats() {
  uint32_t now = millis();
  if (now - g_stat_last_ms < STATS_INTERVAL_MS) return;

  uint32_t dt = now - g_stat_last_ms;
  uint32_t db = g_rx_bytes - g_stat_last_rx;
  // 速率用 B/s，同时给出换算后的 kbit/s，方便和波特率对照
  float bps = dt ? (db * 1000.0f / dt) : 0.0f;

  if (g_mode == MODE_HEX) hexFlushAll();

  Serial.printf("[统计] 收 %lu B (%.0f B/s ≈ %.1f kbit/s)  发 %lu B  行 %lu  截断 %lu  模式 %s\r\n",
                (unsigned long)g_rx_bytes, bps, bps * 8 / 1000.0f,
                (unsigned long)g_tx_bytes, (unsigned long)g_lines,
                (unsigned long)g_overflow, modeName(g_mode));

  g_stat_last_ms = now;
  g_stat_last_rx = g_rx_bytes;
}

// ======================== setup / loop ========================
void setup() {
  // ① USB 串口监视器
  Serial.begin(MONITOR_BAUD);

  // 等 USB CDC 枚举完成，但**必须有上限**：没有主机连接时 Serial 永远不会 ready，
  // 用 while(!Serial) 会让板子卡死在启动阶段。
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {
    delay(10);
  }
  delay(200);   // 给监视器一点时间接上，避免开头几行丢失

  Serial.println();
  Serial.println(F("================================================"));
  Serial.println(F("  ESP32-S3 串口监视 / 桥接"));
  Serial.println(F("  看到这几行 = USB 串口通道正常"));
  Serial.println(F("================================================"));
  Serial.printf("监视器串口 : USB CDC @ %d\r\n", MONITOR_BAUD);

  // ② 与外部设备通信的 UART1
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.printf("外部串口   : UART1  RX=GPIO%d  TX=GPIO%d  @ %d 8N1\r\n",
                UART_RX_PIN, UART_TX_PIN, UART_BAUD);
  Serial.printf("转发输入   : %s\r\n", ENABLE_MONITOR_TO_UART ? "开（透明桥）" : "关（只收不发）");

  printHelp();
  Serial.printf("当前模式   : %s\r\n", modeName(g_mode));
  Serial.println();

  g_stat_last_ms = millis();
}

void loop() {
  // ① UART1 -> 监视器
  // 一次最多取 128 字节，避免外设猛发时长时间占用循环、漏掉监视器输入
  while (Serial1.available() > 0) {
    int c = Serial1.read();
    if (c < 0) break;
    handleRxByte((uint8_t)c);
    if (Serial1.available() > 256) break;   // 让出一次，处理监视器输入
  }

  // ② 监视器 -> UART1（以及模式切换命令）
  handleMonitorInput();

  // ③ 统计
  printStats();

  // 不需要 delay()：above 的 available() 都是非阻塞的，
  // 加 delay 反而会丢字节（UART 硬件 FIFO 只有 128 字节）。
}
