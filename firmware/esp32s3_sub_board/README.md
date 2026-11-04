# ESP32-S3 Sub Board

ESP32-S3副板工程，用于与主控板(ESP32-S3-EYE)进行WiFi通信。

## 工程结构

```text
esp32s3_sub_board/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
├── build.ps1
├── main/
│   ├── CMakeLists.txt
│   ├── main.c
│   └── Kconfig.projbuild
└── components/
    ├── wifi/
    ├── atgm336h/
    ├── m100m/
    ├── max30102/
    ├── speech_link/
    ├── speech_output/
    ├── uart_link/
    ├── vibration_motor/
    └── volc_asr/
```

## 构建

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

在 `Sub Board Configuration` 中配置 Wi-Fi SSID 和密码，主板和副板应加入同一个 2.4 GHz 网络。

## 当前功能

- Wi-Fi STA 连接、掉线自动重连和连接状态查询
- UART_LINK UDP 发现、TCP 握手、ACK、心跳和自动重连
- 障碍、下台阶和紧急事件发送到主板
- 接收主板控制标志并回复 ACK
- UART0 和原生 USB Serial/JTAG 调试命令入口
- ATGM336H GNSS（UART1，GPIO4/5，PPS GPIO6）
- M100M 4G 串口透传（UART2，RX GPIO15，TX GPIO16）
- MAX30102 心率/血氧原型检测（I2C GPIO8/9，INT GPIO10）
- FY-SP701 USB Audio 麦克风和扬声器（USB D-/D+ GPIO19/20）
- VAD、火山 ASR 与主板 VOICE 文本链路
- 1027 振动电机非阻塞驱动（GPIO11 仅驱动外部 MOSFET/三极管）
- 菜单播报、视觉提醒、健康检测和紧急报警的独立任务状态

事件联调命令：

```text
status
obstacle 40 front 95
step_down front 90
emergency
gps_status
m100_status
m100_send BB|TEST|001
m100_alarm
max30102_status
pcm_status
speech_status
speech_test <Chinese text>
speech_recognize [1-4 seconds]
asr_status
```

M100M 的串口参数默认是 115200、8N1、无流控，必须与银尔达小程序中的
串口配置一致。模块必须使用独立的 3.3V～4.2V 大电流电源，推荐 3.8V、
至少 3A；不能由 ESP32-S3 或 USB 转串口的 3.3V 输出供电。

`emergency` 会同时尝试现有主板链路和 M100M 4G链路。仅验证4G报警时可使用
`m100_alarm`，其消息格式为：

```text
BB1|ALARM|序号|EMERGENCY|GPS有效标志|纬度|经度|UTC时间|UTC日期|MAX30102可用|检测到手指|心率有效|心率BPM|血氧有效|血氧百分比
```

报警触发时会读取 MAX30102 的最新快照。没有检测到手指或估算尚未稳定时，
对应有效标志为 `0`，接收器会在邮件中明确显示“无有效读数”，不会把 `0`
误报为实际心率或血氧。Ubuntu 接收器仍兼容不含心率血氧字段的旧版9字段报警帧。

Ubuntu端接收与ACK：

```bash
python3 tools/m100m_alarm_receiver.py --port /dev/ttyUSB0 --once
```

QQ邮箱报警由Ubuntu接收端发送，不依赖银尔达小程序的“报警设备”列表。先在
QQ邮箱中开启SMTP并生成授权码，然后只在当前终端设置环境变量：

```bash
export BLINDBADGE_SMTP_USER='你的完整QQ邮箱地址'
export BLINDBADGE_ALERT_TO='报警接收邮箱地址'
read -rsp 'QQ邮箱SMTP授权码: ' BLINDBADGE_SMTP_AUTH_CODE
export BLINDBADGE_SMTP_AUTH_CODE
python3 tools/m100m_alarm_receiver.py --port /dev/ttyUSB0 --email
```

不要把QQ登录密码或SMTP授权码写入源码、命令历史、日志或提交到Git。

### MAX30102 心率血氧模块

MAX30102 模块使用 3.3V 供电，并按以下方式连接：

```text
模块 VIN -> 3.3V
模块 GND -> GND
模块 SDA -> GPIO8
模块 SCL -> GPIO9
模块 INT -> GPIO10
```

驱动使用 I2C 地址 `0x57`，启动时会校验 `PART_ID=0x15`。串口输入
`max30102_status` 可查看芯片ID、RED/IR原始值、采样数、FIFO与I2C错误、
手指状态以及估算的心率和血氧。估算值仅用于原型功能验证，不可作为医疗
诊断数据。

### FY-SP701 与语音

FY-SP701 使用外部 5V 供电并与副板共地，USB D- 连接 GPIO19、D+ 连接
GPIO20。模块枚举后应同时出现 16 kHz 单声道扬声器和 16 kHz 双声道麦克风。
ASR 凭证在本地配置，不能提交到仓库。

### 1027 振动电机

GPIO11 只能连接 MOSFET/三极管驱动级输入，不能直接给电机供电。电机红线接
受控正电源，黑线接驱动回路；电机电源地、驱动地和副板 GND 必须共地，并应
配置续流保护。视觉提醒触发一次短振动，紧急报警发送后触发三次振动；驱动使用
非阻塞定时状态机，不占用语音或网络任务。

外设驱动接入时调用 `UART_LINK_SendObstacle()`、
`UART_LINK_SendStepDown()` 或 `UART_LINK_SendEmergency()`。主板下发的
`VIBRATION_OFF`、`VIBRATION_SHORT`、`VIBRATION_STRONG` 和
`EMERGENCY_SEND` 会进入 `UART_LINK_SetFlagHandler()` 注册的回调。

双方日志出现以下内容才表示应用层链路已打通：

```text
主板: [UART_LINK] TCP peer connected
副板: UART_LINK: connected
主板: [UART_LINK] RX type=PING ...
```

## 许可证

MIT License
