"""配置读写的单元测试。"""
from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

from vision_assist.config import AppConfig, CameraCalib


class TestConfigRoundTrip(unittest.TestCase):
    def test_to_dict_from_dict_roundtrip(self):
        cfg = AppConfig()
        cfg.camera = CameraCalib(height_m=1.35, pitch_deg=27.5, hfov_deg=70.0)
        cfg.tactile.conf = 0.33
        cfg.speech.cooldown_s = dict(cfg.speech.cooldown_s, OBSTACLE_NEAR=9.0)
        cfg.source = "mjpeg:http://10.0.0.5/stream"

        restored = AppConfig.from_dict(json.loads(json.dumps(cfg.to_dict())))

        self.assertEqual(restored.camera.height_m, 1.35)
        self.assertEqual(restored.camera.pitch_deg, 27.5)
        self.assertEqual(restored.tactile.conf, 0.33)
        self.assertEqual(restored.speech.cooldown_s["OBSTACLE_NEAR"], 9.0)
        self.assertEqual(restored.source, "mjpeg:http://10.0.0.5/stream")
        # 往返后应完全等价
        self.assertEqual(restored.to_dict(), cfg.to_dict())

    def test_nested_types_are_rebuilt(self):
        """from_dict 必须重建出真正的 dataclass，而不是留着 dict。"""
        restored = AppConfig.from_dict(AppConfig().to_dict())
        self.assertIsInstance(restored.camera, CameraCalib)
        self.assertIsInstance(restored.tactile, type(AppConfig().tactile))
        self.assertIsInstance(restored.crosswalk, type(AppConfig().crosswalk))
        self.assertIsInstance(restored.obstacle.class_map, dict)

    def test_unknown_keys_ignored(self):
        data = AppConfig().to_dict()
        data["tactile"]["not_a_real_field"] = 123
        cfg = AppConfig.from_dict(data)
        self.assertFalse(hasattr(cfg.tactile, "not_a_real_field"))

    def test_partial_dict_uses_defaults(self):
        cfg = AppConfig.from_dict({"tactile": {"conf": 0.5}})
        self.assertEqual(cfg.tactile.conf, 0.5)
        self.assertEqual(cfg.camera.pitch_deg, CameraCalib().pitch_deg)

    def test_from_dict_covers_every_top_level_scalar_field(self):
        """``from_dict`` 必须覆盖所有顶层标量字段。

        这类 bug 已经犯过两次（``show_ground_grid``、``imgsz_obstacle``）：
        字段加进 dataclass 但忘了在 ``from_dict`` 里带上，结果 ``POST /config``
        改它完全无效，而且**不会报错**，只能靠肉眼发现。
        """
        import dataclasses

        default = AppConfig()
        probe = {}
        for f in dataclasses.fields(AppConfig):
            current = getattr(default, f.name)
            if isinstance(current, bool):
                probe[f.name] = not current
            elif isinstance(current, int):
                probe[f.name] = current + 7
            elif isinstance(current, float):
                probe[f.name] = current + 3.5
            elif isinstance(current, str):
                probe[f.name] = current + "_X"

        self.assertGreater(len(probe), 5, "探针字段太少，测试本身失效了")

        merged = default.to_dict()
        merged.update(probe)
        restored = AppConfig.from_dict(merged)

        missed = [(k, v, getattr(restored, k)) for k, v in probe.items()
                  if getattr(restored, k) != v]
        self.assertEqual(missed, [],
                         f"这些顶层字段没有在 from_dict 里生效: {missed}")


class TestConfigFileAndEnv(unittest.TestCase):
    def test_save_and_load(self):
        cfg = AppConfig()
        cfg.camera = CameraCalib(height_m=1.45, pitch_deg=22.0)
        # 写在工作区内（沙箱允许），避免系统临时目录权限问题
        td = Path(__file__).resolve().parent / "_scratch"
        td.mkdir(exist_ok=True)
        try:
            path = td / "calib.json"
            cfg.save(path)
            loaded = AppConfig.load(path)
        finally:
            if path.is_file():
                path.unlink()
            try:
                td.rmdir()
            except OSError:
                pass
        self.assertEqual(loaded.camera.height_m, 1.45)
        self.assertEqual(loaded.camera.pitch_deg, 22.0)

    def test_missing_file_raises(self):
        with self.assertRaises(FileNotFoundError):
            AppConfig.load("no_such_config.json")

    def test_env_overrides(self):
        old = {k: os.environ.get(k) for k in
               ("VISION_CAM_HEIGHT_M", "VISION_CAM_PITCH", "VISION_SOURCE")}
        try:
            os.environ["VISION_CAM_HEIGHT_M"] = "1.6"
            os.environ["VISION_CAM_PITCH"] = "25"
            os.environ["VISION_SOURCE"] = "usb:0"
            cfg = AppConfig.load()
            self.assertEqual(cfg.camera.height_m, 1.6)
            self.assertEqual(cfg.camera.pitch_deg, 25.0)
            self.assertEqual(cfg.source, "usb:0")
        finally:
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v


class TestDefaults(unittest.TestCase):
    def test_threshold_units_are_sane(self):
        cfg = AppConfig()
        self.assertGreater(cfg.camera.height_m, 0.5)
        self.assertLess(cfg.camera.height_m, 2.5)
        self.assertTrue(0 < cfg.camera.pitch_deg < 90)
        self.assertLess(cfg.tactile.drift_exit_m, cfg.tactile.drift_enter_m,
                        "滞回要求退出阈值小于进入阈值")
        self.assertLess(cfg.tactile.turn_exit_deg, cfg.tactile.turn_enter_deg)
        self.assertEqual(cfg.temporal.confirm_frames >= 1, True)

    def test_priority_table_covers_cooldown_keys(self):
        cfg = AppConfig()
        missing = set(cfg.speech.cooldown_s) - set(cfg.speech.priority)
        self.assertFalse(missing, f"这些事件有冷却但没有优先级: {missing}")

    def test_model_paths_exist_or_are_expected(self):
        from vision_assist.config import COCO_MODEL_PATH, MODELS_DIR, TACTILE_MODEL_PATH
        self.assertEqual(TACTILE_MODEL_PATH.parent, MODELS_DIR)
        self.assertEqual(COCO_MODEL_PATH.parent, MODELS_DIR)
        self.assertTrue(str(TACTILE_MODEL_PATH).endswith(".pt"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
