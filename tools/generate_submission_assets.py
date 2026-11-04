#!/usr/bin/env python3
"""Generate BlindBadge submission PDF and MP4 assets.

The workspace does not depend on external office/video tools. This script uses
Pillow, ReportLab, and OpenCV, which are available in the current environment.
"""

from pathlib import Path
import textwrap

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from reportlab.lib.pagesizes import landscape, A4
from reportlab.pdfgen import canvas


REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "submission"
FRAMES = OUT / "frames"

W, H = 1280, 720
FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_BOLD = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"


def font(size, bold=False, mono=False):
    path = MONO if mono else (FONT_BOLD if bold else FONT)
    return ImageFont.truetype(path, size)


F = {
    "title": font(54, bold=True),
    "subtitle": font(28),
    "h": font(34, bold=True),
    "body": font(25),
    "small": font(20),
    "mono": font(20, mono=True),
    "mono_small": font(17, mono=True),
}


def draw_wrapped(draw, text, xy, fnt, fill, width, line_gap=8):
    x, y = xy
    def measure(s):
        return draw.textsize(s, font=fnt)[0]

    for para in text.split("\n"):
      if not para:
        y += fnt.size + line_gap
        continue
      lines = []
      line = ""
      for ch in para:
        trial = line + ch
        if measure(trial) <= width:
          line = trial
        else:
          if line:
            lines.append(line)
          line = ch
      if line:
        lines.append(line)
      for line in lines:
        draw.text((x, y), line, font=fnt, fill=fill)
        y += fnt.size + line_gap
    return y


def terminal(draw, x, y, w, h, lines, title="openvela nsh"):
    draw.rectangle((x, y, x + w, y + h), fill=(18, 24, 32), outline=(70, 92, 118))
    draw.rectangle((x, y, x + w, y + 44), fill=(35, 45, 58))
    draw.text((x + 18, y + 11), title, font=F["small"], fill=(214, 226, 240))
    yy = y + 62
    for line in lines:
        fill = (170, 232, 180) if line.startswith("$") or line.startswith(">") else (228, 236, 242)
        draw.text((x + 22, yy), line[:92], font=F["mono_small"], fill=fill)
        yy += 27
        if yy > y + h - 30:
            break


def lcd(draw, x, y, scale=1.0, status="WiFi OK", body=None):
    size = int(250 * scale)
    draw.rectangle((x, y, x + size, y + size), fill=(12, 28, 42), outline=(80, 120, 140))
    draw.rectangle((x + 18, y + 18, x + size - 18, y + size - 18), fill=(5, 18, 28))
    draw.text((x + 34, y + 36), "BlindBadge", font=font(int(24 * scale), bold=True), fill=(235, 248, 255))
    color = (80, 230, 130) if "OK" in status else (248, 92, 92)
    draw.text((x + 34, y + 76), status, font=font(int(20 * scale), bold=True), fill=color)
    body = body or ["DANGER / AI", "前方40厘米有障碍", "请先停下确认"]
    yy = y + 120
    for line in body:
        draw.text((x + 34, yy), line, font=font(int(18 * scale), bold=True), fill=(255, 230, 160))
        yy += int(36 * scale)


def base(title, subtitle=None):
    img = Image.new("RGB", (W, H), (242, 246, 250))
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W, 96), fill=(20, 42, 68))
    d.text((54, 26), title, font=F["h"], fill=(255, 255, 255))
    if subtitle:
        d.text((54, 100), subtitle, font=F["subtitle"], fill=(45, 64, 84))
    d.text((1050, 34), "openvela 2026", font=F["small"], fill=(196, 215, 232))
    return img, d


SLIDES = []


def add_slide(name, title, subtitle, body=None, term=None, lcd_body=None, foot=None):
    img, d = base(title, subtitle)
    if body:
        y = 170
        for heading, text in body:
            d.text((64, y), heading, font=F["h"], fill=(20, 42, 68))
            y += 46
            y = draw_wrapped(d, text, (76, y), F["body"], (32, 44, 58), 560, 8) + 22
    if term:
        terminal(d, 680, 152, 540, 425, term)
    if lcd_body:
        lcd(d, 890, 340, 1.05, body=lcd_body)
    if foot:
        d.text((64, 660), foot, font=F["small"], fill=(72, 88, 104))
    SLIDES.append((name, img))


add_slide(
    "01_title",
    "BlindBadge 盲人辅助胸牌",
    "基于 openvela + ai_agent + MiMo 的 AI 硬件安全提醒原型",
    body=[
        ("作品定位", "面向盲人和低视力用户的可穿戴安全提醒胸牌。它把障碍、台阶、紧急求助等事件转换成短句提醒，并保留本地安全 fallback。"),
        ("边界声明", "这不是成熟导航设备，不替代手杖、导盲犬或专业辅助设备；它验证的是端侧事件处理、AI 提醒和执行动作闭环。"),
    ],
    lcd_body=["READY", "WiFi OK", "安全提醒就绪"],
    foot="提交材料：作品介绍 PDF + 5 分钟内 MP4 演示视频",
)

add_slide(
    "02_user_story",
    "用户故事与核心价值",
    "低打扰、短提醒、失败可控",
    body=[
        ("场景", "用户佩戴 BlindBadge 行走。设备发现前方障碍逐渐接近、下行台阶或紧急求助事件。"),
        ("输出", "设备只给一句行动明确的中文提醒，例如“前方40厘米有障碍，请先停下确认”。"),
        ("安全原则", "AI 可以优化表达，但本地规则永远能给出安全提醒；紧急求助不依赖 AI。"),
    ],
    term=[
        "$ blind_badge_app obstacle 40 front --ai",
        "[BlindBadge] event: obstacle_near",
        "[BlindBadge] distance_cm: 40",
        "[BlindBadge] suggestion: 前方40厘米有障碍，请立即停下确认。",
        "[BlindBadge] ai_response: 前方40厘米有障碍，请先停下确认。",
        "[BlindBadge] action.vibration: strong",
    ],
)

add_slide(
    "03_arch",
    "系统架构",
    "事件 -> 策略 -> AI -> fallback -> 执行动作",
    body=[
        ("端侧闭环", "QEMU 阶段用命令模拟传感器事件；硬件阶段在 ESP32-S3-EYE 上通过 LCD、Wi-Fi、I2S/串口验证同一条产品链路。"),
        ("模块", "blind_badge_event 负责事件，policy 负责风险阈值，ai 模块调用 ai_agent/MiMo，action 输出语音、震动和紧急发送动作。"),
    ],
    term=[
        "hazard event",
        "  -> local policy / severity",
        "  -> ai_prompt",
        "  -> ai_agent / MiMo",
        "  -> fallback guard",
        "  -> voice / vibration / emergency_send",
    ],
)

add_slide(
    "04_openvela",
    "openvela 能力使用",
    "NSH 应用、QEMU、ESP32-S3-EYE、网络与显示",
    body=[
        ("应用接入", "比赛仓库通过 manifest linkfile 将 app/blind_badge_app 映射到 packages/demos/contest2026_024_blind_badge_app。"),
        ("运行目标", "QEMU 使用 goldfish-arm64-v8a-ap；硬件目标是 ESP32-S3-EYE，使用 Wi-Fi、framebuffer LCD、I2S 麦克风验证路径。"),
    ],
    term=[
        "$ ./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j2",
        "build completed successfully",
        "$ ./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap -read-only -no-window",
        "goldfish-armv8a-ap>",
    ],
)

add_slide(
    "05_skill",
    "AI Agent / Skill",
    "自定义 BlindBadge Safety Reminder Skill",
    body=[
        ("Skill 规则", "限制 AI 只输出一句中文安全提醒，不长篇解释，不承诺绝对安全。不同事件有不同动作要求。"),
        ("运行时安装", "blind_badge_app install_skill 会把 Skill 写入 /data/ai_agent/skills/blind-badge.md，ai_agent 可通过 /skill 看到它。"),
    ],
    term=[
        "$ blind_badge_app install_skill",
        "[BlindBadge] skill_installed: /data/ai_agent/skills/blind-badge.md",
        "$ echo ask /skill | ai_agent",
        "[Agent]: BlindBadge Safety Reminder Skill",
    ],
)

add_slide(
    "06_qemu",
    "QEMU 演示结果",
    "主动风险接近、台阶、紧急求助",
    body=[
        ("演示命令", "blind_badge_app demo 和 demo --ai 展示障碍从 120cm 接近到 80cm、40cm，随后检测下行台阶并触发 emergency。"),
        ("结果", "每个事件都有 event、suggestion、ai_prompt、ai_response、severity 和 action 输出。"),
    ],
    term=[
        "$ blind_badge_app demo --ai",
        "[BlindBadge] demo: proactive_hazard_alert",
        "[BlindBadge] demo_stage: obstacle warning threshold 80cm",
        "[BlindBadge] ai_response: 前方80厘米有障碍，请减速并绕行。",
        "[BlindBadge] demo_stage: urgent obstacle threshold 40cm",
        "[BlindBadge] ai_response: 前方40厘米有障碍，请先停下确认。",
        "[BlindBadge] demo_stage: emergency help message",
        "[BlindBadge] action.emergency_send: triggered",
        "[BlindBadge] demo: completed",
    ],
)

add_slide(
    "07_hardware",
    "ESP32-S3-EYE 硬件验证",
    "真实板卡启动、Wi-Fi/MiMo、LCD 显示路径",
    body=[
        ("已验证", "构建/烧录 PASS，Wi-Fi/DNS PASS，ai_agent ask hi 返回真实回复，BlindBadge obstacle/emergency/demo --ai PASS。"),
        ("LCD", "framebuffer /dev/fb0 显示 BlindBadge 状态、WiFi OK/ERROR、AI 或 fallback 中文提醒。"),
    ],
    term=[
        "ESP32-S3-EYE boot -> nsh>",
        "Wi-Fi/DNS -> ai_agent direct commands",
        "ai_agent ask hi -> real MiMo reply",
        "blind_badge_app obstacle 40 front --ai --lcd",
        "[BlindBadge] lcd: updated 240x240 bpp=16",
    ],
    lcd_body=["DANGER / AI", "前方40厘米有障碍", "请先停下确认"],
)

add_slide(
    "08_fallback",
    "安全 fallback 与执行动作",
    "AI 失败时仍保持产品闭环",
    body=[
        ("fallback", "没有 MiMo、网络超时、AI 空回复时，应用打印 fallback_reason，并把本地安全提醒作为最终 ai_response。"),
        ("执行动作", "voice、vibration、emergency_send 会继续运行；emergency 强制触发强震动和求助动作。"),
    ],
    term=[
        "$ blind_badge_app emergency --ai",
        "[BlindBadge] fallback_response: 我需要帮助，请联系我或前往我的当前位置。",
        "[BlindBadge] severity: emergency",
        "[BlindBadge] action.voice: 我需要帮助，请联系我或前往我的当前位置。",
        "[BlindBadge] action.vibration: strong",
        "[BlindBadge] action.emergency_send: triggered",
        "[BlindBadge] fallback: active",
    ],
)

add_slide(
    "09_assets",
    "提交材料状态",
    "代码、AI Coding logs、硬件依赖说明已进入 PR",
    body=[
        ("代码仓库", "BlindBadge 源码、README、Skill、QEMU/硬件验证文档已推送到 mountainfish11 fork，并同步官方 PR。"),
        ("日志与依赖", "logs/mountainfish11 下包含 3 个真实 Codex 会话 JSONL；openvela_workspace_dependencies.md 列出 nuttx、ai_agent、apps 的硬件复现依赖。"),
    ],
    term=[
        "latest commit: 9d32b45",
        "docs: add AI coding logs and workspace dependencies",
        "logs/mountainfish11/manifest.json",
        "docs/openvela_workspace_dependencies.md",
    ],
)

add_slide(
    "10_close",
    "总结",
    "BlindBadge 展示的是一个安全优先的 AI 硬件闭环",
    body=[
        ("创新点", "不是普通聊天机器人，而是把 openvela 端侧事件、ai_agent Skill、MiMo 回复、本地 fallback 和执行动作串成硬件产品流程。"),
        ("下一步", "把模拟事件替换成摄像头/ToF/超声波/IMU 输入，把 action.voice 接入 TTS，把 emergency_send 接入手机伴侣或 MQTT 网关。"),
    ],
    lcd_body=["DEMO DONE", "AI + fallback", "安全优先"],
)


def save_frames():
    FRAMES.mkdir(parents=True, exist_ok=True)
    paths = []
    for idx, (name, img) in enumerate(SLIDES, 1):
        path = FRAMES / f"{idx:02d}_{name}.png"
        img.save(path)
        paths.append(path)
    return paths


def make_pdf(paths):
    pdf = OUT / "BlindBadge_work_introduction.pdf"
    c = canvas.Canvas(str(pdf), pagesize=landscape(A4))
    page_w, page_h = landscape(A4)
    for path in paths:
        c.drawImage(str(path), 0, 0, width=page_w, height=page_h)
        c.showPage()
    c.save()
    return pdf


def make_video(paths):
    video = OUT / "BlindBadge_demo_video.mp4"
    fps = 5
    seconds_per_slide = [14, 16, 16, 16, 16, 20, 20, 18, 14, 14]
    writer = cv2.VideoWriter(str(video), cv2.VideoWriter_fourcc(*"mp4v"), fps, (W, H))
    for path, seconds in zip(paths, seconds_per_slide):
        frame = cv2.cvtColor(np.array(Image.open(path).convert("RGB")), cv2.COLOR_RGB2BGR)
        total = seconds * fps
        for i in range(total):
            f = frame.copy()
            progress = int((i + 1) * W / total)
            cv2.rectangle(f, (0, H - 8), (progress, H), (72, 158, 255), -1)
            writer.write(f)
    writer.release()
    return video


def make_script():
    script = OUT / "BlindBadge_demo_video_script.md"
    script.write_text(
        """# BlindBadge Demo Video Script

Generated companion script for `BlindBadge_demo_video.mp4`.

Duration: about 2 minutes 44 seconds.

1. Opening: BlindBadge is an AI assistive badge for blind and low-vision users. It turns hazard events into short safety reminders and keeps local fallback.
2. User story: nearby obstacle, downward step, and emergency help are converted into one action-oriented reminder.
3. Architecture: event -> policy -> ai_prompt -> ai_agent/MiMo -> fallback -> voice/vibration/emergency_send.
4. openvela: QEMU demo plus ESP32-S3-EYE hardware validation use openvela NSH app, Wi-Fi, LCD, and ai_agent.
5. Skill: BlindBadge Safety Reminder Skill constrains AI to one short Chinese safety sentence.
6. QEMU: demo --ai shows proactive obstacle stages, step_down, and emergency.
7. Hardware: ESP32-S3-EYE validation records Wi-Fi/DNS, real MiMo reply, LCD update, and BlindBadge AI actions.
8. Fallback: AI/network failure still returns local safe wording and keeps actions running.
9. Submission: code, logs, and workspace dependency notes are in the PR.
10. Closing: BlindBadge is a safety-first AI hardware loop, not a general chatbot.
""",
        encoding="utf-8",
    )
    return script


def main():
    OUT.mkdir(exist_ok=True)
    paths = save_frames()
    pdf = make_pdf(paths)
    video = make_video(paths)
    script = make_script()
    for path in [pdf, video, script]:
        print(path)


if __name__ == "__main__":
    main()
