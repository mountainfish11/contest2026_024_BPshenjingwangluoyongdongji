"""TCP bridge for RGB565 frames captured by the OpenVela main board.

Wire format (network byte order)::

    request  = b"BBV1" + seq:u32 + width:u16 + height:u16
               + format:u32 + payload_len:u32 + payload
    response = json_len:u32 + compact_scene_json

Format 1 is little-endian RGB565.  A connection may carry multiple frames.
"""
from __future__ import annotations

import argparse
import json
import os
import socketserver
import struct
import threading
import time
from typing import Any

import numpy as np

from .config import AppConfig, CameraCalib
from .pipeline import Pipeline

MAGIC = b"BBV1"
PIXEL_RGB565_LE = 1
HEADER = struct.Struct("!4sIHHII")
LENGTH = struct.Struct("!I")
MAX_FRAME_BYTES = 640 * 480 * 2


def decode_rgb565(payload: bytes, width: int, height: int) -> np.ndarray:
    """Convert little-endian RGB565 bytes to an OpenCV BGR image."""
    expected = width * height * 2
    if len(payload) != expected:
        raise ValueError(f"RGB565 length {len(payload)} != {expected}")

    pixels = np.frombuffer(payload, dtype="<u2").reshape(height, width)
    red = ((pixels >> 11) & 0x1f).astype(np.uint16)
    green = ((pixels >> 5) & 0x3f).astype(np.uint16)
    blue = (pixels & 0x1f).astype(np.uint16)
    bgr = np.empty((height, width, 3), dtype=np.uint8)
    bgr[..., 0] = ((blue * 255 + 15) // 31).astype(np.uint8)
    bgr[..., 1] = ((green * 255 + 31) // 63).astype(np.uint8)
    bgr[..., 2] = ((red * 255 + 15) // 31).astype(np.uint8)
    return bgr


def compact_scene(scene: Any, seq: int) -> dict:
    """Return only fields consumed by the constrained OpenVela client."""
    data = scene.to_dict()
    speech = data.get("speech") or {}
    tracker = data.get("tracker") or {}
    alerts = (data.get("alerts") or {}).get("alerts") or []
    alert = alerts[0] if alerts else {}
    return {
        "ok": True,
        "seq": seq,
        "state": data.get("state") or "UNKNOWN",
        "speech": speech.get("text") or "",
        "event": speech.get("event") or "",
        "path_present": bool(tracker.get("present", False)),
        "path_lost": bool(tracker.get("lost", False)),
        "offset_m": tracker.get("offset_m"),
        "heading_deg": tracker.get("heading_deg"),
        "alert": alert.get("kind") or "",
        "alert_distance_m": alert.get("distance_m"),
        "alert_detail": alert.get("detail") or "",
        "latency_ms": (data.get("latency_ms") or {}).get("total"),
    }


class VisionRuntime:
    def __init__(self, config: AppConfig):
        self.pipeline = Pipeline(config)
        self.lock = threading.Lock()

    def process(self, payload: bytes, seq: int, width: int, height: int,
                pixel_format: int) -> dict:
        if pixel_format != PIXEL_RGB565_LE:
            raise ValueError(f"unsupported pixel format {pixel_format}")
        frame = decode_rgb565(payload, width, height)
        source = {
            "type": "blindbadge-openvela",
            "format": "rgb565le",
            "size": [width, height],
        }
        with self.lock:
            scene = self.pipeline.process(frame, seq, source, time.time())
        return compact_scene(scene, seq)


def _read_exact(stream, length: int) -> bytes:
    data = stream.read(length)
    if data is None or len(data) != length:
        raise EOFError
    return data


class VisionBridgeHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        runtime: VisionRuntime = self.server.runtime  # type: ignore[attr-defined]
        while True:
            try:
                raw = _read_exact(self.rfile, HEADER.size)
            except EOFError:
                return

            magic, seq, width, height, pixel_format, payload_len = HEADER.unpack(raw)
            if magic != MAGIC:
                self._reply({"ok": False, "error": "bad magic"})
                return
            if (width <= 0 or height <= 0 or payload_len <= 0 or
                    payload_len > MAX_FRAME_BYTES or
                    payload_len != width * height * 2):
                self._reply({"ok": False, "error": "invalid frame size"})
                return

            try:
                payload = _read_exact(self.rfile, payload_len)
                result = runtime.process(payload, seq, width, height,
                                         pixel_format)
            except EOFError:
                return
            except Exception as exc:  # keep the device protocol alive
                result = {"ok": False, "seq": seq, "error": str(exc)[:160]}
            self._reply(result)

    def _reply(self, result: dict) -> None:
        encoded = json.dumps(result, ensure_ascii=False,
                             separators=(",", ":")).encode("utf-8")
        self.wfile.write(LENGTH.pack(len(encoded)))
        self.wfile.write(encoded)
        self.wfile.flush()


class VisionBridgeServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, runtime: VisionRuntime):
        self.runtime = runtime
        super().__init__(address, VisionBridgeHandler)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="BlindBadge OpenVela vision bridge")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=45681)
    parser.add_argument("--config", default=None)
    parser.add_argument("--height", type=float, default=None)
    parser.add_argument("--pitch", type=float, default=None)
    parser.add_argument("--hfov", type=float, default=None)
    parser.add_argument("--device", default=None)
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    os.environ.setdefault("YOLO_CONFIG_DIR", "/tmp/blindbadge-yolo")
    cfg = AppConfig.load(args.config)
    camera = cfg.camera.to_dict()
    for key, value in (("height_m", args.height),
                       ("pitch_deg", args.pitch),
                       ("hfov_deg", args.hfov)):
        if value is not None:
            camera[key] = value
    cfg.camera = CameraCalib(**camera)
    if args.device:
        cfg.device = args.device

    runtime = VisionRuntime(cfg)
    server = VisionBridgeServer((args.host, args.port), runtime)
    print(f"[vision-bridge] listening on {args.host}:{args.port}", flush=True)
    print(f"[vision-bridge] inference: {runtime.pipeline.device_description}",
          flush=True)
    print("[vision-bridge] input: QVGA RGB565 from BlindBadge", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
