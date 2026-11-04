"""统一的帧源接口：本地视频 / ESP32 MJPEG 流 / USB 摄像头。

所有帧源都实现同一套协议::

    src = open_source("mjpeg:http://192.168.1.20/stream")
    frame = src.read()      # np.ndarray(BGR) 或 None
    src.info                # 描述信息 dict
    src.close()

MJPEG 解析刻意手写在 ``http.client`` 之上，而不是用 ``cv2.VideoCapture(url)``：
后者的 FFmpeg 后端会缓冲若干帧，延迟明显，且重连行为不可控。
"""
from __future__ import annotations

import re
import socket
import sys
import threading
import time
from abc import ABC, abstractmethod
from http.client import HTTPConnection, HTTPSConnection
from typing import Optional
from urllib.parse import urlparse

import cv2
import numpy as np

_MAX_BUFFER = 8 * 1024 * 1024


class FrameSource(ABC):
    """帧源抽象基类。"""

    @property
    @abstractmethod
    def info(self) -> dict:
        """描述该帧源的字典（用于写进输出 JSON 的 ``source`` 字段）。"""

    @abstractmethod
    def read(self) -> Optional[np.ndarray]:
        """读取下一帧 BGR 图像；暂时读不到时返回 ``None``。"""

    def close(self) -> None:  # pragma: no cover - 默认无资源
        pass

    def __enter__(self) -> "FrameSource":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# --------------------------------------------------------------------------- #
# 本地视频文件
# --------------------------------------------------------------------------- #
class LocalVideoSource(FrameSource):
    """本地视频文件（评测与离线调试用）。"""

    def __init__(self, path: str, loop: bool = True):
        self.path = str(path)
        self.loop = loop
        self._cap = cv2.VideoCapture(self.path)
        if not self._cap.isOpened():
            raise IOError(f"无法打开视频文件: {self.path}")
        self._frames_read = 0
        self._loops = 0

    def read(self) -> Optional[np.ndarray]:
        ok, frame = self._cap.read()
        if not ok:
            if self.loop:
                self._cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                self._loops += 1
                ok, frame = self._cap.read()
            if not ok:
                return None
        self._frames_read += 1
        return frame

    @property
    def info(self) -> dict:
        return {
            "type": "file",
            "uri": self.path,
            "size": [
                int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
                int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
            ],
            "fps": round(float(self._cap.get(cv2.CAP_PROP_FPS) or 0.0), 2),
            "frames_read": self._frames_read,
            "loops": self._loops,
        }

    def close(self) -> None:
        self._cap.release()


# --------------------------------------------------------------------------- #
# USB 摄像头
# --------------------------------------------------------------------------- #
class UsbCameraSource(FrameSource):
    """本机摄像头。"""

    def __init__(self, index: int = 0, width: int = 640, height: int = 480, fps: float = 30.0):
        backend = cv2.CAP_DSHOW if hasattr(cv2, "CAP_DSHOW") else cv2.CAP_ANY
        self._cap = cv2.VideoCapture(index, backend)
        if not self._cap.isOpened():
            self._cap = cv2.VideoCapture(index)
        if not self._cap.isOpened():
            raise IOError(f"无法打开摄像头 index={index}")
        self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        self._cap.set(cv2.CAP_PROP_FPS, fps)
        self.index = index

    def read(self) -> Optional[np.ndarray]:
        ok, frame = self._cap.read()
        return frame if ok else None

    @property
    def info(self) -> dict:
        return {
            "type": "usb",
            "uri": str(self.index),
            "size": [
                int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
                int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
            ],
            "fps": round(float(self._cap.get(cv2.CAP_PROP_FPS) or 0.0), 2),
        }

    def close(self) -> None:
        self._cap.release()


# --------------------------------------------------------------------------- #
# 网络 MJPEG（ESP32-S3 / 大多数 IP 摄像头）
# --------------------------------------------------------------------------- #
class MjpegSource(FrameSource):
    """读取 ``multipart/x-mixed-replace`` 的 MJPEG 流。

    兼容两种打流方式：
    - 带 ``Content-Length`` 的标准分片（ESP32-S3-EYE 示例代码属于这种）；
    - 不带长度、靠 JPEG 的 ``FFD8``/``FFD9`` 标记定界的分片。

    连接断开时自动重连，重连间隔指数退避（封顶 ``max_reconnect_delay`` 秒）。
    """

    def __init__(
        self,
        url: str,
        timeout: float = 6.0,
        max_reconnect_delay: float = 1.0,
    ):
        self.url = url
        self.timeout = timeout
        self.max_reconnect_delay = max_reconnect_delay
        self._conn: Optional[HTTPConnection] = None
        self._resp = None
        self._buf = bytearray()
        self._boundary = b"frame"
        self._parts = 0
        self._bytes = 0
        self._reconnects = 0
        self._last_error: Optional[str] = None
        self._size = [0, 0]

    # ------------------------------------------------------------ 连接管理
    def _connect(self) -> bool:
        try:
            parts = urlparse(self.url)
            if parts.scheme not in ("http", "https") or not parts.hostname:
                raise ValueError(f"不支持的 MJPEG URL: {self.url}")
            cls = HTTPSConnection if parts.scheme == "https" else HTTPConnection
            self._conn = cls(parts.hostname, parts.port, timeout=self.timeout)
            path = parts.path or "/"
            if parts.query:
                path += "?" + parts.query
            self._conn.request("GET", path, headers={"Accept": "multipart/x-mixed-replace"})
            resp = self._conn.getresponse()
            if resp.status != 200:
                raise ConnectionError(f"HTTP {resp.status} {resp.reason}")
            ctype = resp.getheader("Content-Type", "") or ""
            m = re.search(r'boundary="?([^";]+)"?', ctype, re.I)
            if m:
                self._boundary = m.group(1).strip().encode("latin-1")
            self._resp = resp
            self._buf.clear()
            return True
        except Exception as exc:  # noqa: BLE001 - 任何失败都走重连
            self._last_error = f"{type(exc).__name__}: {exc}"
            self._drop()
            return False

    def _drop(self) -> None:
        self._resp = None
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:  # noqa: BLE001
                pass
        self._conn = None
        self._buf.clear()

    def _reconnect(self) -> None:
        self._reconnects += 1
        delay = min(self.max_reconnect_delay, 0.1 * (2 ** min(self._reconnects, 5)))
        time.sleep(delay)
        self._connect()

    def _fill(self, n: int = 65536) -> bool:
        """从网络再读一些字节进缓冲区。

        **必须用 ``read1`` 而不是 ``read``**：``HTTPResponse.read(n)`` 会一直阻塞
        到攒满 n 字节才返回。板子一帧才 9 KB，n=64 KB 就等于要等 7 帧（约 350 ms）
        才返回一次数据，整条管线被卡成 4–5 fps。

        ``read1(n)`` 是"有多少给多少"，一次 ``recv`` 就返回，不会攒包。
        这个坑很隐蔽：``tools/probe_stream`` 用的是原始 ``socket.recv``，所以能测出
        真实的 22 fps，而服务端一直是 4 fps——两边数字对不上就是它造成的。
        """
        if self._resp is None:
            return False
        try:
            read1 = getattr(self._resp, "read1", None)
            data = read1(n) if read1 is not None else self._resp.read(n)
        except (socket.timeout, ConnectionError, OSError) as exc:
            self._last_error = f"{type(exc).__name__}: {exc}"
            return False
        if not data:
            return False
        self._bytes += len(data)
        self._buf.extend(data)
        if len(self._buf) > _MAX_BUFFER:  # 防内存无界增长
            del self._buf[: len(self._buf) // 2]
        return True

    # -------------------------------------------------------------- 解帧
    def _find_boundary(self) -> Optional[int]:
        """在缓冲区中定位分片边界，返回边界起始下标。

        要求 ``--<boundary>`` 之后紧跟 ``\\r\\n``，避免与 JPEG 数据中偶然出现的
        ``--f`` 误匹配。
        """
        pat = b"--" + self._boundary
        start = 0
        while True:
            idx = self._buf.find(pat, start)
            if idx < 0:
                return None
            after = idx + len(pat)
            if self._buf[after:after + 2] == b"\r\n":
                return idx
            start = idx + 1

    def _extract_next_part(self) -> Optional[bytes]:
        while True:
            idx = self._find_boundary()
            if idx is None:
                # 缓冲区尾部可能只有半个边界标记，保留一小段避免误删
                if len(self._buf) > 4096:
                    del self._buf[: len(self._buf) - 4096]
                if not self._fill():
                    return None
                continue

            head_end = self._buf.find(b"\r\n\r\n", idx)
            if head_end < 0:
                if not self._fill():
                    return None
                continue

            headers = bytes(self._buf[idx:head_end]).decode("latin-1", "replace")
            body_start = head_end + 4
            m = re.search(r"Content-Length:\s*(\d+)", headers, re.I)
            if m:
                length = int(m.group(1))
                while len(self._buf) < body_start + length:
                    if not self._fill():
                        return None
                data = bytes(self._buf[body_start:body_start + length])
                del self._buf[: body_start + length]
            else:
                # 无长度信息：退回用 JPEG 的 SOI/EOI 标记定界
                del self._buf[:body_start]
                while True:
                    soi = self._buf.find(b"\xff\xd8")
                    if soi < 0:
                        if not self._fill():
                            return None
                        continue
                    eoi = self._buf.find(b"\xff\xd9", soi + 2)
                    if eoi < 0:
                        if not self._fill():
                            return None
                        continue
                    data = bytes(self._buf[soi:eoi + 2])
                    del self._buf[:eoi + 2]
                    break

            if data:
                self._parts += 1
                return data
            # 空分片，继续找下一个

    def read(self) -> Optional[np.ndarray]:
        if self._resp is None and not self._connect():
            self._reconnect()
            return None

        for _ in range(2):  # 允许一次重连重试
            data = self._extract_next_part()
            if data is None:
                self._drop()
                self._reconnect()
                continue
            frame = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_COLOR)
            if frame is None:
                self._last_error = "JPEG 解码失败"
                continue
            if frame.shape[1] != self._size[0] or frame.shape[0] != self._size[1]:
                self._size = [int(frame.shape[1]), int(frame.shape[0])]
            # 收到帧就重置重连计数：固件里给每条连接加了 20 秒上限，连接会周期性地
            # 主动断开重连。若退避只增不减，跑一小时后每次重连都要等满 2 秒，
            # 相当于白白丢掉 10%% 的帧。
            self._reconnects = 0
            return frame
        return None

    @property
    def bytes_received(self) -> int:
        """累计收到的网络字节数（用来算带宽）。"""
        return self._bytes

    @property
    def info(self) -> dict:
        return {
            "type": "mjpeg",
            "uri": self.url,
            "size": list(self._size),
            "parts": self._parts,
            "bytes": self._bytes,
            "reconnects": self._reconnects,
            "last_error": self._last_error,
        }

    def close(self) -> None:
        self._drop()


# --------------------------------------------------------------------------- #
# 后台抽帧包装：只保留最新帧
# --------------------------------------------------------------------------- #
class LatestFrameSource(FrameSource):
    """把任意帧源包成"后台持续抽帧、前台只取最新帧"。

    为什么必须这样做
    ----------------
    ESP32 那套固件里 ``handleStream()`` 是逐帧同步写 socket 的：相机出一帧就
    ``cl.write()`` 一帧。如果消费端（我们的识别管线）每帧要 70ms，读得比推得慢，
    数据就会积在 TCP 缓冲区里，最后把 ESP32 的 ``cl.write()`` 堵住。表现出来就是：

    - 画面一顿一顿（拿到的是排队很久的旧帧）
    - 掉帧（ESP32 侧 ``CAMERA_GRAB_LATEST`` 只能留最新一帧）
    - 端到端延迟越来越大

    这里把"抽帧"和"处理"拆到两个线程：后台线程**只负责尽快把流读空**，永远不阻塞
    发送方；前台线程想处理的时候取最新那帧。慢的永远是处理，而不是网络。
    """

    def __init__(self, inner: FrameSource, name: str = "", fps_window_s: float = 2.0):
        self._inner = inner
        self._name = name or getattr(inner, "info", {}).get("type", "source")
        self._lock = threading.Lock()
        # 条件变量：让消费端**阻塞**等新帧，而不是 time.sleep 轮询。
        # Windows 的 sleep 粒度是 15.6 ms，轮询一次就白等 15 毫秒，每帧几次
        # 就把吞吐砍到脚踝——实测 19 fps 的流只能处理 4 fps。
        self._cond = threading.Condition(self._lock)
        self._frame: Optional[np.ndarray] = None
        self._seq = 0
        self._frame_ts = 0.0
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._received = 0
        self._read_errors = 0
        self._fps_ema: Optional[float] = None
        self._size = [0, 0]
        # 用"滑动窗口内收到多少帧"算速率。之前用相邻两次读取的间隔取倒数，
        # 但 MJPEG 会突发到达（一次 recv 拿到好几帧），间隔接近 0 会把瞬时
        # 速率炸成几万，EMA 直接被污染成 13561 fps 这种离谱值。
        self._fps_window_s = max(0.2, float(fps_window_s))
        self._window_start = 0.0
        self._window_count = 0
        self._bytes_received = 0
        self._kbps_ema: Optional[float] = None
        self._window_bytes = 0

    # ------------------------------------------------------------------ 生命周期
    def start(self) -> "LatestFrameSource":
        if self._thread is not None:
            return self
        self._stop.clear()
        # 减小 GIL 切换间隔：主线程做识别时是 CPU 密集的 Python 代码，默认 5ms 的
        # 切换间隔会让读流线程长时间拿不到 GIL，表现为"源帧率莫名其妙变低"。
        try:
            sys.setswitchinterval(0.002)
        except Exception:  # noqa: BLE001
            pass
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name=f"frame-reader-{self._name}")
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        with self._cond:
            self._cond.notify_all()      # 唤醒可能还在等帧的消费端
        if self._thread is not None:
            self._thread.join(timeout=3.0)
            self._thread = None
        self._inner.close()

    def close(self) -> None:
        self.stop()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inner.read()
            except Exception:  # noqa: BLE001 - 后台线程不能死
                self._read_errors += 1
                time.sleep(0.05)
                continue

            if frame is None:
                time.sleep(0.002)
                continue

            now = time.time()
            bytes_now = int(getattr(self._inner, "bytes_received", 0) or 0)
            bytes_delta = bytes_now - self._bytes_received
            self._bytes_received = bytes_now
            with self._lock:
                self._frame = frame
                self._seq += 1
                self._frame_ts = now
                self._received += 1
                if frame.shape[1] != self._size[0] or frame.shape[0] != self._size[1]:
                    self._size = [int(frame.shape[1]), int(frame.shape[0])]
                self._cond.notify_all()   # 有新帧了，唤醒等待者

                # 滑动窗口计数法算源帧率
                if self._window_start == 0.0:
                    self._window_start = now
                    self._window_count = 0
                self._window_count += 1
                elapsed = now - self._window_start
                # 第一个值不用等满窗口：攒够 3 帧且过了 0.25 秒就先给个估计，
                # 否则刚启动那两秒界面上一直没有源帧率。
                ready = elapsed >= self._fps_window_s or (
                    self._fps_ema is None and self._window_count >= 3 and elapsed >= 0.25)
                self._window_bytes += bytes_delta
                if ready and elapsed > 1e-6:
                    instant = self._window_count / elapsed
                    kb = (self._window_bytes / elapsed) / 1024.0
                    self._kbps_ema = kb if self._kbps_ema is None else (
                        0.5 * self._kbps_ema + 0.5 * kb)
                    self._window_bytes = 0
                    self._fps_ema = instant if self._fps_ema is None else (
                        0.5 * self._fps_ema + 0.5 * instant)
                    self._window_start = now
                    self._window_count = 0

    # ------------------------------------------------------------------ 取帧
    def read(self) -> Optional[np.ndarray]:
        """取最新一帧（非阻塞）。返回的是共享数组，**不要原地修改**。"""
        with self._lock:
            return self._frame

    def read_with_meta(self) -> tuple[Optional[np.ndarray], int, float]:
        """返回 ``(最新帧, 序号, 采集时间戳)``，便于判断帧是否重复。"""
        with self._lock:
            return self._frame, self._seq, self._frame_ts

    def wait_for_frame(self, after_seq: int, timeout: float = 0.2
                       ) -> tuple[Optional[np.ndarray], int, float]:
        """阻塞等到比 ``after_seq`` 更新的帧（或超时）。

        这比"读一下、睡 2 毫秒、再看"高效得多：Windows 的 sleep 粒度是 15.6 ms，
        轮询式等帧每次白等 15 毫秒——实测 19 fps 的流因此只能处理 4 fps。
        条件变量则是一有新帧立刻唤醒。
        """
        with self._cond:
            if self._seq <= after_seq:
                self._cond.wait(timeout)
            return self._frame, self._seq, self._frame_ts

    # ------------------------------------------------------------------ 统计
    @property
    def received(self) -> int:
        return self._received

    @property
    def read_errors(self) -> int:
        return self._read_errors

    @property
    def kbps(self) -> Optional[float]:
        """流占用的带宽（KB/s），用来判断是不是带宽不够。"""
        return self._kbps_ema

    @property
    def bytes_received(self) -> int:
        return self._bytes_received

    @property
    def source_fps(self) -> Optional[float]:
        """流本身有多少帧/秒到达（与"处理了多少帧"是两件事）。"""
        return self._fps_ema

    @property
    def info(self) -> dict:
        inner_info = dict(self._inner.info)
        inner_info.update({
            "threaded": True,
            "received": self._received,
            "source_fps": None if self._fps_ema is None else round(self._fps_ema, 1),
            "kbps": None if self._kbps_ema is None else round(self._kbps_ema, 1),
            "read_errors": self._read_errors,
            "size": self._size or inner_info.get("size", [0, 0]),
        })
        return inner_info


# --------------------------------------------------------------------------- #
# 工厂
# --------------------------------------------------------------------------- #
def open_source(spec: str, **kwargs) -> FrameSource:
    """按 ``spec`` 打开帧源。

    支持的写法::

        "mjpeg:http://192.168.1.20/stream"
        "http://192.168.1.20/stream"          # 自动识别为 mjpeg
        "file:uploads/Test.mp4"
        "C:/path/to/video.mp4"                # 自动识别为 file
        "usb:0"
        "camera:0"
    """
    spec = str(spec).strip()
    if not spec:
        raise ValueError("帧源描述为空")

    low = spec.lower()
    if low.startswith(("mjpeg:", "http://", "https://")):
        # 注意：这里只支持 MJPEG over HTTP(S)。rtsp:// 需要 FFmpeg，不在本设计范围内。
        url = spec.split(":", 1)[1] if low.startswith("mjpeg:") else spec
        return MjpegSource(url, **{k: v for k, v in kwargs.items() if k in ("timeout", "max_reconnect_delay")})
    if low.startswith(("usb:", "camera:", "cam:")):
        index = int(spec.split(":", 1)[1])
        return UsbCameraSource(index, **{k: v for k, v in kwargs.items() if k in ("width", "height", "fps")})
    if low.startswith(("file:", "video:")):
        return LocalVideoSource(spec.split(":", 1)[1], **kwargs)
    return LocalVideoSource(spec, **kwargs)
