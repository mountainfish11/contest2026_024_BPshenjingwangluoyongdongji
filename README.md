# BlindBadge

BlindBadge 是面向盲人和低视力用户的双板式 AI 辅助胸牌原型。主板运行
openvela/NuttX，负责菜单、摄像头、任务调度和 AI 对话；副板运行 ESP-IDF，
负责语音输入输出、健康传感器、GNSS、4G 报警和振动反馈。Windows 主机使用
GPU 完成视觉推理，并把简短提醒发送给副板播报。

> 本项目是竞赛原型，不是医疗器械，也不能替代盲杖、导盲犬或专业辅助设备。

## 已实现链路

- `VISION`：ESP32-S3-EYE MJPEG -> Windows GPU 视觉识别 -> 副板语音与短振动提醒。
- `VOICE`：FY-SP701 麦克风 -> VAD/火山 ASR -> 主板 MiMo -> 副板语音播报。
- `HEALTH`：MAX30102 心率/血氧原型检测与结果播报。
- `ALERT`：副板 M100M 4G 模块触发紧急报警，Ubuntu 接收端发送邮件并三次振动。
- 菜单：MENU 循环选择，PLAY 单击进入任务、双击强制退出；任务退出时释放任务资源。
- 其他外设：ATGM336H GNSS、FY-SP701 USB Audio、1027 振动电机。

## 仓库结构

```text
app/blind_badge_app/           openvela 主板应用
firmware/esp32s3_sub_board/    ESP-IDF 副板完整源码快照
vision/                        Windows GPU 视觉服务与测试工具
skills/blind-badge/            BlindBadge ai_agent Skill
patches/openvela/              nuttx/ai_agent/apps 公共仓补丁
protocol/                      主副板协议
docs/                          架构、验证和复现资料
logs/                          官方格式 AI Coding 日志
submission/                    已有演示材料
```

## 获取工程

```bash
repo init -u https://github.com/open-vela/contest2026_024_BPshenjingwangluoyongdongji \
  -b dev-ai-contest-2026 \
  -m contest2026_024_BPshenjingwangluoyongdongji.xml
repo sync -c -j8
```

比赛仓中的主板应用由 manifest 映射到：

```text
packages/demos/contest2026_024_blind_badge_app
```

## 应用硬件补丁

真机版本依赖 `nuttx`、`packages/ai_agent` 和 `apps` 的补丁。新工作区先执行：

```bash
cd contest2026_024_BPshenjingwangluoyongdongji
./tools/apply_openvela_patches.sh
```

脚本会检查公共仓基线，按顺序使用 `git am` 应用补丁；不会写入任何凭证。
补丁范围和基线见 [docs/openvela_workspace_dependencies.md](docs/openvela_workspace_dependencies.md)。

## 构建主板

在 openvela 工作区根目录执行：

```bash
PATH=/tmp/blindbadge-tools:$PWD/prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf/bin:$PATH \
  ./build.sh esp32s3-eye:blindbadge -j8
```

输出为 `nuttx/nuttx.bin`。该配置使用 Simple Boot，主板镜像烧录偏移为
`0x0000`，不是 `0x10000`。烧录前必须确认目标是主板 MAC
`A4:CB:8F:E1:E0:80`，不要把主板镜像烧入副板。

## 构建副板

```bash
cd contest2026_024_BPshenjingwangluoyongdongji/firmware/esp32s3_sub_board
idf.py set-target esp32s3
idf.py build
```

副板为 ESP32-S3N16R8，详细接线、配置和验证命令见
[firmware/esp32s3_sub_board/README.md](firmware/esp32s3_sub_board/README.md)。

## Windows GPU 视觉服务

```powershell
$visionRoot = "<仓库路径>\vision"
Set-Location $visionRoot

& "$visionRoot\.venv-cuda\Scripts\python.exe" -u -m vision_assist.cli `
  --serve `
  --source "mjpeg:http://<主板IP>/stream" `
  --host "127.0.0.1" `
  --port 8760 `
  --device "cuda:0" `
  --performance-profile balanced `
  --target-fps 30 `
  --height 1.20 `
  --pitch 30.0 `
  --width-limit 0
```

调试页面：`http://127.0.0.1:8760/`。语音桥接命令见 `vision/README.md`。

## 凭证安全

仓库不包含 Wi-Fi 密码、MiMo Token Plan Key、火山 ASR 凭证、邮箱密码或
SMTP 授权码。凭证必须在设备运行时、本地环境变量或不纳入 Git 的本地配置中
提供。请勿把真实凭证写入源码、提交信息或演示日志。

## 复现与验证

- [docs/clean_reproduction_check.md](docs/clean_reproduction_check.md)
- [docs/openvela_workspace_dependencies.md](docs/openvela_workspace_dependencies.md)
- [docs/vision_integration.md](docs/vision_integration.md)
- [protocol/blind_badge_protocol.md](protocol/blind_badge_protocol.md)
- [tests/qemu_manual_test.md](tests/qemu_manual_test.md)

QEMU 演示仍可用于无硬件检查；最终真机能力以双板与 Windows 主机链路为准。
