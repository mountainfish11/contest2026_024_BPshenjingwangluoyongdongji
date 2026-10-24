/*
 * ============================================================================
 * 【VGA 版本】为「盲人辅助设备视觉识别」改的固件
 *
 * 相对原始 camera_stream_download.ino 只改了 4 行，作用是把分辨率从
 * QVGA 320x240 提到 VGA 640x480 —— QVGA 下红绿灯只有几个像素，认不出来。
 *
 *   c.xclk_freq_hz  10000000 -> 20000000   （VGA 下保住帧率）
 *   c.frame_size    QVGA     -> VGA        （像素数 x4）
 *   c.jpeg_quality  12       -> 10         （质量更高，补偿像素变多）
 *   c.fb_count      1        -> 2          （双缓冲，减少丢帧）
 *
 * 【烧录前必查 Arduino IDE 设置】缺一不可：
 *   工具 -> Board            ESP32S3 Dev Module
 *   工具 -> PSRAM            OPI PSRAM        <== 必须！VGA+双缓冲要 PSRAM
 *   工具 -> Flash Mode       QIO 80MHz
 *   工具 -> USB CDC On Boot  Enable           （为了看串口日志）
 *   工具 -> Partition Scheme 随意（本 sketch 不用文件系统）
 *
 * 【烧完怎么看结果】
 *   工具 -> Serial Monitor，波特率 115200，会打印：
 *     WiFi OK: 192.168.x.x
 *     HTTP ready: http://192.168.x.x/
 *   浏览器打开那个地址，右下角 badge 应显示 640x480。
 *
 * 【注意】电脑要和板子连同一个 WiFi/热点，否则电脑上的视觉服务连不上它。
 * ============================================================================
 */
/*
 * ESP32-S3-EYE 图传 + 浏览器录制下载
 * 硬件: ESP32-S3-EYE v2.2 (OV2640)
 * 环境: Arduino IDE + ESP32 >= 2.0.14
 *
 * 重要设置:
 *   工具 → PSRAM → OPI PSRAM
 *   工具 → USB CDC On Boot → Enable
 *   工具 → Flash Mode → QIO 80MHz
 */

#include <stdarg.h>   // va_list（logf 要用）
#include <WiFi.h>
#include <WebServer.h>
#include <esp_camera.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#define LED_PIN 3  // GPIO3 = JTAG TMS

static const char *TAG = "cam";

// ===================== WiFi =====================
const char* SSID     = "QxQ";
const char* PASS     = "12345609";
#define AP_SSID     "ESP32-S3-CAM"

// ===================== 摄像头引脚 (官方 ESP32-S3-EYE) =====================
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


// ===================== 串口日志 =====================
// 原始示例只用 ESP_LOGI/ESP_LOGE，它们在 ESP32-S3 上走 UART0 的 GPIO43/44，
// 而板子的 USB 口接的是 USB Serial/JTAG，所以日志根本到不了串口监视器
// （现象：只看到一行 "ESP-ROM:esp32s3-20210327" 就没了）。
// 这里统一走 Serial（配合 工具->USB CDC On Boot = Enable 就是同一个 USB 口）。
static void logf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  ESP_LOGI(TAG, "%s", buf);   // 同时保留 UART0 通道，方便接 USB-TTL 的人
}

WebServer server(80);
String localIP = "";

// ===================== HTML 页面 =====================
const char HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3-EYE</title>
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif}
*{box-sizing:border-box}body{margin:0;background:#15171a;color:#fff}
main{width:min(920px,100%);margin:auto;padding:16px}
h1{font-size:22px}.video-wrap{position:relative;border-radius:14px;overflow:hidden;background:#000}
img,video{display:block;width:100%;max-height:70vh;background:#000}
canvas{display:none}.badge{position:absolute;left:10px;bottom:10px;padding:5px 9px;border-radius:8px;background:#000a;font-size:13px}
.controls{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:14px}
button{min-height:48px;border:0;border-radius:12px;font-size:17px;font-weight:700;color:#fff;cursor:pointer}
button:disabled{opacity:.4;cursor:not-allowed}
#rec{background:#16894c}#stop{background:#c93636}#snap{background:#1769aa;margin-top:12px;width:100%}
.status{display:flex;justify-content:space-between;margin-top:12px;padding:10px 12px;border-radius:10px;background:#ffffff0d}
#dl{display:none;margin-top:12px;padding:14px;border-radius:10px;text-align:center;background:#1769aa;color:#fff;text-decoration:none}
#dl.show{display:block}.hint{color:#b9bec7;font-size:13px;margin-top:10px}
</style></head><body>
<main><h1>ESP32-S3-EYE 图传</h1>
<div class="video-wrap"><img id="live" src="/stream" alt="加载中...">
<canvas id="c"></canvas><div class="badge"><span id="res">等待视频</span> | <span id="fps">FPS:--</span></div></div>
<div class="controls"><button id="rec" disabled>开始录制</button><button id="stop" disabled>停止录制</button></div>
<button id="snap" disabled>拍照快照</button>
<div class="status"><span id="state">正在连接……</span><span id="timer">00:00</span></div>
<a id="dl">下载视频</a><p class="hint">视频由浏览器录制，不占用ESP32存储。</p></main>
<script>
const v=document.getElementById('live'),cv=document.getElementById('c'),cx=cv.getContext('2d');
const r=document.getElementById('rec'),s=document.getElementById('stop'),sn=document.getElementById('snap');
const st=document.getElementById('state'),tm=document.getElementById('timer'),rs=document.getElementById('res'),fs=document.getElementById('fps'),dl=document.getElementById('dl');
let mr=null,ch=[],th=null,rs0=0,rec=false,ld=null,fc=0,fl=Date.now();
v.onload=()=>{r.disabled=sn.disabled=false;st.textContent='视频已连接';rs.textContent=v.naturalWidth+'x'+v.naturalHeight;cv.width=v.naturalWidth||640;cv.height=v.naturalHeight||480;fc++;let n=Date.now();if(n-fl>=1000){fs.textContent='FPS:'+Math.round(fc*1000/(n-fl));fc=0;fl=n}};
v.onerror=()=>{st.textContent='视频断开,重连中...';r.disabled=true;setTimeout(()=>{v.src='/stream?t='+Date.now()},2000)};
function ut(){let sec=Math.floor((Date.now()-rs0)/1000);tm.textContent=String(Math.floor(sec/60)).padStart(2,'0')+':'+String(sec%60).padStart(2,'0')}
function df(){if(!rec)return;try{cx.drawImage(v,0,0,cv.width,cv.height)}catch(e){}if(rec)requestAnimationFrame(df)}
r.onclick=()=>{if(!v.complete||!v.naturalWidth)return;ch=[];dl.classList.remove('show');if(ld){URL.revokeObjectURL(ld);ld=null}
cv.width=v.naturalWidth;cv.height=v.naturalHeight;let stm=cv.captureStream(30),mt=MediaRecorder.isTypeSupported('video/webm;codecs=vp8')?'video/webm;codecs=vp8':'video/webm';
mr=new MediaRecorder(stm,{mimeType:mt});mr.ondataavailable=e=>{if(e.data.size)ch.push(e.data)};
mr.onerror=e=>{st.textContent='录制错误'};mr.onstop=()=>{clearInterval(th);rec=false;
let b=new Blob(ch,{type:mr.mimeType||'video/webm'});ld=URL.createObjectURL(b);dl.href=ld;
dl.download='esp32cam-'+new Date().toISOString().replace(/[:.]/g,'-')+'.webm';
dl.textContent='下载('+(b.size/1048576).toFixed(1)+'MB)';dl.classList.add('show');
st.textContent='完成,点上方下载';r.disabled=false;s.disabled=true};
mr.start(1000);rs0=Date.now();rec=true;df();ut();th=setInterval(ut,500);
st.textContent='录制中...';r.disabled=true;s.disabled=false};
s.onclick=()=>{if(mr&&mr.state==='recording')mr.stop()};
sn.onclick=()=>{let a=document.createElement('a');a.href='/capture';a.download='p-'+Date.now()+'.jpg';a.click()};
</script></main></body></html>
)rawliteral";

// ===================== 摄像头初始化 =====================
bool initCamera() {
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
  c.frame_size   = FRAMESIZE_VGA;
  c.jpeg_quality = 10;
  c.fb_count     = 2;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  if (ESP.getPsramSize() > 0) {
    c.fb_location = CAMERA_FB_IN_PSRAM;
    logf("try PSRAM...");
  } else {
    c.fb_location = CAMERA_FB_IN_DRAM;
    logf("no PSRAM, DRAM mode");
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK && c.fb_location == CAMERA_FB_IN_PSRAM) {
    logf("PSRAM failed (0x%x), retry DRAM...", err);
    esp_camera_deinit();
    delay(300);
    c.fb_location = CAMERA_FB_IN_DRAM;
    err = esp_camera_init(&c);
  }

  if (err != ESP_OK) {
    logf("init failed: 0x%x", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 1); }
  logf("camera OK %s",
           c.fb_location == CAMERA_FB_IN_PSRAM ? "PSRAM" : "DRAM");
  return true;
}

// ===================== WiFi =====================
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // 关掉省电，推流更稳
  WiFi.begin(SSID, PASS);
  logf("WiFi: 正在连接 \"%s\" ...", SSID);

  // 最多等 40 秒（热点慢、DHCP 慢都要留余量），每 5 秒报一次进度
  const unsigned long TOTAL_MS = 40000;
  unsigned long t0 = millis();
  unsigned long lastReport = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < TOTAL_MS) {
    delay(200);
    if (millis() - lastReport >= 5000) {
      lastReport = millis();
      logf("  等待中 %lus ... 状态码=%d", (millis() - t0) / 1000, (int)WiFi.status());
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    localIP = WiFi.localIP().toString();
    logf("WiFi OK: %s  (信号 %d dBm)", localIP.c_str(), WiFi.RSSI());
    logf("");
    logf("  >>> 浏览器打开: http://%s/ <<<", localIP.c_str());
    logf("     快照地址:   http://%s/capture", localIP.c_str());
    logf("     视觉端拉流: mjpeg:http://%s/stream", localIP.c_str());
    logf("");
  } else {
    logf("");
    logf("!!! 连不上 \"%s\"，改用 AP 模式（板子自己发热点）", SSID);
    logf("    最常见原因：手机热点是 5GHz。ESP32-S3 只支持 2.4GHz!");
    logf("    -> 手机热点设置里改成 2.4GHz（或打开\"最大兼容性\"），再重烧");
    logf("    其他原因：SSID/密码写错；板子开机时热点还没起来");
    logf("");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    localIP = WiFi.softAPIP().toString();
    logf("AP 模式已开启");
    logf("  热点名: %s   (无密码)", AP_SSID);
    logf("  >>> 电脑要连上这个热点，然后浏览器打开: http://%s/ <<<", localIP.c_str());
    logf("  注意：连这个热点时电脑会没有外网，属正常现象");
    logf("");
  }
}

// ===================== HTTP 处理 =====================
// ===================== 推流 =====================
// 关键改动：给每条推流连接加**时间上限**。
//
// 原来的 while (cl.connected()) 有个致命问题：客户端（电脑）异常断开后，
// lwIP 的 TCP 重传超时要 1–2 分钟才会让 cl.write() 返回失败，而这段时间
// handleStream() 一直堵在里面，loop() 里的 server.handleClient() 完全跑不动
// —— 表现就是"TCP 能连上，但 /stream 和 /capture 全都没响应"。
//
// 加时间上限后，最多 STREAM_MAX_MS 就会主动断开、让出 loop()：
//   * 浏览器端 onerror 会 2 秒后自动重连（页面里已实现）
//   * 电脑端 vision_assist 的 MjpegSource 也会自动重连
// 所以对正常使用完全无感，但板子不会再卡死。
#define STREAM_MAX_MS 20000UL
#define STREAM_MAX_IDLE_MS 5000UL

void handleStream() {
  WiFiClient cl = server.client();
  if (!cl.connected()) return;
  cl.setNoDelay(true);
  cl.setTimeout(3);
  cl.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=f\r\n\r\n");
  cl.flush();

  unsigned long connStart = millis();
  unsigned long lastFrame = connStart;
  unsigned long framesSent = 0;

  while (cl.connected()) {
    // ① 连接活得太久 -> 主动让出，给 handleClient() 服务其它请求的机会
    if (millis() - connStart > STREAM_MAX_MS) {
      logf("推流达到时间上限（%lu 秒，%lu 帧），主动断开以释放 WebServer",
           STREAM_MAX_MS / 1000, framesSent);
      break;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      delay(10);
      if (millis() - lastFrame > STREAM_MAX_IDLE_MS) break;
      continue;
    }

    size_t hdr = cl.print("--f\r\nContent-Type: image/jpeg\r\nContent-Length: ");
    if (!hdr) { esp_camera_fb_return(fb); break; }
    cl.print(fb->len);
    cl.print("\r\n\r\n");
    size_t sent = cl.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
    cl.print("\r\n");

    // ② 写失败（客户端已消失）立刻退出，不要继续等
    if (sent == 0) break;
    lastFrame = millis();
    framesSent++;
    delay(1);
    yield();
  }

  cl.stop();   // 明确关闭，避免连接悬挂
  if (framesSent) {
    logf("推流结束：%lu 帧，用时 %lu 秒", framesSent, (millis() - connStart) / 1000);
  }
}

void handleCapture() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { server.send(500, "text/plain", "fail"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=photo.jpg");
  server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", HTML);
}

// ===================== Setup =====================
void setup() {
  // 【必须】初始化串口。原示例没有这句，所以什么都打不出来。
  Serial.begin(115200);
  delay(800);                       // 等 USB CDC 枚举完成

  logf("");
  logf("================================================");
  logf("  ESP32-S3-EYE  VGA 图传  (视觉识别专用固件)");
  logf("  如果你的串口看到这几行，说明日志通道正常了");
  logf("================================================");

  gpio_reset_pin(GPIO_NUM_3);
  pinMode(LED_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(LED_PIN, LOW); delay(200);
  digitalWrite(LED_PIN, HIGH);

  logf("===== ESP32-S3-EYE =====");
  logf("CPU:%dMHz PSRAM:%dMB FreeDRAM:%dKB",
           getCpuFrequencyMhz(), ESP.getPsramSize()/1048576, heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024);

  if (!initCamera()) {
    logf("");
    logf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    logf("  相机初始化失败，板子会在 5 秒后重启并重复这个过程");
    logf("  -> 99%% 是 Arduino IDE 里 工具->PSRAM 没设成 OPI PSRAM");
    logf("  -> 回 IDE 改设置，重新烧录");
    logf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.flush();
    delay(5000);
    ESP.restart();
  }
  initWiFi();

  server.on("/", handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.begin();

  logf("HTTP ready: http://%s/", localIP.c_str());
  digitalWrite(LED_PIN, LOW);
}

void loop() {
  server.handleClient();
  static unsigned long lastLog = 0;
  if (millis() - lastLog > 10000) {
    lastLog = millis();
    logf("heap free DRAM:%dKB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  }
}
