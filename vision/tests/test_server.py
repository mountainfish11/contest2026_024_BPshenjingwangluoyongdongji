"""HTTP 调试服务的测试（真实起服务，用标准库客户端发请求）。"""
from __future__ import annotations

import json
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

import cv2

from vision_assist.config import AppConfig, CameraCalib, find_sample_video
from vision_assist.server import VisionService, serve

ROOT = Path(__file__).resolve().parent.parent
TEST_VIDEO = find_sample_video() or (ROOT / "data" / "Test.mp4")  # 占位，is_file() 为 False

#: 测试里关掉模型，只验证 HTTP 管道（快）
FAST_DETECTORS = dict(enable_tactile=False, enable_obstacle=False,
                      enable_crosswalk=False, enable_stairs=False)


def http_get(url: str, timeout: float = 10.0):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.status, resp.read(), dict(resp.headers)


def http_post_json(url: str, payload: dict, timeout: float = 10.0):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data,
                                headers={"Content-Type": "application/json"},
                                method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        # 4xx/5xx 也要能读回 JSON 响应体
        body = exc.read()
        try:
            return exc.code, json.loads(body.decode())
        except (json.JSONDecodeError, UnicodeDecodeError):
            return exc.code, {"ok": False, "error": body.decode("utf-8", "replace")}


@unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
class TestHttpEndpoints(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = AppConfig()
        cfg.camera = CameraCalib(height_m=1.3, pitch_deg=85.0)
        cls.service = VisionService(cfg, source=f"file:{TEST_VIDEO}",
                                   width_limit=480, pipeline_kwargs=FAST_DETECTORS)
        cls.service.start()
        cls.server = serve(cls.service, host="127.0.0.1", port=0)
        cls.port = cls.server.server_address[1]
        cls.base = f"http://127.0.0.1:{cls.port}"
        # 等服务产出第一帧
        deadline = time.time() + 15
        while time.time() < deadline and cls.service.scene() is None:
            time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.service.stop()

    def test_index_page_served(self):
        status, body, headers = http_get(self.base + "/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        self.assertIn("调试台".encode("utf-8"), body)

    def test_health(self):
        status, body, _h = http_get(self.base + "/health")
        self.assertEqual(status, 200)
        data = json.loads(body.decode())
        self.assertTrue(data["running"])
        self.assertEqual(data["source"], f"file:{TEST_VIDEO}")
        self.assertIn("schema_version", data)
        self.assertIsNotNone(data["camera"])

    def test_scene_endpoint(self):
        status, body, _h = http_get(self.base + "/api/scene")
        self.assertEqual(status, 200)
        scene = json.loads(body.decode())
        for key in ("state", "tactile", "tracker", "alerts", "quality",
                    "obstacles", "crosswalk", "traffic_light", "stairs"):
            self.assertIn(key, scene, f"Scene 缺少 {key}")
        self.assertNotIn("schema_problems", scene,
                         f"输出 schema 校验失败: {scene.get('schema_problems')}")

    def test_snapshot_is_jpeg(self):
        status, body, headers = http_get(self.base + "/snapshot.jpg")
        self.assertEqual(status, 200)
        self.assertIn("image/jpeg", headers.get("Content-Type", ""))
        self.assertTrue(body.startswith(b"\xff\xd8"), "应返回 JPEG（SOI 标记）")

    def test_stream_returns_mjpeg_frames(self):
        """读一小段 MJPEG 流，确认有分片边界与 JPEG 数据。"""
        req = urllib.request.Request(self.base + "/stream.mjpg")
        with urllib.request.urlopen(req, timeout=15) as resp:
            self.assertIn("multipart/x-mixed-replace", resp.headers.get("Content-Type", ""))
            buf = b""
            deadline = time.time() + 12
            while time.time() < deadline and buf.count(b"--frame") < 2:
                chunk = resp.read(4096)
                if not chunk:
                    break
                buf += chunk
        self.assertGreaterEqual(buf.count(b"--frame"), 2, "MJPEG 应至少包含两个分片")
        self.assertIn(b"\xff\xd8", buf, "分片里应有 JPEG 数据")

    def test_events_sse_pushes_scene(self):
        req = urllib.request.Request(self.base + "/events")
        with urllib.request.urlopen(req, timeout=15) as resp:
            self.assertIn("text/event-stream", resp.headers.get("Content-Type", ""))
            buf = b""
            deadline = time.time() + 12
            while time.time() < deadline and b"data: " not in buf:
                chunk = resp.read(2048)
                if not chunk:
                    break
                buf += chunk
        self.assertIn(b"data: ", buf)
        payload = buf.split(b"data: ", 1)[1].split(b"\n", 1)[0]
        scene = json.loads(payload.decode())
        self.assertIn("state", scene)

    def test_config_endpoint_updates_calibration(self):
        status, resp = http_post_json(self.base + "/config",
                                     {"camera": {"pitch_deg": 30.0, "height_m": 1.2}})
        self.assertEqual(status, 200)
        self.assertTrue(resp["ok"])
        self.assertAlmostEqual(resp["config"]["camera"]["pitch_deg"], 30.0)
        # 生效后 /health 里的相机描述应同步更新
        deadline = time.time() + 10
        seen = None
        while time.time() < deadline:
            _s, body, _h = http_get(self.base + "/health")
            cam = json.loads(body.decode()).get("camera")
            if cam and abs(cam["pitch_deg"] - 30.0) < 0.01:
                seen = cam
                break
            time.sleep(0.1)
        self.assertIsNotNone(seen, "配置修改后相机标定未生效")

    def test_source_switch_rejects_bad_spec(self):
        status, resp = http_post_json(self.base + "/source", {"source": ""})
        self.assertEqual(status, 400)
        self.assertFalse(resp["ok"])

    # ---- 浏览器内标定：这是 GUI 起不来时的主路径 -------------------------------
    def test_config_exposes_ground_grid_flag(self):
        status, body, _h = http_get(self.base + "/api/config")
        self.assertEqual(status, 200)
        cfg = json.loads(body.decode())
        self.assertIn("show_ground_grid", cfg)
        self.assertIn("camera", cfg)

    def test_can_toggle_ground_grid(self):
        _s, resp = http_post_json(self.base + "/config", {"show_ground_grid": False})
        self.assertTrue(resp["ok"])
        self.assertFalse(resp["config"]["show_ground_grid"])
        _s, resp2 = http_post_json(self.base + "/config", {"show_ground_grid": True})
        self.assertTrue(resp2["config"]["show_ground_grid"])

    def test_save_config_writes_file(self):
        target = "tests/_scratch/browser_calib.json"
        scratch = ROOT / "tests" / "_scratch"
        scratch.mkdir(parents=True, exist_ok=True)
        out = ROOT / target
        try:
            _s, resp = http_post_json(self.base + "/config/save", {"path": target})
            self.assertTrue(resp["ok"], resp)
            self.assertTrue(out.is_file(), "配置文件没有真的写出来")
            saved = json.loads(out.read_text(encoding="utf-8"))
            self.assertIn("camera", saved)
            self.assertEqual(saved["camera"]["height_m"], resp["config"]["camera"]["height_m"])
        finally:
            if out.is_file():
                out.unlink()
            try:
                scratch.rmdir()
            except OSError:
                pass

    def test_save_config_rejects_path_outside_project(self):
        _s, resp = http_post_json(self.base + "/config/save",
                                  {"path": "../../evil_calibration.json"})
        self.assertFalse(resp["ok"])
        self.assertIn("项目目录", resp["error"])

    def test_snapshot_contains_ground_grid_pixels(self):
        """开着地面网格时，画面里应该出现绿色网格线（G 通道显著偏高的像素）。"""
        http_post_json(self.base + "/config", {"show_ground_grid": True,
                                               "camera": {"pitch_deg": 40.0}})
        time.sleep(0.8)
        _s, body, _h = http_get(self.base + "/snapshot.jpg")
        import numpy as np
        img = cv2.imdecode(np.frombuffer(body, np.uint8), cv2.IMREAD_COLOR)
        self.assertIsNotNone(img)
        b, g, r = img[:, :, 0].astype(int), img[:, :, 1].astype(int), img[:, :, 2].astype(int)
        greenish = ((g > 120) & (g - r > 25) & (g - b > 25)).sum()
        self.assertGreater(greenish, 200, "画面里没看到绿色网格线")

    def test_unknown_path_404(self):
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            http_get(self.base + "/nope")
        self.assertEqual(ctx.exception.code, 404)

    def test_reset_endpoint(self):
        status, resp = http_post_json(self.base + "/reset", {})
        self.assertEqual(status, 200)
        self.assertTrue(resp["ok"])


@unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
class TestPortConflict(unittest.TestCase):
    """端口被占用时必须自动换端口。

    Windows 上 SO_REUSEADDR 允许第二个进程绑定同一端口（端口劫持），
    浏览器会连到几小时前那个"死服务"上，界面永远显示"无新帧"，
    而命令行看起来一切正常——这个坑实际踩过。
    """

    def test_port_in_use_detects_listener(self):
        import socket as _socket

        from vision_assist.server import port_in_use

        holder = _socket.socket()
        holder.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
        try:
            holder.bind(("127.0.0.1", 0))
            holder.listen(1)
            port = holder.getsockname()[1]
            self.assertTrue(port_in_use("127.0.0.1", port))
        finally:
            holder.close()

    def test_free_port_reported_free(self):
        import socket as _socket

        from vision_assist.server import port_in_use

        probe = _socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        self.assertFalse(port_in_use("127.0.0.1", port))

    def test_serve_falls_back_to_next_port(self):
        import socket as _socket

        from vision_assist.server import serve

        cfg = AppConfig()
        cfg.device = "cpu"
        service = VisionService(cfg, source=f"file:{TEST_VIDEO}",
                                pipeline_kwargs=FAST_DETECTORS)
        service.start()

        holder = _socket.socket()
        holder.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
        holder.bind(("127.0.0.1", 0))
        holder.listen(1)
        taken = holder.getsockname()[1]
        try:
            server = serve(service, host="127.0.0.1", port=taken)
            try:
                self.assertNotEqual(server.server_address[1], taken,
                                    "端口被占时应该换端口，而不是劫持它")
                # 新端口应该真的能提供服务
                base = f"http://127.0.0.1:{server.server_address[1]}"
                status, body, _h = http_get(base + "/health")
                self.assertEqual(status, 200)
                self.assertTrue(json.loads(body.decode())["running"])
            finally:
                server.shutdown()
                server.server_close()
        finally:
            holder.close()
            service.stop()


@unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
class TestServiceWithTactile(unittest.TestCase):
    """至少验证一次"真的跑模型"的服务链路。"""

    def test_scene_contains_tactile_fields(self):
        cfg = AppConfig()
        cfg.camera = CameraCalib(height_m=1.3, pitch_deg=85.0)
        service = VisionService(cfg, source=f"file:{TEST_VIDEO}", width_limit=480,
                                pipeline_kwargs=dict(enable_obstacle=False,
                                                     enable_crosswalk=False,
                                                     enable_stairs=False))
        service.start()
        server = serve(service, host="127.0.0.1", port=0)
        try:
            base = f"http://127.0.0.1:{server.server_address[1]}"
            deadline = time.time() + 40
            scene = None
            while time.time() < deadline:
                _s, body, _h = http_get(base + "/api/scene")
                candidate = json.loads(body.decode())
                if candidate.get("frame", 0) > 2:
                    scene = candidate
                    break
                time.sleep(0.2)
            self.assertIsNotNone(scene, "服务在 40 秒内没有产出帧")
            self.assertIn("tile_type", scene["tactile"])
            self.assertNotIn("schema_problems", scene)
        finally:
            server.shutdown()
            server.server_close()
            service.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
