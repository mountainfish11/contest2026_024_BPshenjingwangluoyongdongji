# blind_badge_app

BlindBadge 主板应用，运行在 ESP32-S3-EYE 的 openvela/NuttX 上。比赛仓通过
manifest 将本目录映射到：

```text
packages/demos/contest2026_024_blind_badge_app
```

## 启动与菜单

主板上电初始化 LCD、相机和通信基础设施，前台显示纵向轻量菜单。任务不会因为
菜单滚动而启动：

- MENU 单击：循环选择 `VISION -> VOICE -> HEALTH -> ALERT -> SETTING -> TEST`；
- PLAY 单击：进入当前项目并启动对应任务；
- PLAY 双击：无条件请求停止当前任务、释放任务资源并返回菜单；
- 菜单选中项由副板按索引播报，避免重复传输整段文字。

相机、LCD、副板初始化属于设备基础设施；视觉流、语音会话、健康采集和报警
属于互斥任务。退出任务时只释放任务占用，不反复拆建设备初始化资源。

## 服务

### VISION

进入后启动 HTTP 相机服务，状态页显示 `RUNNING`。接口：

```text
GET /health
GET /capture
GET /stream
```

MJPEG 只允许一个活动客户端。Windows GPU 服务和语音桥接见仓库根目录
`docs/vision_integration.md`。每条视觉播报同时触发副板一次非阻塞短振动。

### VOICE

主板向副板发送 VOICE 模式控制。副板 FY-SP701 使用 VAD 检测人声、流式上传
火山 ASR，并通过 `ASR_RESULT` 把文字发给主板；主板调用 ai_agent/MiMo，随后
把回答文本发回副板播报。会话支持多轮，退出时取消当前任务并恢复菜单播报模式。

### HEALTH

副板提示用户放置手指并读取 MAX30102；持续获得有效数据后播报心率和血氧
原型结果并结束采集。结果不用于医疗诊断。

### ALERT

主板触发副板 M100M 报警帧。Ubuntu 接收器确认后发送紧急联系人邮件，副板
播报报警已发送并触发三次非阻塞振动。报警动作不依赖 AI 返回。

### SETTING / TEST

预留扩展项。TEST 用于隔离诊断，不应占用其他正式任务资源。

## Wi-Fi 与凭证

当前稳定链路使用 STA：主板、副板和 Windows 位于同一 2.4 GHz Wi-Fi。
主板可从 MicroSD 的 `WIFI.CFG` 读取：

```text
SSID=<network name>
PASSWORD=<network password>
```

真实 Wi-Fi、MiMo、ASR 和 SMTP 凭证不得写入源码或提交到 Git。MiMo Token
Plan 使用 `token-plan-cn.xiaomimimo.com` 和项目支持的模型配置，调用时关闭
thinking，避免响应格式与内存开销干扰嵌入式解析。

## 主副板链路

主板监听 UDP `45678` 发现和 TCP `45679` 长连接。协议格式：

```text
UART_LINK/1 <TYPE> <SEQ> <LEN> <PAYLOAD>\n
```

消息包含握手/心跳、事件、命令、语音文本和 ACK。详细定义见仓库根目录
`protocol/blind_badge_protocol.md`。

## 常用诊断命令

```text
blind_badge_app status
blind_badge_app state
blind_badge_app button_status
blind_badge_app service_status
blind_badge_app play
blind_badge_app back
blind_badge_app vision_server_status
blind_badge_app vision_camera_test
blind_badge_app uart_link_start
blind_badge_app uart_link_send TEST_FLAG
```

这些命令用于诊断；正常演示通过实体 MENU/PLAY 按键完成。构建与公共仓依赖见
仓库根目录 `README.md` 和 `docs/openvela_workspace_dependencies.md`。
