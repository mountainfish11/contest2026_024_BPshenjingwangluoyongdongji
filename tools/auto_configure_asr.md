# 主板 ASR 自动配置

`auto_configure_asr.py` 在电脑上运行，通过 USB 串口向指定主板发送
`ai_agent set_volc_asr` 和 `ai_agent set_voice_asr volcengine`。
检测到主板重新连接或重启时重新配置，不依赖网络连接。

依赖 Python 3 和 pyserial。私有 JSON 配置包含字符串字段 `app_id`、
`token`、`resource`、`usb_serial`，必须属于当前用户且权限为 600。
凭据不放入此仓库、固件或日志。主板由 USB VID/PID 和唯一序列号匹配。

可选字段 `dns_server` 为 IPv4 DNS 地址。需要支持 `ai_agent set_dns`
的固件；该命令只修改 DNS，不重新配置无线网卡地址。应填写当前网络
实际可用的 DNS，切换热点后需要核查此字段。

当前电脑的私有配置位于工作空间根目录 `.blindbadge-local/asr.json`，
用户服务为 `blindbadge-asr.service`，登录电脑后自动启动。
该功能需要电脑运行并连接主板；不等于主板脱离电脑后能够永久保存凭据。

```bash
systemctl --user status blindbadge-asr.service
journalctl --user -u blindbadge-asr.service -n 20 --no-pager
```

脚本常驻时占用主板串口。手动串口调试和烧录前停止服务，完成后再启动：

```bash
systemctl --user stop blindbadge-asr.service
systemctl --user start blindbadge-asr.service
```

单次配置（先停止服务）：

```bash
python3 tools/auto_configure_asr.py \
  --config <openvela-workspace>/.blindbadge-local/asr.json --once
```

串口配置失败只记录异常类型，不输出主板原始回显，避免暴露凭据。
打开串口时不改变 RTS/DTR，避免触发主板复位。常驻日志记录火山连接
阶段和 ASR 错误，隐藏凭据和识别文本，可通过上述 journalctl 命令查看。
电脑上其他串口程序也必须停止读取该串口，否则可能干扰命令和响应。
