# openvela 公共仓依赖

BlindBadge 的 ESP32-S3-EYE 真机版本除了比赛仓主应用，还依赖三个 openvela
公共仓的修改。为保证评委可以从干净工作区复现，本仓保存了完整、按提交顺序
生成的 `git format-patch`，不依赖开发机上的未提交文件。

## 基线与最终提交

| 工作区路径 | 补丁基线 | 本项目验证版本 | 补丁数 |
|---|---|---|---:|
| `nuttx/` | `861540f7d52` | `792b2d2d68d` | 9 |
| `packages/ai_agent/` | `964117f` | `c463b96` | 6 |
| `apps/` | `c90b4e6c5` | `3fed86b66` | 1 |

这些基线对应整理提交时工作区的 `openvela/trunk`。若上游基线已经变化，脚本会
停止并提示人工检查，不会静默覆盖上游代码。

## 补丁内容

```text
patches/openvela/nuttx/               ESP32-S3-EYE 摄像头、Wi-Fi、I2S、SDMMC、板级配置
patches/openvela/packages_ai_agent/   MiMo、TLS/网络控制、语音与 ASR 支持
patches/openvela/apps/                NxRecorder 采集接口
```

## 应用方法

在比赛仓目录运行：

```bash
./tools/apply_openvela_patches.sh
```

脚本对每个仓执行以下安全检查：

1. 目标仓存在且工作树干净；
2. 当前 HEAD 等于记录的基线，或已经包含最终提交；
3. 只在满足条件时运行 `git am`；
4. 失败时保留 `git am` 状态供人工检查，不自动 reset。

## 为什么提交补丁

主板应用、视觉服务和副板源码都在比赛仓，但摄像头 V4L2、ESP32-S3-EYE
板级支持以及 ai_agent MiMo/TLS 能力属于公共仓。只提交应用会导致“代码可看、
硬件不可复现”。补丁用于本次作品复现；公共能力仍应按大赛规则分别向相应
openvela 公共仓提交 PR。

## 不包含的内容

- 编译目录、目标文件和固件缓存；
- 开发机绝对路径或串口设备绑定；
- Wi-Fi、ASR、MiMo、SMTP 等真实凭证；
- 副板目录中未纳入 Git 的 PDF 手册和构建缓存。
