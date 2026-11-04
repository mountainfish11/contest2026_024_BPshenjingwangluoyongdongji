"""画面质量评估：亮度、模糊、过曝。

盲人用户没法自己判断"是不是镜头糊了/逆光看不清了"，所以系统必须能说
"现在画面太暗/太模糊，识别可能不可靠"。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

import cv2
import numpy as np

from .config import QualityThresholds


@dataclass
class QualityResult:
    ok: bool = True
    brightness: float = 0.0
    blur: float = 0.0
    overexposure_ratio: float = 0.0
    warnings: List[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "ok": self.ok,
            "brightness": round(self.brightness, 1),
            "blur": round(self.blur, 1),
            "overexposure": round(self.overexposure_ratio, 3),
            "warnings": list(self.warnings),
        }


#: 质量评估用的最大边长；超过则先降采样。这些指标都是全局统计量，
#: 降采样几乎不影响判断，但能把 720p 上的耗时从 ~10ms 降到 ~2ms。
QUALITY_MAX_SIDE = 480


def assess_quality(
    frame: np.ndarray, th: Optional[QualityThresholds] = None
) -> QualityResult:
    """评估一帧的可识别性。"""
    th = th or QualityThresholds()
    if frame is None or frame.size == 0:
        return QualityResult(ok=False, warnings=["empty_frame"])

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY) if frame.ndim == 3 else frame
    longest = max(gray.shape[:2])
    if longest > QUALITY_MAX_SIDE:
        scale = QUALITY_MAX_SIDE / float(longest)
        gray = cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)

    brightness = float(gray.mean())
    # Laplacian 方差是经典的清晰度指标：越模糊值越小
    blur = float(cv2.Laplacian(gray, cv2.CV_64F).var())
    overexposure = float((gray >= 250).mean())

    warnings: List[str] = []
    if brightness < th.brightness_min:
        warnings.append("too_dark")
    if brightness > th.brightness_max:
        warnings.append("too_bright")
    if blur < th.blur_min:
        warnings.append("blurry")
    if overexposure > th.overexposure_max_ratio:
        warnings.append("overexposed")

    return QualityResult(
        ok=not warnings,
        brightness=brightness,
        blur=blur,
        overexposure_ratio=overexposure,
        warnings=warnings,
    )
