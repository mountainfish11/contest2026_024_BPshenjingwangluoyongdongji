# BlindBadge 视觉链路

## 当前架构

当前稳定版本使用 STA：Windows、ESP32-S3-EYE 主板和 ESP32-S3 副板连接同一个
2.4 GHz Wi-Fi。主板从 `/dev/video0` 获取 640x480 JPEG，通过 HTTP/MJPEG
输出；Windows 使用 PyTorch/Ultralytics 和 NVIDIA GPU 推理；视觉事件再通过
副板 TCP 服务播报并触发短振动。

```text
ESP32-S3-EYE camera
  -> http://<main-board-ip>/stream
  -> Windows vision_assist (CUDA)
  -> speech_bridge
  -> ESP32-S3 sub-board FY-SP701 + vibration motor
```

主板 IP 由热点或路由器 DHCP 分配，不在源码中固定。可从热点客户端列表、串口
网络状态或 `python -m tools.find_board` 获取。

## 主板 HTTP 接口

进入主板 `VISION` 菜单并显示 `RUNNING` 后：

```text
GET /health    服务、相机和流状态
GET /capture   单帧 JPEG（流占用相机时返回 busy）
GET /stream    单客户端 MJPEG 流
```

`/health` 的关键字段为：

```json
{"camera_ready":true,"stream_active":false,"frames":0,"last_error":0}
```

同一时刻只保留一个 MJPEG 客户端，防止旧连接占住相机。退出 VISION 后服务应
停止并释放任务资源。

## Windows 启动

在 `vision` 目录安装依赖并检查 GPU：

```powershell
python -m pip install -r requirements.txt
python -X utf8 -m vision_assist.cli --check-gpu
```

假设主板 IP 为 `<MAIN_IP>`，先验证原始流：

```powershell
python -X utf8 -m tools.probe_stream "mjpeg:http://<MAIN_IP>/stream"
```

启动 GPU 服务：

```powershell
python -X utf8 -m vision_assist.cli `
  --serve `
  --source "mjpeg:http://<MAIN_IP>/stream" `
  --host 127.0.0.1 `
  --port 8760 `
  --device cuda:0 `
  --performance-profile balanced `
  --target-fps 30 `
  --height 1.20 `
  --pitch 30.0 `
  --width-limit 0
```

打开 `http://127.0.0.1:8760/`。也可使用：

```bat
start_windows.bat <MAIN_IP>
verify_board_windows.bat <MAIN_IP>
```

## 视觉语音桥接

GPU 服务正常后，另开终端执行：

```powershell
python -X utf8 .\tools\speech_bridge.py `
  --sub-board <SUB_BOARD_IP> `
  --vision-url "http://127.0.0.1:8760" `
  --interval 0.5
```

桥接只转发视觉服务产生的提醒文本；菜单播报、VOICE、HEALTH 和 ALERT 有各自
任务边界，不应在 VISION 退出后继续占用播放或相机资源。

## 验证门槛

1. Windows、主板、副板位于同一网络且可互相访问；
2. 主板 `/health` 返回 `camera_ready=true`；
3. `/capture` 是可解码的 640x480 JPEG；
4. `tools.probe_stream` 连续收到多帧且没有持续重连；
5. 本地 `/health` 显示 `device=cuda:0`、新帧持续增长；
6. 网页显示标注画面；视觉事件能在副板播报并短振动；
7. 双击 PLAY 退出后，主板 `stream_active=false`，桥接不再发送视觉提醒。

主机处理 FPS 不可能超过主板源 FPS。若二者接近，瓶颈在相机/网络侧，不是
GPU 推理侧。
