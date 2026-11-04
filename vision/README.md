# 盲人辅助设备 · 视觉识别子系统

从 ESP32-S3-EYE 的 MJPEG 图传拉流，实时识别盲道方向、砖型、障碍物、斑马线、红绿灯，
输出结构化 JSON 与中文播报建议，并带一个可视化调试台。

零新依赖：只用 `ultralytics` + `torch` + `opencv` + `numpy` + `scipy` 与 Python 标准库。

---

## 1. 它解决什么问题

参照项目 `材料/blind_navigation-2.2.0/` 的视觉部分是 `routes/video.py` 里约 40 行：
用 YOLO 检出盲道框，**要求至少 2 个框**，对框中心做 `polyfit(ys→xs)` 取斜率，
`|slope| > 0.41` 就播报左右转，全局 14 秒冷却。

实测它的问题（详见 `docs/设计.md` 第 2 节）：**在 `Test.mp4` 上有 33/300 帧（11.0%）
因框数不足而完全无法判定**，而且像素斜率并不等于几何航向角。

本实现的核心做法：**把一切几何判断搬到地面度量空间**。用针孔模型把像素反投影到地面，
得到真实的米和度，再在度量空间里算横向偏移、航向角、距离、斑马线周期性。
同时补上了时序去抖、状态机、正交警报、以及障碍物/红绿灯/斑马线检测。

## 2. 能力矩阵（重要：精度等级不同）

| 能力 | 方法 | 精度等级 |
|---|---|---|
| 盲道存在 / 范围 | 复用参照项目的 `best.pt`（YOLOv8n 自训） | **模型级**，mAP50 0.678 是其上限 |
| 行进砖 / 提示砖 | LSD 长线段统计（在 8 个标注样本上 **8/8**） | 规则级，已实测 |
| 横向偏移 / 航向角 | 地面反投影 + 中心线鲁棒拟合 | 几何精确（依赖标定） |
| 障碍物 + 距离 | YOLOv8n-COCO + 框底边反投影 | 模型级 |
| 红绿灯状态 | COCO `traffic light` 框 + 框内 HSV + 时序投票 | 模型级检测 + 规则级颜色 |
| 斑马线 | 度量空间白条沿 Z 的周期性 | 规则级 |
| 台阶 | 度量空间亮度跳变 + 色相不变性 + 多帧确认 | 规则级，**只报"疑似"** |
| 盲道丢失 / 画面质量 | 跟踪丢失计时；亮度 / Laplacian / 过曝 | 规则级 |

## 3. 快速开始

### 3.1 先标定（**必做**，不标定距离和偏移都是错的）

```bash
# 有桌面环境：拖动滑块，让地面距离网格与实际地面吻合
python -m tools.calibrate --source mjpeg:http://<esp32-ip>/stream \
    --height 1.2 --pitch 30

# 无桌面环境：直接出标注图看对不对
python -m tools.calibrate --source file:data/Test.mp4 \
    --frame 0 --pitch 85 --height 1.3 --no-gui --save out/calib.jpg
```

三个参数的含义：

- `--height`：镜头离地高度（米）
- `--pitch`：镜头**低于水平线**的角度（度），正数朝下看
- `--hfov`：水平视场角（度），ESP32-S3-EYE 的 OV2640 约 65°

把得到的值写进 `calibration.json`，或者用 `--height/--pitch` 每次传：
```bash
python -m tools.calibrate ... --save-config calibration.json
export VISION_CONFIG=calibration.json     # Windows: $env:VISION_CONFIG="calibration.json"
```

### 3.2 起调试台

```bash
python -m vision_assist.cli --serve \
    --source mjpeg:http://<esp32-ip>/stream \
    --height 1.2 --pitch 30 --port 8760
```

浏览器打开 <http://127.0.0.1:8760/> ：左侧实时画面（带检测框与中心线），
右侧引导状态 / 警报 / 关键指标 / 播报时间线，底部可切帧源、改标定。

### 3.3 没有板子也能跑

```bash
# 终端 A：把本地视频模拟成 ESP32 的 MJPEG 流
python -m tools.mock_stream --video data/Test.mp4 --port 8099

# 终端 B：
python -m vision_assist.cli --serve --source mjpeg:http://127.0.0.1:8099/stream \
    --pitch 85 --height 1.3
```

### 3.4 纯命令行

```bash
# 逐帧输出 Scene JSON
python -m vision_assist.cli --source file:xxx.mp4 --json --frames 100 --pitch 85

# 存带叠加的视频 + 缩略图墙
python -m vision_assist.cli --source file:xxx.mp4 --save-video out/run.mp4 \
    --save-sheet out/sheet.jpg

# 只验几何，不加载模型
python -m vision_assist.cli --source file:xxx.mp4 --no-detect
```

## 4. 接 ESP32-S3

硬件侧**必须先升级分辨率**，否则红绿灯只有几个像素、远处盲道纹理全丢。

**用 Arduino IDE 烧录的话**，直接打开这个已改好的 sketch 就能烧：

```
esp32/camera_stream_download_vga/camera_stream_download_vga.ino
```

只需 4 行改动（已帮你改好）：

```cpp
c.frame_size   = FRAMESIZE_QVGA;  →  FRAMESIZE_VGA;   // 320x240 -> 640x480
c.jpeg_quality = 12;              →  10;
c.fb_count     = 1;               →  2;
c.xclk_freq_hz = 10000000;        →  20000000;
```

烧录前**必须**把 `工具 → PSRAM` 设成 **OPI PSRAM**，否则 VGA+双缓冲会初始化失败、
板子反复重启。完整的 IDE 设置清单、串口日志怎么读、IP 怎么找、常见报错，
见 [`patches/README.md`](patches/README.md)。

用 git 的话也可以打补丁：`git apply patches/esp32_vga.patch`。

打完补丁后在浏览器打开 `http://<esp32-ip>/` 确认显示 `640x480`，然后：

```bash
python -m vision_assist.cli --serve --source mjpeg:http://<esp32-ip>/stream
```

视觉端会自动按实际帧尺寸换算内参，**换分辨率不需要改代码**，但标定值要重新对一遍。

## 5. 输出协议

协议版本 `1.0`（`vision_assist/schema.py`，字段只增不改）。一帧示例（节选）：

```json
{
  "ts": 1739123456.78, "frame": 1234,
  "state": "DRIFT_LEFT", "state_reason": "offset:-0.18m <= -0.15",
  "tactile": {"present": true, "tile_type": "guide", "tile_type_conf": 0.86,
              "lateral_offset_m": -0.18, "heading_deg": -12.5,
              "near_m": 0.9, "far_m": 4.2, "warning_near_m": null,
              "on_warning_tile": false},
  "tracker": {"offset_m": -0.18, "heading_deg": -12.5, "lost": false},
  "alerts": {"alerts": [{"kind": "OBSTACLE_NEAR", "priority": 100,
                         "distance_m": 2.4, "detail": "行人"}],
             "on_warning_tile": false},
  "speech": {"event": "OBSTACLE_NEAR", "text": "前方2.4米有行人",
             "spoke": true, "priority": 100, "reason": "ok"},
  "latency_ms": {"total": 71.1}, "fps": 14.1
}
```

**两点约定**：

- 单位一律**米**和**度**；`offset_m` 正=盲道在右，`heading_deg` 正=前方盲道向右偏。
  状态名按"用户该往哪边走"命名（`offset_m < 0` → `DRIFT_LEFT`）。
- `tactile` 是本帧原始检测，`tracker` 是平滑去抖后的结果。**对接方优先用 `tracker`**。

服务端点：`/stream.mjpg`（带叠加的 MJPEG）、`/events`（SSE 推 JSON）、
`/api/scene`、`/snapshot.jpg`、`/health`、`POST /source`、`POST /config`、`POST /reset`。

## 6. 评测

```bash
python -m eval.run --max-frames 300       # 结果写到 eval/report.md
```

在 `Test.mp4` 前 300 帧上的实测（详见 `eval/report.md`）：

| 指标 | 本实现 | 参照实现（slope 法） |
|---|---|---|
| 能给出方向/偏移的帧 | **298/300（99.3%）** | 267/300（89.0%） |
| 因框数不足而"失明" | 0 | **33（11.0%）** |
| 端到端延迟 | p50 **71 ms** / p95 88 ms | 约 30 ms（只跑一次 YOLO，无其他能力） |
| 行进砖/提示砖准确率 | **8/8** | 不提供该能力 |
| Scene JSON schema 校验失败 | 0 | — |

> **斑马线 / 红绿灯 / 台阶的准确率目前是 N/A**——现有素材 `Test.mp4` 里没有这些目标。
> 需要用现场实拍视频标注后重跑，工具在 `eval/label_tool.py`。此处不编造数字。

## 7. 已知限制（不粉饰）

1. `best.pt` 的 mAP50 只有 **0.678**，盲道召回的上限就锁在这里。想突破只能重训，
   需要补标注数据。
2. **ESP32 默认 QVGA 320×240，红绿灯只有几个像素，判不出来**。必须打
   `patches/esp32_vga.patch` 升到 VGA。
3. **台阶检测只能给出"疑似"**，置信度上限压在 0.6。宁可漏报不误报。
4. 纯 CPU 跑双模型约 **70 ms/帧 ≈ 14 FPS**（本机 32 核、YOLOv8n）。
5. 单目测距依赖地面平面假设，**对悬空障碍（树枝、招牌）无效**，且标定参数不准会
   直接放大误差。
6. 胸挂相机会拍到自己**的鞋/腿**，COCO 会识别成 `person`；已用
   `obstacle.ignore_below_m = 0.7` 过滤近处检测。
7. `torch` 线程数不是越多越好——本机实测 `threads=1` 最快，`threads=2` 反而慢一倍。
   默认已设为 1；核数少的机器可试 2–4。

## 8. 性能调优（帧数低 / 画面卡）

### 两个曾经把吞吐砍到 1/4 的实现 bug（已修，有回归测试）

| bug | 现象 | 根因 |
|---|---|---|
| `HTTPResponse.read(65536)` | 处理只有 **4–5 fps**，而 `probe_stream` 能测出 22 fps | `read(n)` 会**阻塞到攒满 n 字节**。板子一帧才 9 KB，等于每 7 帧（约 350 ms）才返回一次，帧一阵一阵到达。改用 `read1()`（有多少给多少） |
| 消费端用 `time.sleep()` 轮询等帧 | 每帧白等 100+ ms | Windows 的 sleep 粒度是 **15.6 ms**，`sleep(0.002)` 实际睡 15 毫秒。改成条件变量 `wait_for_frame()` 阻塞等待 |

修完在真板子（VGA、19 fps 流）上的实测：

```
源帧率   : 17.2 fps   带宽 199 KB/s   9 KB/帧
处理帧率 : 15.8–17.8 fps      ← 已经跟得上流，不再是瓶颈
GPU      : cuda:0（RTX 4060 Laptop）
分项     : tactile ≈ 10 ms   coco ≈ 11 ms   total ≈ 26 ms
```

**教训**：当 `probe_stream`（原始 socket）和服务端测出的帧率差好几倍时，先怀疑
读取层，不要怀疑模型。

### 先分清是谁慢

调试页顶部的徽章直接告诉你瓶颈在哪：

| 徽章 | 含义 | 怎么判断 |
|---|---|---|
| **源FPS** | 流本身每秒到达多少帧 | 明显高于 FPS → **处理端慢** |
| **FPS** | 实际处理了多少帧 | 两者都低 → **采集端/网络慢** |
| **跳过** | 处理不过来而跳过的帧数 | 这是**正常设计**，不是故障 |

> **关于"丢包"**：处理慢时旧实现会让 TCP 缓冲区堆积，进而把 ESP32 的
> `cl.write()` 堵住，表现成"画面一顿一顿、延迟越积越大"。现在后台线程
> （`LatestFrameSource`）永远把流读空，前台只处理**最新**那帧——所以"跳过帧"
> 反而是健康的表现：它保证你看到的永远是当下画面，而不是几秒前的旧帧。

### 处理端是瓶颈时怎么降开销

```bash
# 盲道模型从 640 降到 416，处理帧率约 +25%，代价是远端精度略降
python -m vision_assist.cli --serve --source ... --imgsz 416

# 再降到 320，约 +40%
python -m vision_assist.cli --serve --source ... --imgsz 320

# 限制处理帧率，避免吃满 CPU 拖累别的程序
--target-fps 12
```

本机（32 核 CPU、YOLOv8n×2、**VGA 640×480 输入**，即板子的真实输出）实测：

| 配置 | 源帧率 | 处理帧率 |
|---|---|---|
| imgsz 640（默认） | 24.2 | **16.6** |
| imgsz 416 + 障碍每 2 帧 | 24.5 | **17.1** |
| imgsz 416 + 障碍每 3 帧 | 24.5 | **19.4** |
| imgsz 320 + 障碍每 3 帧 | 24.5 | **22.5** |

源帧率始终稳定在 24+ fps，说明**网络和板子都不是瓶颈，纯 CPU 推理是瓶颈**。
这对用途影响不大——语音引导每秒 16 次更新已经远超需求。

> 早期文档里写的 "12–14 fps" 是拿 720×1280 的测试视频测的，对 VGA 板子**偏悲观**，
> 已按真实分辨率修正。

> **ONNX Runtime 不会更快**：本机实测 ONNX 反而比 torch 慢（盲道 640 下
> 79.1 ms vs 31.6 ms）。torch 2.12 的 CPU 后端（oneDNN）已经很好。
> ONNX 的价值在于**可移植性**（安卓 / RK3588 / Jetson 用），不是桌面提速。
> 导出产物在 `models/*.onnx`，固定了输入尺寸，给后续移植用。

### 用上 GPU（有 NVIDIA 显卡时，提速 5–10 倍）

先检查：

```bash
python -m vision_assist.cli --check-gpu
```

如果显示 `CUDA 编译版本: 无 —— 这是 CPU 专用版 torch`，说明装了 CPU 版 PyTorch，
显卡完全没用上（这是最坑的一点：`torch.cuda.is_available()` 返回 False，而代码
只会静默跑 CPU）。

**解决（在你自己的终端里跑，不要用 `--upgrade` 之外的参数乱改）**：

```bash
# CUDA 12.4 + torch 2.6（驱动 555 及以上都稳）
python -m pip install "torch==2.6.0+cu124" "torchvision==0.21.0+cu124" ^
    --index-url https://download.pytorch.org/whl/cu124
```

（Linux / macOS 把 `^` 换成 `\`。约 2.4 GB，装完**重开终端**再检查。）

再次 `--check-gpu` 应显示 `✅ 可以用 GPU`。然后：

```bash
python -m vision_assist.cli --serve --source mjpeg:http://192.168.4.1/stream \
    --target-fps 30 --height 1.2 --pitch 30
```

**注意**：`--target-fps` 默认是 15，不提高的话 GPU 再快也用不上（会被主动限速）。
板子出 24 fps，所以设 30 就能跑满。

`--device` 可以强制指定：`--device cpu` / `--device cuda:0`；默认 `auto`（有 GPU 就用）。

> GPU 只加速**模型推理**。颜色掩码、LSD 纹理、地面反投影、JPEG 解码仍然在 CPU 上，
> 所以不会无限快——实测瓶颈会从"模型"转移到"CPU 后处理"。



- WiFi 信号弱：AP 模式下把电脑靠近板子
- 降 `jpeg_quality`（固件里，从 10 调到 12 可以减小单帧体积）
- 关掉手机上同时开着的板子页面，避免抢带宽



```
vision_assist/        识别包（几何、取帧、检测器、时序、状态机、播报、服务）
tools/                mock_stream（模拟 ESP32 流）、calibrate（标定）
web/index.html        可视化调试台
eval/                 run.py（评测）、baseline.py（参照方法复刻）、label_tool.py、labels.json
patches/              esp32_vga.patch（ESP32 固件补丁）+ 说明
tests/                190 个单元/集成测试
docs/设计.md          设计文档、实测记录、踩过的坑汇总
models/               blind_path.pt（盲道）、yolov8n.pt（COCO）
材料/                 原始参考材料
```

## 9. 测试

```bash
python -m unittest discover -s tests -t .        # 全量，约 14 秒
python -m unittest tests.test_geometry -v        # 单个模块
```

测试覆盖：地面反投影数学、MJPEG 解帧（含无 Content-Length 兜底）、配置往返、
砖型判别（含 8 个标注样本回归）、时序去抖、状态机滞回与符号约定、播报冷却与抢占、
斑马线/台阶/障碍物/红绿灯后处理、Scene schema、HTTP 端点。

## 10. 文档

- [`docs/设计.md`](docs/设计.md)：完整设计、实测数据、**踩过的坑汇总**（建议先看第 12 节）
- [`patches/README.md`](patches/README.md)：ESP32 固件改动
- [`eval/report.md`](eval/report.md)：最新一次评测报告
