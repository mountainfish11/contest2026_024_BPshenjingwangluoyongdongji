"""Scene JSON schema 校验的测试。"""
from __future__ import annotations

import json
import unittest

import numpy as np

from vision_assist.config import AppConfig, CameraCalib, find_sample_video
from vision_assist.detectors.tactile import TactileBox, TactileResult
from vision_assist.pipeline import Pipeline
from vision_assist.schema import (
    ALERT_KINDS,
    GUIDE_STATES,
    TILE_TYPES,
    assert_valid_scene,
    validate_scene,
)
from vision_assist.sources import LocalVideoSource
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST_VIDEO = find_sample_video() or (ROOT / "data" / "Test.mp4")  # 占位，is_file() 为 False


class TestSchemaConstants(unittest.TestCase):
    def test_states_match_enum(self):
        from vision_assist.state import GuideState
        self.assertEqual(set(GUIDE_STATES), {s.value for s in GuideState})

    def test_alert_kinds_match_module(self):
        import vision_assist.alerts as alerts
        expected = {getattr(alerts, k) for k in ALERT_KINDS}
        self.assertEqual(expected, set(ALERT_KINDS))


class TestValidateScene(unittest.TestCase):
    def _scene(self, **kw):
        from vision_assist.pipeline import Scene
        return Scene(**kw).to_dict()

    def test_empty_scene_is_valid(self):
        d = self._scene()
        self.assertEqual(validate_scene(d), [], "默认 Scene 应通过校验")

    def test_json_roundtrip_is_valid(self):
        d = json.loads(json.dumps(self._scene(), ensure_ascii=False))
        self.assertEqual(validate_scene(d), [])

    def test_detects_unknown_state(self):
        d = self._scene()
        d["state"] = "NOT_A_STATE"
        self.assertTrue(any("未知的 state" in p for p in validate_scene(d)))

    def test_detects_missing_field(self):
        d = self._scene()
        del d["tactile"]["lateral_offset_m"]
        self.assertTrue(any("lateral_offset_m" in p for p in validate_scene(d)))

    def test_detects_negative_warning_distance(self):
        d = self._scene()
        d["tactile"]["warning_near_m"] = -1.24
        problems = validate_scene(d)
        self.assertTrue(any("不能为负" in p for p in problems))

    def test_detects_unknown_alert_kind(self):
        d = self._scene()
        d["alerts"]["alerts"] = [{"kind": "ALIEN", "priority": 1, "distance_m": None, "detail": ""}]
        self.assertTrue(any("未知 kind" in p for p in validate_scene(d)))

    def test_detects_wrong_tile_type(self):
        d = self._scene()
        d["tactile"]["tile_type"] = "brick"
        self.assertTrue(any("tile_type" in p for p in validate_scene(d)))

    def test_assert_raises_on_invalid(self):
        d = self._scene()
        d["state"] = "BAD"
        with self.assertRaises(ValueError):
            assert_valid_scene(d)


@unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
class TestRealPipelineOutputIsValid(unittest.TestCase):
    def test_every_frame_passes_schema(self):
        cfg = AppConfig()
        cfg.camera = CameraCalib(height_m=1.3, pitch_deg=85.0)
        pipe = Pipeline(cfg)
        src = LocalVideoSource(str(TEST_VIDEO))
        try:
            for i in range(25):
                frame = src.read()
                if frame is None:
                    break
                scene = pipe.process(frame, i, src.info, now=i / 15.0)
                problems = validate_scene(scene.to_dict())
                self.assertEqual(problems, [], f"第 {i} 帧 schema 不合法: {problems}")
        finally:
            src.close()
            pipe.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
