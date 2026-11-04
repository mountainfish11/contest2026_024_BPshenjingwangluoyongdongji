"""后台抽帧包装的测试。

这个包装解决的是一个真实故障：处理管线每帧要 70ms，而 ESP32 是逐帧同步写 socket 的。
读得比推得慢 → TCP 缓冲区堆积 → ESP32 的 cl.write() 被堵住 → 画面一顿一顿还掉帧。
包装后后台线程永远把流读空，前台只取最新帧。
"""
from __future__ import annotations

import threading
import time
import unittest
from typing import List, Optional

import numpy as np

from vision_assist.sources import FrameSource, LatestFrameSource


class FakeSource(FrameSource):
    """按固定速率产出递增帧，可控制"帧内第几帧"以便断言取到的是最新帧。"""

    def __init__(self, fps: float = 30.0, size=(240, 320)):
        self.fps = fps
        self.size = size
        self.produced = 0
        self.closed = False
        self.raise_at: Optional[int] = None
        self._lock = threading.Lock()

    def read(self) -> Optional[np.ndarray]:
        time.sleep(1.0 / self.fps)
        with self._lock:
            if self.raise_at is not None and self.produced >= self.raise_at:
                raise OSError("模拟读帧异常")
            self.produced += 1
            frame = np.full((self.size[0], self.size[1], 3), self.produced % 256, np.uint8)
        return frame

    @property
    def info(self) -> dict:
        return {"type": "fake", "size": list(self.size), "fps": self.fps}

    def close(self) -> None:
        self.closed = True


class TestLatestFrameSource(unittest.TestCase):
    def test_returns_latest_frame(self):
        inner = FakeSource(fps=200.0)
        src = LatestFrameSource(inner).start()
        try:
            time.sleep(0.3)
            frame, seq, ts = src.read_with_meta()
            self.assertIsNotNone(frame)
            self.assertGreater(seq, 5)
            self.assertGreater(ts, 0.0)
            # 拿到的应该是较新的一帧，而不是第 1 帧
            self.assertGreater(int(frame[0, 0, 0]), 5)
        finally:
            src.stop()

    def test_producer_is_never_blocked_by_slow_consumer(self):
        """核心承诺：消费端再慢，生产端也不该被拖住。"""
        inner = FakeSource(fps=100.0)
        src = LatestFrameSource(inner).start()
        try:
            before = inner.produced
            time.sleep(0.6)
            after = inner.produced
            # 0.6 秒内至少应该产出几十帧（消费端完全没读）
            self.assertGreater(after - before, 20,
                               "生产端被阻塞了，说明后台没有持续读空")
        finally:
            src.stop()

    def test_reader_survives_exceptions(self):
        inner = FakeSource(fps=200.0)
        inner.raise_at = 5
        src = LatestFrameSource(inner).start()
        try:
            time.sleep(0.3)
            self.assertGreater(src.read_errors, 0, "异常应被计数")
            self.assertTrue(src._thread.is_alive(), "读帧线程不能因为异常死掉")
        finally:
            src.stop()

    def test_reports_source_fps(self):
        inner = FakeSource(fps=60.0)
        src = LatestFrameSource(inner).start()
        try:
            time.sleep(0.8)
            fps = src.source_fps
            self.assertIsNotNone(fps)
            self.assertGreater(fps, 10.0)
        finally:
            src.stop()

    def test_stop_closes_inner(self):
        inner = FakeSource(fps=200.0)
        src = LatestFrameSource(inner).start()
        time.sleep(0.1)
        src.stop()
        self.assertTrue(inner.closed, "内层帧源必须被关闭")
        self.assertFalse(src._thread is not None and src._thread.is_alive())

    def test_info_merges_inner(self):
        inner = FakeSource(fps=50.0)
        src = LatestFrameSource(inner).start()
        try:
            time.sleep(0.2)
            info = src.info
            self.assertEqual(info["type"], "fake")
            self.assertTrue(info["threaded"])
            self.assertIn("source_fps", info)
            self.assertGreaterEqual(info["received"], 1)
        finally:
            src.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
