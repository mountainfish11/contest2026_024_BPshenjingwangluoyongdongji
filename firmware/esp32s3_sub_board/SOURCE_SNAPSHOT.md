# Source Snapshot

此目录从 BlindBadge 副板独立 ESP-IDF 工程的 Git 提交导出：

```text
commit: a433c75
subject: fix: stabilize MAX30102 health measurements
```

只导出了该提交跟踪的源码和配置文件。原工程中的构建目录、PDF 数据手册和
未跟踪调试资料没有进入比赛仓。

本快照已在 ESP32-S3 副板真机验证：MAX30102 可稳定枚举，空载不会误判手指，
手指稳定覆盖后的测试窗口输出约 77–82 BPM、95%–99% SpO2，且无 FIFO 溢出或
I2C 错误。该读数仅用于原型功能验证，不作为医疗诊断依据。
