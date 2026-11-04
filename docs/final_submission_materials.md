# 决赛提交索引

## 代码与复现

1. `README.md`：当前产品能力、目录和构建入口。
2. `docs/clean_reproduction_check.md`：干净工作区复现与验收门槛。
3. `docs/openvela_workspace_dependencies.md`：公共仓基线和补丁。
4. `patches/openvela/`：可审计的公共仓提交序列。
5. `firmware/esp32s3_sub_board/`：副板完整源码快照。
6. `vision/`：Windows GPU 视觉服务。
7. `protocol/blind_badge_protocol.md`：主副板消息协议。
8. `logs/`：官方格式 AI Coding 日志。

## 评委演示重点

- 主板 openvela 菜单负责服务生命周期，PLAY 双击强制退出当前任务。
- VISION：相机流、Windows CUDA 推理、语音提醒和非阻塞振动。
- VOICE：FY-SP701、VAD、流式火山 ASR、主板 MiMo、多轮播报。
- HEALTH：MAX30102 原型心率/血氧检测与播报。
- ALERT：M100M 4G 报警、邮件确认和三次振动。
- 服务之间有清晰边界，退出后释放任务资源，初始化资源除外。

## 安全边界

BlindBadge 是辅助提醒原型，不替代盲杖、导盲犬或医疗设备。心率/血氧结果仅
用于功能演示，不用于诊断。AI 失败时不能阻塞紧急报警等确定性安全动作。

## 决赛平台材料

平台填写与代码 PR 是两件事。平台材料包括：

- 答辩 PPT（必传，使用官方群公告模板）；
- 已合入的大赛专属 GitHub 仓库地址（必填）；
- 演示视频（建议）；
- 实物照片（建议）；
- 初赛后主要更新（选填）。

评审代码以截止时已经合入专属仓库的内容为准，fork 和未合并分支不计入。
