#!/usr/bin/env python3
"""Configure the main board on USB connection or reboot; never log credentials."""

import argparse
import json
import logging
import os
from pathlib import Path
import stat
import time

import serial
from serial.tools import list_ports


class BoardSerial(serial.Serial):
    """Leave USB modem control lines untouched when opening the console."""

    def _update_dtr_state(self):
        pass

    def _update_rts_state(self):
        pass


def read_config(path):
    info = path.stat()
    if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077:
        raise ValueError("private configuration must be owned by you with mode 600")
    config = json.loads(path.read_text())
    for key in ("app_id", "token", "resource", "usb_serial"):
        value = config[key]
        if not isinstance(value, str) or not value or any(c.isspace() for c in value):
            raise ValueError("invalid configuration field: " + key)
    return config


def receive_prompt(link, timeout):
    deadline = time.monotonic() + timeout
    response = bytearray()
    while time.monotonic() < deadline:
        response.extend(link.read(1024))
        if b"nsh>" in response:
            return bytes(response)
        if len(response) > 32768:
            del response[:-16384]
    raise TimeoutError("shell prompt timeout")


def configure(link, config):
    # Allow startup tasks to settle before requesting the shell prompt.
    time.sleep(3)
    link.reset_input_buffer()
    link.write(b"\n")
    receive_prompt(link, 120)
    commands = [
        ("ai_agent set_volc_asr {app_id} {token} {resource}".format(**config),
         b"ASR credentials saved"),
        ("ai_agent set_voice_asr volcengine", b"ASR backend set to: volcengine"),
    ]
    for command, expected in commands:
        # Pace input for the embedded USB console; discard all echoed secrets.
        for byte in (command + "\n").encode():
            link.write(bytes([byte]))
            time.sleep(0.003)
        if expected not in receive_prompt(link, 30):
            raise RuntimeError("board did not confirm configuration")
    logging.info("ASR credentials and volcengine backend configured")


def log_diagnostics(data, config):
    for raw in data.splitlines():
        line = raw.decode("utf-8", "replace").strip()
        if line.startswith("[volc_asr]") or (
                line.startswith("[BlindBadge]") and "ASR failed" in line):
            logging.info("%s", line.replace(config["token"], "[redacted]"))
        elif line.startswith("[BlindBadge] asr_text:"):
            logging.info("Speech recognition succeeded")


def run(config, once):
    waiting = False
    while True:
        port = next((p.device for p in list_ports.comports()
                     if p.vid == 0x303A and p.pid == 0x1001
                     and (p.serial_number or "").upper() == config["usb_serial"].upper()), None)
        if not port:
            if not waiting:
                logging.info("Waiting for main board USB connection")
                waiting = True
            time.sleep(2)
            continue
        waiting = False
        try:
            # Even deasserting RTS/DTR during open can reset this USB console.
            link = BoardSerial(port=None, baudrate=115200, timeout=0.2,
                                 write_timeout=3, exclusive=True,
                                 rtscts=False, dsrdtr=False)
            link.dtr = False
            link.rts = False
            link.port = port
            link.open()
            with link:
                configure(link, config)
                if once:
                    return
                tail = b""
                pending = b""
                while True:
                    data = link.read(1024)
                    tail = (tail + data)[-2048:]
                    pending += data
                    if b"\n" in pending:
                        complete, pending = pending.rsplit(b"\n", 1)
                        log_diagnostics(complete, config)
                    pending = pending[-8192:]
                    if b"NuttShell (NSH)" in tail or b"ESP-ROM:" in tail:
                        logging.info("Main board reboot detected")
                        configure(link, config)
                        tail = b""
        except (serial.SerialException, OSError, TimeoutError, RuntimeError) as error:
            # Exception payloads and board output may contain echoed credentials.
            logging.warning("Serial configuration unavailable (%s); retrying", type(error).__name__)
            if once:
                raise SystemExit(1)
            time.sleep(3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    try:
        config = read_config(args.config)
    except (OSError, ValueError, KeyError):
        parser.exit(2, "Cannot load private ASR configuration; check fields and permissions.\n")
    run(config, args.once)


if __name__ == "__main__":
    main()
