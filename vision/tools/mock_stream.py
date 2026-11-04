"""把本地视频模拟成 ESP32-S3-EYE 的 MJPEG 流，用于无板子时联调。

用法::

    python -m tools.mock_stream --video data/Test.mp4 --port 8099

    # 然后
    python -m vision_assist.cli --source mjpeg:http://127.0.0.1:8099/stream

故意把分片格式做成和 ``camera_stream_download.ino`` 一致::

    --f\\r\\n
    Content-Type: image/jpeg\\r\\n
    Content-Length: <N>\\r\\n
    \\r\\n
    <jpeg bytes>\\r\\n

``--no-length`` 可以去掉 Content-Length，用来验证客户端"靠 JPEG 标记定界"的兜底分支。
"""
from __future__ import annotations

import argparse
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

import cv2

INDEX_HTML = b"""<!doctype html><html lang="zh"><meta charset="utf-8">
<title>Mock ESP32 Stream</title>
<body style="background:#15171a;color:#eee;font-family:system-ui;text-align:center">
<h3>Mock ESP32-S3-EYE \xe5\x9b\xbe\xe4\xbc\xa0</h3>
<img src="/stream" style="max-width:90%;border-radius:12px">
<p><a href="/capture" style="color:#6cf">/capture</a></p></body></html>"""


class MockMjpegServer:
    """在后台线程里跑一个 MJPEG 服务器。"""

    def __init__(
        self,
        video_path: str,
        host: str = "127.0.0.1",
        port: int = 0,
        fps: float = 15.0,
        boundary: str = "f",
        quality: int = 80,
        send_length: bool = True,
        loop: bool = True,
        size: Optional[tuple] = None,
    ):
        self.video_path = str(video_path)
        self.host = host
        self.port = int(port)
        self.fps = float(fps)
        self.boundary = boundary
        self.quality = int(quality)
        self.send_length = bool(send_length)
        self.loop = bool(loop)
        #: 把帧缩放到这个尺寸再发（用来模拟板子的真实分辨率，如 (640, 480)）
        self.size = (int(size[0]), int(size[1])) if size else None
        self.running = True
        self._server: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

    # ------------------------------------------------------------------ 生命周期
    def start(self) -> "MockMjpegServer":
        self._server = ThreadingHTTPServer((self.host, self.port), _StreamHandler)
        self._server.daemon_threads = True
        self._server.mock = self  # type: ignore[attr-defined]
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self.running = False
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=3.0)
        self._server = None
        self._thread = None

    def __enter__(self) -> "MockMjpegServer":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/stream"

    # ------------------------------------------------------------------ 取帧
    def open_capture(self) -> cv2.VideoCapture:
        cap = cv2.VideoCapture(self.video_path)
        if not cap.isOpened():
            raise IOError(f"mock 流无法打开视频: {self.video_path}")
        return cap

    def next_frame(self, cap: cv2.VideoCapture):
        ok, frame = cap.read()
        if not ok and self.loop:
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            ok, frame = cap.read()
        if not ok:
            return None
        if self.size is not None and (frame.shape[1], frame.shape[0]) != self.size:
            frame = cv2.resize(frame, self.size, interpolation=cv2.INTER_AREA)
        return frame


class _StreamHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args) -> None:  # 保持安静
        pass

    @property
    def mock(self) -> MockMjpegServer:
        return self.server.mock  # type: ignore[attr-defined]

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler 约定的方法名
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            self._send_bytes(INDEX_HTML, "text/html; charset=utf-8")
        elif path == "/stream":
            self._stream()
        elif path == "/capture":
            self._capture()
        else:
            self._send_bytes(b"not found", "text/plain", status=404)

    # ------------------------------------------------------------------ 辅助
    def _send_bytes(self, body: bytes, ctype: str, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _capture(self) -> None:
        cap = self.mock.open_capture()
        try:
            frame = self.mock.next_frame(cap)
        finally:
            cap.release()
        if frame is None:
            self._send_bytes(b"fail", "text/plain", status=500)
            return
        ok, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, self.mock.quality])
        if not ok:
            self._send_bytes(b"fail", "text/plain", status=500)
            return
        self._send_bytes(buf.tobytes(), "image/jpeg")

    def _stream(self) -> None:
        mock = self.mock
        boundary = mock.boundary.encode("latin-1")
        self.send_response(200)
        self.send_header("Content-Type", f"multipart/x-mixed-replace; boundary={mock.boundary}")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()

        cap = mock.open_capture()
        period = 1.0 / mock.fps if mock.fps > 0 else 0.0
        try:
            while mock.running:
                t0 = time.time()
                frame = mock.next_frame(cap)
                if frame is None:
                    break
                ok, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, mock.quality])
                if not ok:
                    continue
                data = buf.tobytes()
                self.wfile.write(b"--" + boundary + b"\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                if mock.send_length:
                    self.wfile.write(b"Content-Length: " + str(len(data)).encode() + b"\r\n")
                self.wfile.write(b"\r\n")
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
                if period > 0:
                    rest = period - (time.time() - t0)
                    if rest > 0:
                        time.sleep(rest)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except OSError:
            pass
        finally:
            cap.release()


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="把视频模拟成 ESP32 MJPEG 流")
    ap.add_argument("--video", required=True, help="视频文件路径")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--fps", type=float, default=15.0)
    ap.add_argument("--quality", type=int, default=80, help="JPEG 质量 1-100")
    ap.add_argument("--boundary", default="f")
    ap.add_argument("--no-length", action="store_true", help="不发送 Content-Length（测试兜底解析）")
    ap.add_argument("--size", default=None,
                    help="缩放到的分辨率，如 640x480。用来模拟板子的真实输出尺寸")
    args = ap.parse_args(argv)

    size = None
    if args.size:
        try:
            w, h = args.size.lower().split("x")
            size = (int(w), int(h))
        except ValueError:
            raise SystemExit("--size 格式应为 640x480")

    srv = MockMjpegServer(
        args.video,
        host=args.host,
        port=args.port,
        fps=args.fps,
        boundary=args.boundary,
        quality=args.quality,
        send_length=not args.no_length,
        size=size,
    ).start()
    print(f"[mock] 视频: {args.video}")
    if size:
        print(f"[mock] 输出分辨率: {size[0]}x{size[1]}")
    print(f"[mock] 流地址: {srv.url}")
    print(f"[mock] 快照:   http://{args.host}:{srv.port}/capture")
    print(f"[mock] 浏览器: http://{args.host}:{srv.port}/")
    print("[mock] Ctrl+C 退出")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[mock] 退出")
    finally:
        srv.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
