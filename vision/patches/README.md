# ESP32-S3-EYE 固件补丁：升到 VGA

## 为什么必须改

`camera_stream_download.ino` 原始配置是 **QVGA 320×240**。在这个分辨率下：

- 红绿灯在画面里只有 **几个像素**，颜色根本判不出来；
- 远处（5 m 以外）的盲道条只有 1–2 像素宽，方向判定精度严重下降；
- 盲道纹理（条纹 vs 圆点）在远处完全丢失。

所以视觉部分要能用，硬件必须至少出 **VGA 640×480**。

## 改了什么

```diff
-  c.xclk_freq_hz = 10000000;
+  c.xclk_freq_hz = 20000000;
   c.pixel_format = PIXFORMAT_JPEG;
-  c.frame_size   = FRAMESIZE_QVGA;
-  c.jpeg_quality = 12;
-  c.fb_count     = 1;
+  c.frame_size   = FRAMESIZE_VGA;
+  c.jpeg_quality = 10;
+  c.fb_count     = 2;
```

| 参数 | 变化 | 理由 |
|---|---|---|
| `frame_size` | QVGA → VGA | 像素数 ×4，远处目标才可用 |
| `xclk_freq_hz` | 10 MHz → 20 MHz | VGA 下保持帧率；若不稳可退回 10 MHz（帧率会掉） |
| `jpeg_quality` | 12 → 10 | 数值越小质量越高；VGA 下压到 10 让单帧字节数不至于太大 |
| `fb_count` | 1 → 2 | 双缓冲，减少丢帧与撕裂 |

## 我又改了什么（除分辨率之外）

`esp32/camera_stream_download_vga/camera_stream_download_vga.ino` 里还修了两个**会真的把你卡住**的问题：

### ① 串口日志完全不输出

原示例从头到尾没有 `Serial.begin()`，日志全靠 `ESP_LOGI`。而在 ESP32-S3 上：

| | 走哪条通道 | 你看到的现象 |
|---|---|---|
| ROM 横幅 | USB Serial/JTAG | ✅ 能看到 `ESP-ROM:esp32s3-20210327` |
| `ESP_LOGI` 日志 | **UART0 的 GPIO43/44** | ❌ 引脚没接 USB，日志直接丢了 |

现在加了 `Serial.begin(115200)` 和一个 `logf()`，13 处日志全部改走 USB 串口。

### ② 僵尸连接会把板子卡死（这个最坑）

原来的推流循环是：

```cpp
while (cl.connected()) { ... cl.write(...); }
```

客户端（电脑）异常断开后，lwIP 的 TCP 重传超时要 **1–2 分钟**才会让 `cl.write()` 失败。
这段时间 `handleStream()` 一直堵在循环里，`loop()` 里的 `server.handleClient()`
完全跑不动——**表现就是"TCP 能连上，但 /stream 和 /capture 全都没响应"**，
只能断电重启。

现在加了连接时间上限：

```cpp
#define STREAM_MAX_MS 20000UL          // 单条连接最多推 20 秒
#define STREAM_MAX_IDLE_MS 5000UL      // 连续 5 秒拿不到帧就退出

while (cl.connected()) {
  if (millis() - connStart > STREAM_MAX_MS) break;   // 主动让出 loop()
  ...
  if (sent == 0) break;                               // 写失败立刻退
}
cl.stop();                                            // 明确关闭，不留悬挂
```

正常使用时完全无感：浏览器端 `onerror` 会 2 秒后自动重连，电脑端
`MjpegSource` 也会自动重连。但板子最多卡 20 秒就自愈，不再需要拔电。

## 怎么改

### 路线 A：用 Arduino IDE 烧录（推荐，不需要 git）

**已经帮你改好了**，直接打开这个 sketch 就能烧：

```
esp32/camera_stream_download_vga/camera_stream_download_vga.ino
```

> Arduino IDE 要求**文件夹名和 .ino 文件名一致**，所以新目录名也带了 `_vga`
> 后缀，直接双击打开即可，不会报"sketch 必须在同名文件夹中"。

如果你想在原始 sketch 上自己改，就是这 4 行（在 `initCamera()` 里）：

```cpp
// 改前                              // 改后
c.xclk_freq_hz = 10000000;           c.xclk_freq_hz = 20000000;
c.frame_size   = FRAMESIZE_QVGA;     c.frame_size   = FRAMESIZE_VGA;
c.jpeg_quality = 12;                 c.jpeg_quality = 10;
c.fb_count     = 1;                  c.fb_count     = 2;
```

### 路线 B：用 git 打补丁

在 ESP32 工程根目录（`camera_stream_download/` 的上一层）执行：

```bash
git apply --check patches/esp32_vga.patch   # 先检查能不能打上
git apply patches/esp32_vga.patch
```

> **行尾说明**：原 `.ino` 是 LF 行尾。如果你在 Windows 且 `core.autocrlf=true`，
> `git apply` 会把整个文件转成 CRLF（内容不变，只是行尾）。这属于 git 的正常行为；
> 若不想让它发生，先 `git config core.autocrlf false` 再打补丁。

## 烧录前必查的 Arduino IDE 设置（缺一不可）

| 菜单位置 | 要设成 | 为什么 |
|---|---|---|
| 工具 → Board | **ESP32S3 Dev Module** | ESP32-S3-EYE 用这个板型 |
| 工具 → PSRAM | **OPI PSRAM** | **最关键**。VGA + 双缓冲要 PSRAM；设错会停在相机初始化失败并重启循环 |
| 工具 → Flash Mode | QIO 80MHz | 吞吐 |
| 工具 → Flash Size | 按你的板子（EYE 一般是 8MB） | — |
| 工具 → USB CDC On Boot | Enable | 才能看到串口日志 |
| 工具 → Upload Speed | 921600 | 快一些，不稳就降到 460800 |

烧完打开 **工具 → Serial Monitor，波特率 115200**，应能看到：

```
===== ESP32-S3-EYE =====
CPU:240MHz PSRAM:8MB FreeDRAM:xxxKB
camera OK PSRAM
WiFi OK: 192.168.x.x
HTTP ready: http://192.168.x.x/
```

两个必查点：

- `camera OK PSRAM` —— 如果显示 `no PSRAM, DRAM mode` 或反复 `Camera failed, restart in 3s`，
  就是 PSRAM 没开对（回去把 工具 → PSRAM 设成 OPI PSRAM 重烧）
- `WiFi OK: 192.168.x.x` —— 记下这个 IP；如果变成 `AP mode`，说明没连上 WiFi，
  会开热点 `ESP32-S3-CAM`，那电脑也得连这个热点才能访问

> **电脑和板子必须连同一个 WiFi/热点**，否则电脑上的视觉服务连不上它。
> sketch 里的 `SSID` / `PASS` 在第 23–24 行，按你的网络改。

## 打完补丁后怎么验证

```bash
# 1. 浏览器直接看板子出的图，确认显示 640x480
#    http://<esp32-ip>/           右下角 badge 显示分辨率
#    http://<esp32-ip>/capture    单张快照

# 2. 让视觉端接上来跑
python -m vision_assist.cli --serve \
    --source mjpeg:http://<esp32-ip>/stream \
    --height 1.2 --pitch 30

# 3. 打开 http://127.0.0.1:8760/
#    看右上角 "源: mjpeg"、FPS、以及 /health 里 reconnects 是不是 0
```

## 预期影响

- 分辨率：320×240 → 640×480
- 单帧 JPEG 体积约增大 3–4 倍（质量 10 已做过补偿）
- 帧率：WiFi 下预计 **8–15 FPS**，取决于信号质量与热点拥塞
- 视觉端 `vision_assist` 会自动按实际帧尺寸换算内参，**不需要改代码**，
  但标定参数（离地高度 / 俯仰角）要重新用 `python -m tools.calibrate` 对一遍

## 打完补丁后怎么验证

```bash
# 1. 浏览器直接看板子出的图
#    http://<esp32-ip>/           确认分辨率显示 640x480
#    http://<esp32-ip>/capture    单张快照

# 2. 让视觉端接上来跑
python -m vision_assist.cli --serve \
    --source mjpeg:http://<esp32-ip>/stream \
    --height 1.2 --pitch 30

# 3. 打开 http://127.0.0.1:8760/ ，看右侧 FPS 与 "源: mjpeg"
```
