"""行进砖 / 提示砖判别的标注回归测试。

标注来自 ``Test.mp4``：人工圈定区域并确认砖型（条纹脊=行进砖，圆点阵列=提示砖）。
这些数值同时是 :func:`vision_assist.detectors.tactile.classify_tile` 阈值的依据，
一旦改动判据导致退化，这里会立刻报出来。
"""
from __future__ import annotations

import unittest
from pathlib import Path

import cv2
import numpy as np

from vision_assist.detectors.tactile import classify_tile
from vision_assist.sources import LocalVideoSource
from vision_assist.config import find_sample_video

ROOT = Path(__file__).resolve().parent.parent
TEST_VIDEO = find_sample_video() or (ROOT / "data" / "Test.mp4")  # 占位，is_file() 为 False

#: (帧号, 真值, 说明, 相对裁剪框 (y0,y1,x0,x1))
LABELS = [
    (0, "guide", "上部黄色条纹砖", (0.02, 0.20, 0.28, 0.68)),
    (0, "guide", "中部米黄条纹砖", (0.36, 0.56, 0.28, 0.68)),
    (0, "warning", "下部圆点砖", (0.85, 0.99, 0.05, 0.95)),
    (0, "warning", "中下圆点砖", (0.62, 0.78, 0.20, 0.80)),
    (240, "guide", "近处条纹砖", (0.45, 0.85, 0.30, 0.70)),
    (300, "guide", "中段条纹砖", (0.25, 0.55, 0.30, 0.70)),
    (420, "guide", "远段条纹砖(仅2条线)", (0.10, 0.35, 0.28, 0.72)),
    (540, "guide", "条纹砖", (0.30, 0.60, 0.30, 0.70)),
]


@unittest.skipUnless(TEST_VIDEO.is_file(), "缺少测试视频 Test.mp4")
class TestTileLabels(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = LocalVideoSource(str(TEST_VIDEO))
        cls.frames = {}
        try:
            for frame_no in sorted({row[0] for row in LABELS}):
                src._cap.set(cv2.CAP_PROP_POS_FRAMES, frame_no)
                frame = src.read()
                if frame is not None:
                    cls.frames[frame_no] = frame
        finally:
            src.close()

    def test_labeled_accuracy(self):
        correct = 0
        details = []
        for frame_no, truth, desc, (y0, y1, x0, x1) in LABELS:
            frame = self.frames.get(frame_no)
            if frame is None:
                continue
            h, w = frame.shape[:2]
            crop = frame[int(h * y0):int(h * y1), int(w * x0):int(w * x1)]
            if crop.size == 0:
                continue
            tile, conf, metrics = classify_tile(crop)
            hit = tile == truth
            correct += hit
            details.append(
                f"{'OK ' if hit else 'MISS'} gt={truth} got={tile} conf={conf:.2f} "
                f"n={metrics['n_lines']} par={metrics['parallel_ratio']:.2f} "
                f"L={metrics['max_len_ratio']:.2f}  {desc}"
            )
        print("\n" + "\n".join(details))
        self.assertGreaterEqual(correct, len(LABELS) - 1,
                                f"标注集准确率退化: {correct}/{len(LABELS)}\n" + "\n".join(details))

    def test_dot_tiles_are_not_guide(self):
        """提示砖绝不能被判成行进砖——这个错误会让盲人漏掉路口提示。"""
        for frame_no, truth, desc, (y0, y1, x0, x1) in LABELS:
            if truth != "warning":
                continue
            frame = self.frames.get(frame_no)
            if frame is None:
                continue
            h, w = frame.shape[:2]
            crop = frame[int(h * y0):int(h * y1), int(w * x0):int(w * x1)]
            tile, _conf, metrics = classify_tile(crop)
            self.assertEqual(tile, "warning",
                             f"提示砖被判成 {tile}（{desc}, 指标={metrics}）")


if __name__ == "__main__":
    unittest.main(verbosity=2)
