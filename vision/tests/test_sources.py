"""帧源与模拟流的单元测试。"""
from __future__ import annotations

import time
import unittest
from pathlib import Path

import numpy as np

from tools.mock_stream import MockMjpegServer
from vision_assist.config import find_sample_video
from vision_assist.sources import (
    LocalVideoSource,
    MjpegSource,
    open_source,
)

ROOT = Path(__file__).resolve().parent.parent
TEST_VIDEO = find_sample_video() or (ROOT / "data" / "Test.mp4")  # 占位，is_file() 为 False


class TestLocalVideo(unittest.TestCase):
    @unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
    def test_read_and_loop(self):
        src = LocalVideoSource(str(TEST_VIDEO), loop=True)
        try:
            frame = src.read()
            self.assertIsNotNone(frame)
            assert frame is not None
            self.assertEqual(frame.ndim, 3)
            self.assertEqual(frame.shape[2], 3)
            info = src.info
            self.assertEqual(info["type"], "file")
            self.assertGreater(info["size"][0], 0)
        finally:
            src.close()

    def test_missing_file_raises(self):
        with self.assertRaises(IOError):
            LocalVideoSource("definitely_not_here.mp4")


@unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
class TestMjpegSource(unittest.TestCase):
    def _consume(self, server: MockMjpegServer, want: int = 5) -> list[np.ndarray]:
        src = MjpegSource(server.url, timeout=5.0)
        frames: list[np.ndarray] = []
        try:
            deadline = time.time() + 20.0
            while len(frames) < want and time.time() < deadline:
                f = src.read()
                if f is not None:
                    frames.append(f)
        finally:
            src.close()
        return frames

    def test_reads_frames_with_content_length(self):
        with MockMjpegServer(str(TEST_VIDEO), fps=25.0, send_length=True) as srv:
            frames = self._consume(srv, 5)
        self.assertGreaterEqual(len(frames), 5)
        self.assertEqual(frames[0].shape[2], 3)
        self.assertGreater(frames[0].shape[1], 0)

    def test_reads_frames_without_content_length(self):
        """没有 Content-Length 时必须靠 JPEG 的 FFD8/FFD9 标记定界。"""
        with MockMjpegServer(str(TEST_VIDEO), fps=25.0, send_length=False) as srv:
            frames = self._consume(srv, 5)
        self.assertGreaterEqual(len(frames), 5, "兜底解析分支失败")

    def test_frames_differ_over_time(self):
        """确认拿到的是连续帧，而不是同一张图反复返回。"""
        with MockMjpegServer(str(TEST_VIDEO), fps=25.0) as srv:
            frames = self._consume(srv, 6)
        self.assertGreaterEqual(len(frames), 6)
        diffs = [float(np.abs(frames[i].astype(np.int16) - frames[i + 1].astype(np.int16)).mean())
                 for i in range(len(frames) - 1)]
        self.assertGreater(max(diffs), 0.5, "连续帧完全相同，可能是解帧逻辑卡住了")

    def test_throughput_meets_minimum(self):
        """模拟流给 15 fps，客户端至少要稳定拿到 10 fps（验收标准）。"""
        with MockMjpegServer(str(TEST_VIDEO), fps=15.0) as srv:
            src = MjpegSource(srv.url, timeout=5.0)
            try:
                for _ in range(2):  # 预热
                    src.read()
                n = 0
                t0 = time.time()
                while time.time() - t0 < 3.0:
                    if src.read() is not None:
                        n += 1
                fps = n / (time.time() - t0)
            finally:
                src.close()
        self.assertGreaterEqual(fps, 10.0, f"吞吐仅 {fps:.1f} fps，低于 10 fps")

    def test_small_frames_are_not_delayed_by_buffer_fill(self):
        """小尺寸帧不能被"攒满缓冲区"卡成一阵一阵的。

        真实 bug：``HTTPResponse.read(65536)`` 会一直阻塞到攒满 64 KB。板子一帧
        才 9 KB，于是每 7 帧（约 350 ms）才返回一次数据，之后又从缓冲区里一次放出
        好几帧——**平均吞吐看不出来，但帧间隔极不均匀**，消费端只能处理 4–5 fps。

        所以这里测的是**最大帧间隔**而不是平均帧率：用 160x120 的极小帧放大效应。
        """
        with MockMjpegServer(str(TEST_VIDEO), fps=15.0, quality=30,
                             size=(160, 120)) as srv:
            src = MjpegSource(srv.url, timeout=5.0)
            stamps = []
            try:
                for _ in range(3):          # 预热
                    src.read()
                t0 = time.time()
                while time.time() - t0 < 3.0:
                    if src.read() is not None:
                        stamps.append(time.time())
                t0_wall = t0
            finally:
                src.close()

        self.assertGreater(len(stamps), 10, "3 秒内读到的帧太少，无法评估抖动")
        gaps = [b - a for a, b in zip(stamps, stamps[1:])]
        max_gap = max(gaps)
        # 15 fps 的流理论间隔 ≈ 67 ms；攒包会造成几百毫秒的空档
        self.assertLess(
            max_gap, 0.25,
            f"最大帧间隔 {max_gap*1000:.0f} ms —— 说明又退回了阻塞式 read(65536)，"
            "帧会一阵一阵地到达")

    def test_reconnect_counter_resets_after_success(self):
        """收到帧后必须把重连计数清零。

        固件里给每条推流连接加了 20 秒上限（防止僵尸连接卡死板子），连接会周期性
        断开重连。如果退避只增不减，跑久了每次重连都要等满上限，等于白丢帧。
        """
        with MockMjpegServer(str(TEST_VIDEO), fps=25.0) as srv:
            src = MjpegSource(srv.url, timeout=5.0)
            try:
                for _ in range(3):
                    src.read()
                src._reconnects = 7          # 模拟之前失败过很多次
                for _ in range(10):
                    if src.read() is not None:
                        break
                self.assertEqual(src._reconnects, 0,
                                 "读到帧后重连计数应清零，否则退避会一直累积")
            finally:
                src.close()


class TestOpenSourceDispatch(unittest.TestCase):
    def test_http_url_is_mjpeg(self):
        src = open_source("http://127.0.0.1:1/stream")
        self.assertIsInstance(src, MjpegSource)
        src.close()

    def test_mjpeg_prefix_strips(self):
        src = open_source("mjpeg:http://127.0.0.1:1/stream")
        self.assertIsInstance(src, MjpegSource)
        self.assertEqual(src.url, "http://127.0.0.1:1/stream")
        src.close()

    @unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
    def test_file_specs(self):
        for spec in (f"file:{TEST_VIDEO}", f"video:{TEST_VIDEO}", str(TEST_VIDEO)):
            src = open_source(spec)
            self.assertIsInstance(src, LocalVideoSource)
            src.close()

    def test_empty_spec_rejected(self):
        with self.assertRaises(ValueError):
            open_source("   ")


if __name__ == "__main__":
    unittest.main(verbosity=2)
