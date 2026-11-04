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

#include <WiFi.h>
#include <WebServer.h>
#include <esp_camera.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#define LED_PIN 3  // GPIO3 = JTAG TMS

static const char *TAG = "cam";

// ===================== WiFi =====================
const char* SSID     = "mate50";
const char* PASS     = "chongyi580";
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
  c.xclk_freq_hz = 10000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAMESIZE_QVGA;
  c.jpeg_quality = 12;
  c.fb_count     = 1;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  if (ESP.getPsramSize() > 0) {
    c.fb_location = CAMERA_FB_IN_PSRAM;
    ESP_LOGI(TAG, "try PSRAM...");
  } else {
    c.fb_location = CAMERA_FB_IN_DRAM;
    ESP_LOGW(TAG, "no PSRAM, DRAM mode");
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK && c.fb_location == CAMERA_FB_IN_PSRAM) {
    ESP_LOGW(TAG, "PSRAM failed (0x%x), retry DRAM...", err);
    esp_camera_deinit();
    delay(300);
    c.fb_location = CAMERA_FB_IN_DRAM;
    err = esp_camera_init(&c);
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "init failed: 0x%x", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 1); }
  ESP_LOGI(TAG, "camera OK %s",
           c.fb_location == CAMERA_FB_IN_PSRAM ? "PSRAM" : "DRAM");
  return true;
}

// ===================== WiFi =====================
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASS);
  ESP_LOGI(TAG, "WiFi: %s ...", SSID);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    localIP = WiFi.localIP().toString();
    ESP_LOGI(TAG, "WiFi OK: %s", localIP.c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    localIP = WiFi.softAPIP().toString();
    ESP_LOGI(TAG, "AP mode: %s", localIP.c_str());
  }
}

// ===================== HTTP 处理 =====================
void handleStream() {
  WiFiClient cl = server.client();
  if (!cl.connected()) return;
  cl.setNoDelay(true);
  cl.setTimeout(3);
  cl.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=f\r\n\r\n");
  cl.flush();

  unsigned long lastFrame = millis();
  while (cl.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      delay(10);
      if (millis() - lastFrame > 5000) break;
      continue;
    }
    lastFrame = millis();

    size_t hdr = cl.print("--f\r\nContent-Type: image/jpeg\r\nContent-Length: ");
    if (!hdr) break;
    cl.print(fb->len);
    cl.print("\r\n\r\n");
    size_t sent = cl.write(fb->buf, fb->len);
    cl.print("\r\n");
    esp_camera_fb_return(fb);

    if (!sent) break;
    delay(1);
    yield();
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
  gpio_reset_pin(GPIO_NUM_3);
  pinMode(LED_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(LED_PIN, LOW); delay(200);
  digitalWrite(LED_PIN, HIGH);

  ESP_LOGI(TAG, "===== ESP32-S3-EYE =====");
  ESP_LOGI(TAG, "CPU:%dMHz PSRAM:%dMB FreeDRAM:%dKB",
           getCpuFrequencyMhz(), ESP.getPsramSize()/1048576, heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024);

  if (!initCamera()) {
    ESP_LOGE(TAG, "Camera failed, restart in 3s");
    delay(3000);
    ESP.restart();
  }
  initWiFi();

  server.on("/", handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.begin();

  ESP_LOGI(TAG, "HTTP ready: http://%s/", localIP.c_str());
  digitalWrite(LED_PIN, LOW);
}

void loop() {
  server.handleClient();
  static unsigned long lastLog = 0;
  if (millis() - lastLog > 10000) {
    lastLog = millis();
    ESP_LOGI(TAG, "heap free DRAM:%dKB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  }
}
