"""ESP32 固件补丁的测试：补丁文件存在、内容正确、能被 git apply 打上。"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
from vision_assist.config import find_reference_ino  # noqa: E402
PATCH = ROOT / "patches" / "esp32_vga.patch"
ORIGINAL = find_reference_ino() or (ROOT / "esp32" / "_原始参考" / "camera_stream_download.ino")

EXPECTED_REPLACEMENTS = [
    ("c.xclk_freq_hz = 10000000;", "c.xclk_freq_hz = 20000000;"),
    ("c.frame_size   = FRAMESIZE_QVGA;", "c.frame_size   = FRAMESIZE_VGA;"),
    ("c.jpeg_quality = 12;", "c.jpeg_quality = 10;"),
    ("c.fb_count     = 1;", "c.fb_count     = 2;"),
]


class TestPatchFile(unittest.TestCase):
    def test_patch_exists(self):
        self.assertTrue(PATCH.is_file(), f"补丁缺失: {PATCH}")

    def test_patch_uses_lf_line_endings(self):
        """原 .ino 是 LF；补丁若写成 CRLF 会导致 git apply 匹配失败。"""
        data = PATCH.read_bytes()
        self.assertEqual(data.count(b"\r\n"), 0, "补丁文件不应包含 CRLF")

    def test_patch_touches_only_camera_config(self):
        text = PATCH.read_text(encoding="utf-8")
        for old, _new in EXPECTED_REPLACEMENTS:
            self.assertIn(old.lstrip("c").strip(), text,
                          f"补丁里找不到要替换的原始配置: {old}")

    @unittest.skipUnless(ORIGINAL.is_file(), "缺少原始 .ino")
    def test_patch_applies_and_produces_expected_content(self):
        git = shutil.which("git")
        if git is None:
            self.skipTest("环境里没有 git")

        expected = ORIGINAL.read_bytes().decode("utf-8").replace("\r\n", "\n")
        for old, new in EXPECTED_REPLACEMENTS:
            self.assertIn(old, expected, f"原始文件里找不到: {old}")
            expected = expected.replace(old, new)

        with tempfile.TemporaryDirectory(prefix="vision_patch_") as temp_dir:
            work = Path(temp_dir) / "camera_stream_download"
            work.mkdir(parents=True)
            target = work / "camera_stream_download.ino"
            target.write_bytes(ORIGINAL.read_bytes())
            subprocess.run([git, "apply", "--check", str(PATCH)],
                           cwd=work.parent, check=True,
                           capture_output=True, text=True)
            subprocess.run([git, "apply", str(PATCH)],
                           cwd=work.parent, check=True,
                           capture_output=True, text=True)
            # git 在 Windows 上可能按 core.autocrlf 转换行尾，比较时忽略行尾差异
            got = target.read_bytes().decode("utf-8").replace("\r\n", "\n")
            self.assertEqual(got, expected, "打补丁后的内容与预期不符")


class TestBaselineParity(unittest.TestCase):
    """确认基线复刻与参照实现的判据一致。"""

    def test_threshold_matches_reference(self):
        from eval.baseline import THRESHOLD_SLOPE
        self.assertAlmostEqual(THRESHOLD_SLOPE, 0.41, places=6)

    def test_requires_two_boxes(self):
        from eval.baseline import judge
        self.assertEqual(judge([]).direction, "none")
        self.assertEqual(judge([(100.0, 100.0)]).direction, "none")

    def test_slope_sign_semantics(self):
        """cx 随 cy 增大而减小 → 负斜率 → 参照实现判"左转"。"""
        from eval.baseline import judge
        # 越靠下(y 大) 的框中心越靠左(x 小)
        self.assertEqual(judge([(300.0, 100.0), (200.0, 300.0), (100.0, 500.0)]).direction, "left")
        self.assertEqual(judge([(100.0, 100.0), (200.0, 300.0), (300.0, 500.0)]).direction, "right")

    def test_flat_is_straight(self):
        from eval.baseline import judge
        self.assertEqual(judge([(200.0, 100.0), (200.0, 300.0)]).direction, "straight")


if __name__ == "__main__":
    unittest.main(verbosity=2)
