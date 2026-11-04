"""本地 HTTP 服务 + 可视化调试页（仅用 Python 标准库）。

端点
----
``GET  /``                调试页面（web/index.html）
``GET  /stream.mjpg``     带叠加的 MJPEG 实时画面
``GET  /events``          SSE 推送逐帧 Scene JSON
``GET  /api/scene``       最近一帧 Scene JSON（轮询兜底）
``GET  /snapshot.jpg``    当前带叠加的单帧
``GET  /health``          运行状态
``POST /source``          切换帧源           {"source": "mjpeg:http://ip/stream"}
``POST /config``          改标定/阈值        {"camera": {"pitch_deg": 30}, ...}
``POST /reset``           清空时序与播报状态

设计取舍
--------
叠加层画在**服务端**（Python），而不是浏览器 canvas：这样任何客户端拿到的画面
都是同一份，不用重复实现绘制逻辑。代价是每帧多一次 ``cv2.imencode``（约 3–5 ms）。
想省这点开销就用 ``/stream.mjpg?overlay=0`` 拿原始画面。
"""
from __future__ import annotations

import json
import os
import socket
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Dict, Optional
from urllib.parse import parse_qs, urlparse

import cv2
import numpy as np

from .config import PROJECT_ROOT, WEB_DIR, AppConfig
from .pipeline import Pipeline
from .schema import SCHEMA_VERSION, validate_scene
from .sources import FrameSource, LatestFrameSource, open_source

#: 把最新帧留给多少秒前算"过期"
FRAME_STALE_S = 2.0


class VisionService:
    """跑在后台线程里的识别服务，对外暴露"最新一帧"。"""

    def __init__(self, config: Optional[AppConfig] = None, source: Optional[str] = None,
                 width_limit: int = 960, pipeline_kwargs: Optional[dict] = None):
        self.config = config or AppConfig()
        self.source_spec = source or self.config.source
        self.width_limit = int(width_limit)

        self._pipeline = Pipeline(self.config, **(pipeline_kwargs or {}))
        self._source: Optional[FrameSource] = None
        self._thread: Optional[threading.Thread] = None
        self.running = False
        self._lock = threading.Lock()
        self._new_frame = threading.Condition(self._lock)
        self._subscribers: list = []

        self._latest_jpeg: Optional[bytes] = None
        self._latest_scene: Optional[dict] = None
        self._last_display_frame = None
        self._raw_jpeg_cache = (0, None)
        self._frame_seq = 0
        self._frame_ts = 0.0
        self._events: list = []          # 最近的播报/警报记录
        self._errors: list = []
        self._fps_ema: Optional[float] = None
        self._last_frame_time: Optional[float] = None
        self._frames = 0
        #: 抽取线程跑到的序号；用来避免重复处理同一帧
        self._last_source_seq = -1
        #: 因为处理不过来而被跳过的帧数（"丢包"感的主要来源）
        self._skipped = 0
        self._source_info_cache: dict = {}
        self._source_info_age = 0

    # ------------------------------------------------------------------ 生命周期
    def start(self) -> "VisionService":
        self._open_source(self.source_spec)
        self.running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self.running = False
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None
        if self._source is not None:
            self._source.close()
            self._source = None
        try:
            self._pipeline.close()
        except Exception:  # noqa: BLE001
            pass

    def __enter__(self) -> "VisionService":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # ------------------------------------------------------------------ 帧源
    def _open_source(self, spec: str) -> None:
        if self._source is not None:
            try:
                self._source.close()      # LatestFrameSource.close 会停线程并关内层
            except Exception:  # noqa: BLE001
                pass
        # 用后台抽帧包装：永远把流读空，绝不因处理慢而阻塞 ESP32
        self._source = LatestFrameSource(open_source(spec)).start()
        self.source_spec = spec
        self._last_source_seq = -1
        self._skipped = 0
        self._source_info_cache = {}
        # 卡流看门狗：用来把"无新帧"翻译成一句人能看懂的话
        self._last_watchdog = 0.0
        self._last_rx_count = 0
        self._last_rx_change = time.time()
        self._stall_reason: Optional[str] = None
        self._pipeline.reset()

    def _update_stall_watchdog(self, now: float) -> None:
        """每秒检查一次"有没有收到帧"，并给出可执行的原因。

        板子的 WebServer **一次只服务一个流客户端**。浏览器标签页开着板子页面时，
        视觉服务连上去一个字节都收不到，界面只会显示"无新帧"——用户完全无从下手。
        这里把这种情况识别出来，直接说明原因和解法。
        """
        if now - self._last_watchdog < 1.0:
            return
        self._last_watchdog = now
        source = self._source
        if source is None:
            return

        received = getattr(source, "received", 0)
        if received != self._last_rx_count:
            self._last_rx_count = received
            self._last_rx_change = now
            self._stall_reason = None
            return

        stalled = now - self._last_rx_change
        if stalled < 4.0:
            return

        errors = getattr(source, "read_errors", 0)
        info = self._source_info_cache or {}
        stuck_parts = info.get("reconnects", 0)
        if received == 0:
            if stuck_parts and stuck_parts >= 2:
                self._stall_reason = (
                    f"已重连 {stuck_parts} 次仍收不到任何帧。两种可能："
                    "① 浏览器标签页/别的终端还占着这个流（板子一次只服务一个流客户端）；"
                    "② 板子卡在推流循环里（旧固件），需要断电重上电"
                )
            else:
                self._stall_reason = (
                    "连上了但一帧都没收到。最常见原因是**浏览器标签页还开着板子页面**"
                    "（板子一次只服务一个流客户端）——关掉它，或给板子断电重上电"
                )
        else:
            self._stall_reason = (
                f"流已中断 {stalled:.0f} 秒（之前收过 {received} 帧，读流错误 {errors} 次）。"
                "多半是 WiFi 抖动或板子重启了"
            )

    def switch_source(self, spec: str) -> dict:
        try:
            self._open_source(spec)
            return {"ok": True, "source": spec}
        except Exception as exc:  # noqa: BLE001
            self._record_error(f"切换帧源失败: {exc}")
            return {"ok": False, "error": str(exc)}

    # ------------------------------------------------------------------ 主循环
    def _loop(self) -> None:
        target_fps = float(getattr(self.config, "target_fps", 0.0) or 0.0)
        min_interval = (1.0 / target_fps) if target_fps > 0 else 0.0
        last_process = 0.0

        while self.running:
            source = self._source
            if source is None:
                time.sleep(0.05)
                continue

            # 阻塞等新帧（条件变量），绝不要用 sleep 轮询：
            # Windows 的 sleep 粒度是 15.6 ms，轮询式等帧每次白等 15 毫秒，
            # 实测会把 19 fps 的流处理成 4 fps。
            frame, seq, _frame_ts = source.wait_for_frame(self._last_source_seq,
                                                         timeout=0.2)
            if frame is None:
                time.sleep(0.01)
                continue

            # 超时了但还没有新帧：回去继续等，别重复处理同一张
            if seq == self._last_source_seq:
                continue

            now = time.time()
            # 主动限速：宁可跳过帧，也不要让处理长时间落后于采集
            if min_interval and (now - last_process) < min_interval:
                time.sleep(min(0.008, max(0.001, min_interval - (now - last_process))))
                continue

            skipped = max(0, seq - self._last_source_seq - 1)
            self._skipped += skipped
            self._last_source_seq = seq
            last_process = now

            # 帧源信息不用每帧都取（LocalVideoSource.info 会查 4 次 CAP_PROP）
            self._source_info_age += 1
            if not self._source_info_cache or self._source_info_age >= 30:
                self._source_info_age = 0
                try:
                    self._source_info_cache = source.info
                except Exception:  # noqa: BLE001
                    self._source_info_cache = {}

            try:
                scene = self._pipeline.process(
                    frame, self._frames, self._source_info_cache, now)
            except Exception as exc:  # noqa: BLE001
                self._record_error(f"处理异常: {exc}")
                traceback.print_exc()
                time.sleep(0.2)
                continue
            self._frames += 1

            if self._last_frame_time is not None:
                inst = 1.0 / max(1e-6, now - self._last_frame_time)
                self._fps_ema = inst if self._fps_ema is None else 0.9 * self._fps_ema + 0.1 * inst
            self._last_frame_time = now
            scene.fps = self._fps_ema

            # 画面按宽度压缩后再编码，降低 CPU 与带宽
            display = frame
            if self.width_limit and frame.shape[1] > self.width_limit:
                scale = self.width_limit / float(frame.shape[1])
                display = cv2.resize(frame, None, fx=scale, fy=scale,
                                     interpolation=cv2.INTER_AREA)
            annotated = self._pipeline.annotate(display, scene)
            ok, buf = cv2.imencode(".jpg", annotated, [cv2.IMWRITE_JPEG_QUALITY, 80])
            if not ok:
                continue

            data = scene.to_dict()
            problems = validate_scene(data)
            if problems:
                data["schema_problems"] = problems

            with self._lock:
                self._latest_jpeg = buf.tobytes()
                self._latest_scene = data
                self._last_display_frame = display
                self._frame_seq += 1
                self._frame_ts = now
                self._update_events_locked(scene, now)
                self._new_frame.notify_all()

    def _update_events_locked(self, scene, now: float) -> None:
        speech = getattr(scene, "speech", None)
        if speech is not None and speech.spoke and speech.text:
            self._events.append({
                "ts": round(now, 3),
                "kind": "speech",
                "event": speech.event,
                "text": speech.text,
                "priority": speech.priority,
            })
        if len(self._events) > 200:
            del self._events[: len(self._events) - 200]

    def _record_error(self, message: str) -> None:
        with self._lock:
            self._errors.append({"ts": round(time.time(), 3), "message": message})
            if len(self._errors) > 50:
                del self._errors[: len(self._errors) - 50]

    # ------------------------------------------------------------------ 读取
    def snapshot(self, overlay: bool = True) -> Optional[bytes]:
        """取当前帧的 JPEG。原始画面按需编码，不做每帧预编码（省 3–5 ms/帧）。"""
        if overlay:
            with self._lock:
                return self._latest_jpeg

        with self._lock:
            seq, cached = self._raw_jpeg_cache
            if cached is not None and seq == self._frame_seq:
                return cached
            frame = self._last_display_frame
            current_seq = self._frame_seq

        if frame is None:
            return None
        ok, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
        if not ok:
            return None
        data = buf.tobytes()
        with self._lock:
            self._raw_jpeg_cache = (current_seq, data)
        return data

    def wait_frame(self, after_seq: int, timeout: float = 1.0):
        """等到有新帧，返回 ``(seq, jpeg, scene)``。"""
        with self._lock:
            if self._frame_seq <= after_seq:
                self._new_frame.wait(timeout)
            return self._frame_seq, self._latest_jpeg, self._latest_scene

    def scene(self) -> Optional[dict]:
        with self._lock:
            return self._latest_scene

    def health(self) -> dict:
        with self._lock:
            age = (time.time() - self._frame_ts) if self._frame_ts else None
            source = self._source
            source_fps = getattr(source, "source_fps", None) if source is not None else None
            received = getattr(source, "received", 0) if source is not None else 0
            read_errors = getattr(source, "read_errors", 0) if source is not None else 0
            stream_kbps = getattr(source, "kbps", None) if source is not None else None
            bytes_received = getattr(source, "bytes_received", 0) if source is not None else 0
            process_fps = self._fps_ema
            skipped = self._skipped
            return {
                "ok": self.running and (age is not None and age < FRAME_STALE_S),
                "running": self.running,
                "source": self.source_spec,
                "source_info": source.info if source is not None else None,
                "frames": self._frames,
                "frame_seq": self._frame_seq,
                "frame_age_s": None if age is None else round(age, 2),
                "fps": None if process_fps is None else round(process_fps, 1),
                # 这两个是排查"是谁慢"的关键：
                #   source_fps  >>  fps         -> 处理端是瓶颈（降 imgsz / 降分辨率）
                #   source_fps  ≈   fps 且都低  -> 采集端/网络是瓶颈
                "source_fps": None if source_fps is None else round(source_fps, 1),
                "stream_kbps": None if stream_kbps is None else round(stream_kbps, 1),
                "frames_received": received,
                "bytes_received": bytes_received,
                "frames_processed": self._frames,
                "frames_skipped": skipped,
                "reader_errors": read_errors,
                "device": self._pipeline.device,
                "device_description": self._pipeline.device_description,
                "schema_version": SCHEMA_VERSION,
                "errors": list(self._errors[-5:]),
                "camera": self._pipeline.camera.describe() if self._pipeline.camera else None,
            }

    def recent_events(self) -> list:
        with self._lock:
            return list(self._events[-80:])

    # ------------------------------------------------------------------ 配置
    def apply_config(self, patch: dict) -> dict:
        """把 JSON 补丁合并进配置并就地生效（不重新加载模型）。"""
        merged = self.config.to_dict()
        _deep_merge(merged, patch or {})
        new_config = AppConfig.from_dict(merged)
        with self._lock:
            self.config = new_config
            self._pipeline.apply_config(new_config)
        return {"ok": True, "config": new_config.to_dict()}

    def save_config(self, target: str) -> dict:
        """把当前配置写到文件（默认项目根目录下的 calibration.json）。

        只允许写到项目目录内，避免被当成任意文件写入。
        """
        path = Path(target)
        if not path.is_absolute():
            path = Path(PROJECT_ROOT) / path
        try:
            path.resolve().relative_to(Path(PROJECT_ROOT).resolve())
        except ValueError:
            return {"ok": False, "error": f"只允许写到项目目录内: {PROJECT_ROOT}"}
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with self._lock:
                config = self.config
            config.save(path)
        except OSError as exc:
            return {"ok": False, "error": f"写文件失败: {exc}"}
        return {"ok": True, "path": str(path), "config": config.to_dict()}


def _deep_merge(base: dict, patch: dict) -> dict:
    for key, value in patch.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value
    return base


# --------------------------------------------------------------------------- #
# HTTP 层
# --------------------------------------------------------------------------- #
class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VisionAssist/0.1"

    def log_message(self, fmt, *args) -> None:  # 安静，避免刷屏
        pass

    @property
    def service(self) -> VisionService:
        return self.server.service  # type: ignore[attr-defined]

    # ------------------------------------------------------------------ 辅助
    def _json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _bytes(self, body: bytes, ctype: str, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            return {}

    # ------------------------------------------------------------------ 路由
    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        if path in ("/", "/index.html"):
            self._serve_index()
        elif path == "/stream.mjpg":
            overlay = (query.get("overlay", ["1"])[0] or "1") not in ("0", "false")
            self._stream(overlay)
        elif path == "/events":
            self._sse()
        elif path == "/api/scene":
            scene = self.service.scene()
            self._json(scene or {"ok": False, "error": "还没有帧"})
        elif path == "/health":
            self._json(self.service.health())
        elif path == "/snapshot.jpg":
            overlay = (query.get("overlay", ["1"])[0] or "1") not in ("0", "false")
            data = self.service.snapshot(overlay)
            if data is None:
                self._json({"ok": False, "error": "还没有帧"}, status=503)
            else:
                self._bytes(data, "image/jpeg")
        elif path == "/api/events":
            self._json({"events": self.service.recent_events()})
        elif path == "/api/config":
            self._json(self.service.config.to_dict())
        else:
            self._json({"ok": False, "error": "not found"}, status=404)

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/source":
            body = self._read_json()
            spec = (body.get("source") or "").strip()
            if not spec:
                self._json({"ok": False, "error": "缺少 source 字段"}, status=400)
                return
            self._json(self.service.switch_source(spec))
        elif path == "/config":
            body = self._read_json()
            self._json(self.service.apply_config(body))
        elif path == "/config/save":
            body = self._read_json()
            self._json(self.service.save_config(body.get("path") or "calibration.json"))
        elif path == "/reset":
            self.service._pipeline.reset()
            self._json({"ok": True})
        else:
            self._json({"ok": False, "error": "not found"}, status=404)

    # ------------------------------------------------------------------ 实现
    def _serve_index(self) -> None:
        index = Path(WEB_DIR) / "index.html"
        if not index.is_file():
            self._bytes(b"web/index.html not found", "text/plain; charset=utf-8", status=500)
            return
        self._bytes(index.read_bytes(), "text/html; charset=utf-8")

    def _stream(self, overlay: bool) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()

        seq = 0
        try:
            while self.server.alive:  # type: ignore[attr-defined]
                seq, jpeg, _scene = self.service.wait_frame(seq, timeout=1.0)
                if jpeg is None:
                    continue
                data = self.service.snapshot(overlay)
                if data is None:
                    continue
                self.wfile.write(b"--frame\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(b"Content-Length: " + str(len(data)).encode() + b"\r\n\r\n")
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            pass

    def _sse(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()

        # 先补发最近的事件，再进入逐帧推送
        try:
            for event in self.service.recent_events():
                self.wfile.write(b"event: speech\n")
                self.wfile.write(b"data: " + json.dumps(event, ensure_ascii=False).encode() + b"\n\n")
            self.wfile.flush()
        except (BrokenPipeError, OSError):
            return

        seq = 0
        last_scene = None
        try:
            while self.server.alive:  # type: ignore[attr-defined]
                seq, _jpeg, scene = self.service.wait_frame(seq, timeout=1.0)
                if scene is None or scene is last_scene:
                    continue
                last_scene = scene
                payload = json.dumps(scene, ensure_ascii=False).encode()
                self.wfile.write(b"data: " + payload + b"\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            pass


class VisionHttpServer(ThreadingHTTPServer):
    daemon_threads = True
    # Windows 上 SO_REUSEADDR 允许**第二个进程绑定同一个端口**（端口劫持），
    # 于是浏览器可能连到几小时前那个"死服务"上，表现为"页面永远无新帧"。
    # 关掉它，让重复绑定直接报错，我们才能检测并换端口。
    allow_reuse_address = (os.name != "nt")

    def __init__(self, address, service: VisionService):
        super().__init__(address, _Handler)
        self.service = service
        self.alive = True

    def shutdown(self) -> None:  # noqa: D102
        self.alive = False
        super().shutdown()


def port_in_use(host: str, port: int, timeout: float = 0.3) -> bool:
    """端口上是否已有服务在应答。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        return sock.connect_ex((host, port)) == 0
    except OSError:
        return False
    finally:
        sock.close()


def serve(service: VisionService, host: str = "127.0.0.1", port: int = 8760,
          auto_port: bool = True) -> VisionHttpServer:
    """起 HTTP 服务。端口被占用时自动顺延，并打印实际使用的地址。

    没有这一步的话，用户很容易在多个终端里各起一个服务，浏览器连到的是旧的
    那个（源还是错的 IP），界面永远显示"无新帧"，而命令行看起来一切正常。
    """
    candidates = [port] + [port + i for i in range(1, 11)] if auto_port else [port]
    last_error: Optional[OSError] = None
    for candidate in candidates:
        if auto_port and candidate != port and port_in_use(host, candidate):
            continue
        if candidate == port and port_in_use(host, candidate):
            print(f"[vision] ⚠️  端口 {port} 已有服务在运行"
                  f"（很可能是之前没关掉的实例），自动换到下一个空闲端口")
        try:
            server = VisionHttpServer((host, candidate), service)
        except OSError as exc:
            last_error = exc
            continue
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        if candidate != port:
            print(f"[vision] 实际监听端口: {candidate}")
        return server
    raise RuntimeError(
        f"没有可用端口（尝试过 {candidates[0]}–{candidates[-1]}）。"
        f"最后一次错误: {last_error}")
