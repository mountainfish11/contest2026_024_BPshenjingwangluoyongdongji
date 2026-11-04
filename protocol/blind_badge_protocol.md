# BlindBadge Board Protocol

ESP32-S3 子板通过 UDP 发现 ESP32-S3-EYE 主板，再使用 TCP 发送换行结尾的 ASCII 帧。

```text
UART_LINK/1 <TYPE> <SEQ> <PAYLOAD_LEN> <PAYLOAD>\n
```

- `SEQ`：发送方递增序号，各方向独立计数。
- `PAYLOAD_LEN`：payload 的字节数，不含换行。
- 所有方向仅允许 `front`、`left`、`right`。
- `confidence` 范围为 0 到 100。

## 连接帧

```text
UART_LINK/1 DISCOVER 0 2 OK
UART_LINK/1 DISCOVER_ACK 0 2 OK
UART_LINK/1 HELLO 1 15 SUB_BOARD_READY
UART_LINK/1 HELLO_ACK 1 2 OK
UART_LINK/1 PING 2 2 OK
UART_LINK/1 PONG 2 2 OK
```

UDP 发现端口为 `45678`，TCP 数据端口为 `45679`。

## 事件帧

障碍事件 payload 为 `<distance_cm> <direction> <confidence>`：

```text
UART_LINK/1 OBSTACLE 3 11 40 front 95
```

下台阶事件 payload 为 `<direction> <confidence>`：

```text
UART_LINK/1 STEP_DOWN 4 8 front 90
```

紧急事件 payload 为不含空格的事件来源：

```text
UART_LINK/1 EMERGENCY 5 7 console
```

主板接受事件后统一回复：

```text
UART_LINK/1 EVENT_ACK <SEQ> 2 OK
```

收到事件不依赖云端 AI。主板先执行本地安全策略，将本地提醒显示到 LCD，并输出对应动作；AI 仅是可选增强层。

## 主板控制帧

主板使用只含大写字母、数字和下划线的控制标志通知子板执行动作：

```text
UART_LINK/1 COMMAND 1 15 VIBRATION_SHORT
UART_LINK/1 COMMAND 2 16 VIBRATION_STRONG
UART_LINK/1 COMMAND 3 13 VIBRATION_OFF
UART_LINK/1 COMMAND 4 14 EMERGENCY_SEND
```

子板收到并接受控制标志后回复：

```text
UART_LINK/1 COMMAND_ACK <SEQ> 2 OK
```

子板通过 `UART_LINK_SetFlagHandler()` 注册执行回调。当前默认回调仅记录标志，后续外设驱动直接在该回调中分发，不需要修改连接、心跳或 ACK 逻辑。
