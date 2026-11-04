# Clean Reproduction Check

本页是决赛提交前和评委复现时使用的检查清单。它区分静态检查、构建成功和
真机端到端验证，不把其中任何一项冒充另一项。

## 1. 仓库映射

manifest `contest2026_024_BPshenjingwangluoyongdongji.xml` 必须包含：

```xml
<linkfile src="app/blind_badge_app"
          dest="packages/demos/contest2026_024_blind_badge_app"/>
```

## 2. 公共仓补丁

```bash
cd contest2026_024_BPshenjingwangluoyongdongji
./tools/apply_openvela_patches.sh
```

成功标准：三个仓均显示已应用或已经包含目标提交。脚本不允许在脏工作树上
继续。基线与补丁数量见 `docs/openvela_workspace_dependencies.md`。

## 3. 主板构建

```bash
cd <openvela-workspace>
PATH=/tmp/blindbadge-tools:$PWD/prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf/bin:$PATH \
  ./build.sh esp32s3-eye:blindbadge -j8
```

成功标准：生成 `nuttx/nuttx.bin`。Simple Boot 烧录偏移为 `0x0000`；烧录前
核对主板 MAC `A4:CB:8F:E1:E0:80`，不得烧入副板。

## 4. 副板构建

```bash
cd contest2026_024_BPshenjingwangluoyongdongji/firmware/esp32s3_sub_board
idf.py set-target esp32s3
idf.py build
```

成功标准：ESP-IDF 构建完成。真实外设验证还需要按副板 README 接线，构建成功
本身不证明 FY-SP701、MAX30102、M100M 或振动电机已工作。

## 5. 视觉主机静态测试

```bash
cd vision
python -m pytest tests
python -m vision_assist.cli --check-gpu
```

GPU、摄像头和网络端到端验证按 `docs/vision_integration.md` 执行。

## 6. 凭证检查

仓库不得包含以下真实值：

- Wi-Fi SSID 对应的密码；
- MiMo Token Plan Key；
- 火山 ASR App ID、Token 或 Resource ID；
- QQ 邮箱密码或 SMTP 授权码；
- GitHub/API 私人令牌。

源码只允许空值或 `<...>` 占位符。提交前应进行模式扫描，但扫描命令本身不要
包含或打印真实凭证。

## 7. AI Coding 日志

```bash
python3 ../.claude/skills/contest-log-collector/tools/validate-log.py logs/
```

成功标准：`ALL OK`。禁止修改 JSONL 内的事件；含凭证的日志必须整段 session
排除。

## 8. Git 提交卫生

```bash
git diff --check
git status --short
git ls-files | grep -E '(^|/)(build|__pycache__)/' && exit 1 || true
```

提交中不得出现编译目录、缓存、串口抓取临时文件、PDF 数据手册副本、私有配置
或未说明的大文件。

## 9. 真机验收边界

最终真机演示至少逐项确认：

1. 菜单与 PLAY 强制退出；
2. VISION 图像、GPU 推理、语音与短振动；
3. VOICE 多轮 ASR -> MiMo -> 播报；
4. HEALTH 有效手指数据与播报；
5. ALERT 邮件发送与三次振动；
6. 每个任务退出后，下一个任务仍能独占所需资源。

历史运行记录不能代替提交当天的真机验证。
