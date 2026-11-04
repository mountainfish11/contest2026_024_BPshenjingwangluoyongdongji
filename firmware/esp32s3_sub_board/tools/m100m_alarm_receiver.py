#!/usr/bin/env python3
"""Receive BlindBadge alarm frames from the remote M100M USB UART."""

import argparse
import getpass
import json
import os
import smtplib
import ssl
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from email.message import EmailMessage
from pathlib import Path
from typing import Optional

import serial


DEFAULT_EMAIL_CONFIG = Path.home() / ".config/blindbadge/alert.json"


@dataclass(frozen=True)
class Alarm:
    sequence: int
    alarm_type: str
    gps_valid: bool
    latitude: float
    longitude: float
    gps_time: str
    gps_date: str
    health_available: bool = False
    finger_present: bool = False
    heart_rate_valid: bool = False
    heart_rate_bpm: Optional[float] = None
    spo2_valid: bool = False
    spo2_percent: Optional[float] = None


def parse_alarm(line: str) -> Optional[Alarm]:
    fields = line.strip().split("|")
    if len(fields) not in (9, 15) or fields[0:2] != ["BB1", "ALARM"]:
        return None
    try:
        sequence = int(fields[2])
        gps_flag = int(fields[4])
        latitude = float(fields[5])
        longitude = float(fields[6])
    except ValueError:
        return None
    if sequence < 0 or gps_flag not in (0, 1):
        return None

    health_available = False
    finger_present = False
    heart_rate_valid = False
    heart_rate_bpm = None
    spo2_valid = False
    spo2_percent = None
    if len(fields) == 15:
        try:
            health_flag = int(fields[9])
            finger_flag = int(fields[10])
            heart_rate_flag = int(fields[11])
            parsed_heart_rate = float(fields[12])
            spo2_flag = int(fields[13])
            parsed_spo2 = float(fields[14])
        except ValueError:
            return None
        if any(flag not in (0, 1) for flag in (
                health_flag, finger_flag, heart_rate_flag, spo2_flag)):
            return None
        health_available = bool(health_flag)
        finger_present = bool(finger_flag)
        heart_rate_valid = bool(heart_rate_flag)
        spo2_valid = bool(spo2_flag)
        if heart_rate_valid:
            heart_rate_bpm = parsed_heart_rate
        if spo2_valid:
            spo2_percent = parsed_spo2

    return Alarm(
        sequence=sequence,
        alarm_type=fields[3],
        gps_valid=bool(gps_flag),
        latitude=latitude,
        longitude=longitude,
        gps_time=fields[7],
        gps_date=fields[8],
        health_available=health_available,
        finger_present=finger_present,
        heart_rate_valid=heart_rate_valid,
        heart_rate_bpm=heart_rate_bpm,
        spo2_valid=spo2_valid,
        spo2_percent=spo2_percent,
    )


def append_log(path: Path, alarm: Alarm, email_status: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    record = asdict(alarm)
    record["received_at"] = datetime.now(timezone.utc).isoformat()
    record["email_status"] = email_status
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, ensure_ascii=False) + "\n")


def send_qq_email(alarm: Alarm) -> None:
    user = os.environ.get("BLINDBADGE_SMTP_USER", "").strip()
    auth_code = os.environ.get("BLINDBADGE_SMTP_AUTH_CODE", "").strip()
    recipient = os.environ.get("BLINDBADGE_ALERT_TO", user).strip()
    if not user or not auth_code or not recipient:
        raise RuntimeError(
            "BLINDBADGE_SMTP_USER, BLINDBADGE_SMTP_AUTH_CODE and "
            "BLINDBADGE_ALERT_TO must be configured"
        )

    location = "GPS尚未定位"
    if alarm.gps_valid:
        location = (
            f"纬度：{alarm.latitude:.7f}\n"
            f"经度：{alarm.longitude:.7f}\n"
            "地图：https://maps.google.com/?q="
            f"{alarm.latitude:.7f},{alarm.longitude:.7f}"
        )

    health = "心率血氧数据：未随报警提供"
    if alarm.health_available:
        if not alarm.finger_present:
            health = "心率血氧数据：传感器正常，但未检测到手指"
        else:
            heart_rate = "无有效读数"
            if alarm.heart_rate_valid and alarm.heart_rate_bpm is not None:
                heart_rate = f"{alarm.heart_rate_bpm:.1f} BPM"
            spo2 = "无有效读数"
            if alarm.spo2_valid and alarm.spo2_percent is not None:
                spo2 = f"{alarm.spo2_percent:.1f}%"
            health = f"心率：{heart_rate}\n血氧饱和度：{spo2}"

    message = EmailMessage()
    message["Subject"] = f"BlindBadge 紧急报警 #{alarm.sequence}"
    message["From"] = user
    message["To"] = recipient
    message.set_content(
        "BlindBadge收到紧急报警。\n\n"
        f"报警序号：{alarm.sequence}\n"
        f"报警类型：{alarm.alarm_type}\n"
        f"{location}\n"
        f"GPS UTC时间：{alarm.gps_time or '未知'}\n"
        f"GPS UTC日期：{alarm.gps_date or '未知'}\n"
        f"{health}\n"
    )

    context = ssl.create_default_context()
    with smtplib.SMTP_SSL("smtp.qq.com", 465, timeout=15,
                          context=context) as smtp:
        smtp.login(user, auth_code)
        smtp.send_message(message)


def load_email_config(path: Path) -> None:
    if not path.is_file():
        return

    with path.open("r", encoding="utf-8") as stream:
        document = json.load(stream)
    if not isinstance(document, dict):
        raise ValueError("email configuration must be a JSON object")

    values = {
        "BLINDBADGE_SMTP_USER": document.get("smtp_user"),
        "BLINDBADGE_ALERT_TO": document.get("alert_to"),
    }
    for name, value in values.items():
        if value is not None and not isinstance(value, str):
            raise ValueError(f"{name} must be a string")
        if value and not os.environ.get(name, "").strip():
            os.environ[name] = value.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log", type=Path,
                        default=Path("out/m100m_alerts.jsonl"))
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--email", action="store_true",
                        help="send an email through QQ SMTP")
    parser.add_argument("--email-config", type=Path,
                        default=DEFAULT_EMAIL_CONFIG,
                        help="JSON file containing smtp_user and alert_to")
    args = parser.parse_args()

    if not Path(args.port).exists():
        parser.error(
            f"serial port does not exist: {args.port}; connect the M100M "
            "USB receiver to Ubuntu first"
        )

    if args.email:
        try:
            load_email_config(args.email_config)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            parser.error(f"cannot load email configuration: {exc}")
        smtp_user = os.environ.get("BLINDBADGE_SMTP_USER", "").strip()
        if not smtp_user:
            parser.error(
                "BLINDBADGE_SMTP_USER is not configured in the environment "
                f"or {args.email_config}"
            )
        if not os.environ.get("BLINDBADGE_ALERT_TO", "").strip():
            os.environ["BLINDBADGE_ALERT_TO"] = smtp_user
        if not os.environ.get("BLINDBADGE_SMTP_AUTH_CODE", "").strip():
            auth_code = getpass.getpass("QQ邮箱SMTP授权码: ").strip()
            if not auth_code:
                parser.error("QQ邮箱SMTP授权码不能为空")
            os.environ["BLINDBADGE_SMTP_AUTH_CODE"] = auth_code

    print(f"[M100M] listening on {args.port} at {args.baud} baud")
    with serial.Serial(args.port, args.baud, timeout=1) as connection:
        while True:
            raw = connection.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            alarm = parse_alarm(line)
            if alarm is None:
                print(f"[M100M] ignored: {line}")
                continue

            print(
                f"[ALARM] seq={alarm.sequence} type={alarm.alarm_type} "
                f"gps_valid={alarm.gps_valid} "
                f"lat={alarm.latitude:.7f} lon={alarm.longitude:.7f} "
                f"hr={alarm.heart_rate_bpm} spo2={alarm.spo2_percent}"
            )
            email_status = "disabled"
            ack_status = "OK"
            if args.email:
                try:
                    send_qq_email(alarm)
                    email_status = "sent"
                    print(f"[EMAIL] sent for seq={alarm.sequence}")
                except Exception as exc:
                    email_status = f"failed:{type(exc).__name__}"
                    ack_status = "EMAIL_FAIL"
                    print(f"[EMAIL] failed for seq={alarm.sequence}: {exc}")

            append_log(args.log, alarm, email_status)
            ack = (
                f"BB1|ACK|{alarm.sequence}|{ack_status}\r\n"
            ).encode("ascii")
            connection.write(ack)
            connection.flush()
            print(f"[M100M] ACK sent for seq={alarm.sequence}")
            if args.once:
                return 0


if __name__ == "__main__":
    raise SystemExit(main())
