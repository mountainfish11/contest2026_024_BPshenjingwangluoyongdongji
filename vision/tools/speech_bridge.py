"""Forward visual speech events to the ESP32-S3 sub-board.

The vision service stays unchanged.  This bridge polls ``/api/events`` and
delivers only new speech events over the acknowledged BBSP/1 TCP protocol.
"""

from __future__ import annotations

import argparse
from collections import deque
import json
import socket
import time
import urllib.error
import urllib.request
import zlib


DEFAULT_VISION_URL = "http://127.0.0.1:8760"
DEFAULT_SPEECH_PORT = 45682
MAX_TEXT_BYTES = 480
SUCCESS_ACKS = {"QUEUED", "DUPLICATE"}


def event_key(event: dict) -> tuple:
    return (event.get("ts"), event.get("event"), event.get("text"))


def encode_speech_frame(sequence: int, priority: int, text: str) -> bytes:
    payload = text.encode("utf-8")
    if not (1 <= len(payload) <= MAX_TEXT_BYTES):
        raise ValueError(f"UTF-8 text must be 1..{MAX_TEXT_BYTES} bytes")
    if not (0 <= priority <= 100):
        raise ValueError("priority must be 0..100")
    if sequence <= 0:
        raise ValueError("sequence must be positive")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = (
        f"BBSP/1 SPEAK {sequence} {priority} {len(payload)} {crc:08x}\n"
    ).encode("ascii")
    return header + payload


def parse_ack(line: bytes, expected_sequence: int) -> str:
    fields = line.decode("ascii", errors="strict").strip().split()
    if len(fields) != 4 or fields[:2] != ["BBSP/1", "ACK"]:
        raise ValueError(f"invalid ACK: {line!r}")
    if int(fields[2]) != expected_sequence:
        raise ValueError(f"ACK sequence mismatch: {fields[2]}")
    return fields[3]


def send_speech(
    host: str,
    port: int,
    sequence: int,
    priority: int,
    text: str,
    timeout: float = 4.0,
) -> str:
    frame = encode_speech_frame(sequence, priority, text)
    with socket.create_connection((host, port), timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall(frame)
        reply = bytearray()
        while len(reply) < 128:
            byte = connection.recv(1)
            if not byte:
                break
            reply.extend(byte)
            if byte == b"\n":
                break
    if not reply.endswith(b"\n"):
        raise ConnectionError("sub-board closed without a complete ACK")
    return parse_ack(bytes(reply), sequence)


def fetch_events(vision_url: str, timeout: float = 3.0) -> list[dict]:
    url = vision_url.rstrip("/") + "/api/events"
    request = urllib.request.Request(url, headers={"Connection": "close"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        document = json.load(response)
    events = document.get("events")
    if not isinstance(events, list):
        raise ValueError("vision /api/events did not return an events list")
    return [event for event in events if isinstance(event, dict)]


class SpeechBridge:
    def __init__(
        self,
        vision_url: str,
        sub_board_host: str,
        port: int = DEFAULT_SPEECH_PORT,
        replay_existing: bool = False,
    ) -> None:
        self.vision_url = vision_url
        self.sub_board_host = sub_board_host
        self.port = port
        self.replay_existing = replay_existing
        self._seen_order: deque[tuple] = deque(maxlen=256)
        self._seen: set[tuple] = set()
        self._pending_sequences: dict[tuple, int] = {}
        self._sequence = max(1, time.time_ns() & 0x7FFFFFFFFFFFFFFF)
        self._initialized = False

    def _remember(self, key: tuple) -> None:
        if len(self._seen_order) == self._seen_order.maxlen:
            self._seen.discard(self._seen_order[0])
        self._seen_order.append(key)
        self._seen.add(key)

    def poll_once(self) -> int:
        events = [
            event
            for event in fetch_events(self.vision_url)
            if event.get("kind") == "speech" and event.get("text")
        ]
        if not self._initialized and not self.replay_existing:
            for event in events:
                self._remember(event_key(event))
            self._initialized = True
            return 0
        self._initialized = True

        pending = [event for event in events if event_key(event) not in self._seen]
        pending.sort(key=lambda event: (-int(event.get("priority") or 0),
                                        float(event.get("ts") or 0)))
        delivered = 0
        for event in pending:
            key = event_key(event)
            text = str(event["text"])
            priority = max(0, min(100, int(event.get("priority") or 50)))
            sequence = self._pending_sequences.get(key)
            if sequence is None:
                self._sequence += 1
                sequence = self._sequence
                self._pending_sequences[key] = sequence
            status = send_speech(
                self.sub_board_host,
                self.port,
                sequence,
                priority,
                text,
            )
            if status not in SUCCESS_ACKS:
                raise RuntimeError(f"sub-board rejected speech: {status}")
            self._remember(key)
            self._pending_sequences.pop(key, None)
            delivered += 1
            print(
                f"[speech_bridge] {status}: seq={sequence} "
                f"priority={priority} text={text}",
                flush=True,
            )
        return delivered


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Forward BlindBadge visual speech to the sub-board"
    )
    parser.add_argument("--sub-board", required=True, help="sub-board IPv4 address")
    parser.add_argument("--port", type=int, default=DEFAULT_SPEECH_PORT)
    parser.add_argument("--vision-url", default=DEFAULT_VISION_URL)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--replay-existing", action="store_true")
    parser.add_argument("--once", metavar="TEXT", help="send one test sentence")
    parser.add_argument("--priority", type=int, default=50)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.once is not None:
        sequence = max(1, time.time_ns() & 0x7FFFFFFFFFFFFFFF)
        status = send_speech(
            args.sub_board, args.port, sequence, args.priority, args.once
        )
        print(f"[speech_bridge] {status}: {args.once}")
        return 0 if status in SUCCESS_ACKS else 2

    bridge = SpeechBridge(
        args.vision_url,
        args.sub_board,
        args.port,
        replay_existing=args.replay_existing,
    )
    print(
        f"[speech_bridge] vision={args.vision_url} "
        f"sub_board={args.sub_board}:{args.port}",
        flush=True,
    )
    while True:
        try:
            bridge.poll_once()
        except (OSError, ValueError, RuntimeError, urllib.error.URLError) as exc:
            print(f"[speech_bridge] retry after error: {exc}", flush=True)
        time.sleep(max(0.1, args.interval))


if __name__ == "__main__":
    raise SystemExit(main())
