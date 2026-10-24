"""推理设备解析的测试。

重点：装了 CPU 版 torch 时，用户指定 GPU **不能静默退回 CPU** —— 这是最容易
白白浪费时间排查的坑（"我明明有 4060，为什么还是 5 FPS"）。
"""
from __future__ import annotations

import unittest
import warnings

from vision_assist import device as dev


class TestCudaStatus(unittest.TestCase):
    def test_reports_torch_version(self):
        status = dev.cuda_status()
        self.assertIsNotNone(status["torch"])
        self.assertIn("cuda_available", status)
        self.assertIsInstance(status["cuda_available"], bool)

    def test_reports_cuda_build_field(self):
        status = dev.cuda_status()
        self.assertIn("cuda_build", status)


class TestResolveDevice(unittest.TestCase):
    def test_explicit_cpu_is_respected(self):
        spec, desc = dev.resolve_device("cpu")
        self.assertEqual(spec, "cpu")
        self.assertIn("CPU", desc)

    def test_auto_falls_back_to_cpu_without_cuda(self):
        warned = []
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            spec, desc = dev.resolve_device("auto")
        if dev.cuda_status()["cuda_available"]:
            self.assertTrue(spec.startswith("cuda"))
        else:
            self.assertEqual(spec, "cpu")
            self.assertIn("CPU", desc)

    def test_explicit_cuda_warns_when_unavailable(self):
        """指定 cuda 但 torch 是 CPU 版时，必须发出警告而不是悄悄退回 CPU。"""
        if dev.cuda_status()["cuda_available"]:
            self.skipTest("本机 CUDA 可用，这个分支测不到")
        dev._warned = False          # 重置"只提醒一次"
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            spec, desc = dev.resolve_device("cuda:0")
        self.assertEqual(spec, "cpu")
        self.assertIn("退回", desc)
        self.assertTrue(any(issubclass(w.category, RuntimeWarning) for w in caught),
                        "应发出 RuntimeWarning")

    def test_only_warns_once(self):
        if dev.cuda_status()["cuda_available"]:
            self.skipTest("本机 CUDA 可用")
        dev._warned = False
        with warnings.catch_warnings(record=True) as first:
            warnings.simplefilter("always")
            dev.resolve_device("cuda:0")
        with warnings.catch_warnings(record=True) as second:
            warnings.simplefilter("always")
            dev.resolve_device("cuda:0")
        self.assertGreaterEqual(len(first), 1)
        self.assertEqual(len(second), 0, "提示只应打一次，不该每帧刷屏")


class TestGpuReport(unittest.TestCase):
    def test_report_mentions_install_command(self):
        report = dev.gpu_report()
        self.assertIn("torch", report)
        if not dev.cuda_status()["cuda_available"]:
            # 有 N 卡时必须给出可执行的安装命令
            if dev._nvidia_smi_names():
                self.assertIn("--index-url", report)
                self.assertIn("download.pytorch.org", report)


class TestConfigDevice(unittest.TestCase):
    def test_device_field_roundtrips(self):
        from vision_assist.config import AppConfig
        cfg = AppConfig()
        cfg.device = "cuda:0"
        self.assertEqual(AppConfig.from_dict(cfg.to_dict()).device, "cuda:0")

    def test_pipeline_resolves_device(self):
        from vision_assist.config import AppConfig
        from vision_assist.pipeline import Pipeline
        cfg = AppConfig()
        cfg.device = "cpu"
        pipe = Pipeline(cfg, enable_tactile=False, enable_obstacle=False,
                        enable_crosswalk=False, enable_stairs=False)
        try:
            self.assertEqual(pipe.device, "cpu")
            self.assertTrue(pipe.device_description)
        finally:
            pipe.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
