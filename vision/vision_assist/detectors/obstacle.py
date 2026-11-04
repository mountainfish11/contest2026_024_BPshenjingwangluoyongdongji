"""障碍物检测：复用 YOLOv8n-COCO 预训练权重。

为什么用 COCO 通用模型而不是自训
--------------------------------
1. 不需要标注数据即可覆盖"人、自行车、汽车、公交、卡车、狗、长椅、消防栓"等
   街头常见障碍；
2. COCO 自带 ``traffic light`` 类别，红绿灯检测可以复用它，不必单独训练；
3. 距离用地面反投影算，是真实米数，不是像素大小。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

import numpy as np

from ..config import COCO_MODEL_PATH, AppConfig, ObstacleThresholds
from .base import Box, Detector, FrameContext

#: COCO 里的红绿灯类别名
TRAFFIC_LIGHT_CLASS = "traffic light"


@dataclass
class Obstacle:
    label: str
    label_cn: str
    conf: float
    box: Box
    distance_m: Optional[float] = None
    lateral_m: Optional[float] = None

    def to_dict(self) -> dict:
        return {
            "label": self.label,
            "label_cn": self.label_cn,
            "conf": round(float(self.conf), 3),
            "box": [round(float(v), 1) for v in self.box],
            "distance_m": None if self.distance_m is None else round(self.distance_m, 3),
            "lateral_m": None if self.lateral_m is None else round(self.lateral_m, 3),
        }


@dataclass
class ObstacleResult:
    obstacles: List[Obstacle] = field(default_factory=list)
    nearest_m: Optional[float] = None

    def to_dict(self) -> dict:
        return {
            "count": len(self.obstacles),
            "nearest_m": None if self.nearest_m is None else round(self.nearest_m, 3),
            "items": [o.to_dict() for o in self.obstacles],
        }


def ground_from_box(camera, box: Box) -> Tuple[Optional[float], Optional[float]]:
    """用框底边中点求障碍物的地面距离与横向位置（米）。

    负值截断到 0：近景俯视时框底边可能落在相机后方，那在"距离"语义下等于"就在脚下"。
    """
    x_mid = (box[0] + box[2]) / 2.0
    point = camera.px2ground(x_mid, box[3])
    if point is None:
        return None, None
    x_m, z_m = float(point[0]), max(0.0, float(point[1]))
    return z_m, x_m


def build_obstacles(
    raw: Sequence[Tuple[Box, float, str]],
    camera,
    thresholds: Optional[ObstacleThresholds] = None,
) -> ObstacleResult:
    """把原始 COCO 检测结果整理成障碍物列表（按距离从近到远）。"""
    th = thresholds or ObstacleThresholds()
    result = ObstacleResult()
    frame_w = float(getattr(camera, "width", 0) or 0)
    frame_h = float(getattr(camera, "height", 0) or 0)
    frame_area = frame_w * frame_h
    bottom_limit = frame_h - float(th.ignore_bottom_margin_px)

    for box, conf, name in raw:
        if name not in th.class_map:
            continue
        if conf < th.conf:
            continue
        # 被画面底边截断的框几乎都是佩戴者自己的身体（腿/鞋）
        if frame_h > 0 and box[3] >= bottom_limit:
            continue

        box_w = max(0.0, box[2] - box[0])
        box_h = max(0.0, box[3] - box[1])
        # 单个障碍物占掉半屏：要么贴脸（下面会去掉），要么是误检
        if frame_area > 0 and (box_w * box_h) > th.max_area_ratio * frame_area:
            continue
        # 人站着是瘦高的，宽大于高基本是把大片纹理误认成行人
        if name == "person" and box_h > 0 and box_w > th.person_max_aspect * box_h:
            continue

        dist, lateral = ground_from_box(camera, box)
        # 太近的直接丢掉：脚下的东西提醒也来不及
        if dist is not None and dist < th.ignore_below_m:
            continue
        result.obstacles.append(Obstacle(
            label=name,
            label_cn=th.class_map[name],
            conf=float(conf),
            box=box,
            distance_m=dist,
            lateral_m=lateral,
        ))

    result.obstacles.sort(key=lambda o: (o.distance_m is None, o.distance_m or 0.0))
    valid = [o.distance_m for o in result.obstacles if o.distance_m is not None]
    result.nearest_m = min(valid) if valid else None
    return result


class CocoDetector(Detector):
    """跑一次 COCO 模型，结果同时供障碍物与红绿灯复用。"""

    name = "coco"

    def __init__(self, config: Optional[AppConfig] = None,
                 weights: Optional[str | Path] = None):
        super().__init__(config)
        self._weights = str(weights or COCO_MODEL_PATH)
        self._model = None

    def _ensure_model(self):
        if self._model is None:
            if not Path(self._weights).is_file():
                raise FileNotFoundError(
                    f"COCO 模型权重不存在: {self._weights}（预期位于 models/yolov8n.pt）"
                )
            from ultralytics import YOLO

            self._model = YOLO(self._weights)
        return self._model

    def detect_raw(self, frame: np.ndarray) -> List[Tuple[Box, float, str]]:
        """返回 ``[(box, conf, class_name), ...]``，不做类别过滤。"""
        model = self._ensure_model()
        # 阈值取两个消费方里较宽松的那个，过滤留给各自处理
        conf_thr = min(self.config.obstacle.conf, self.config.traffic_light.conf)
        results = model.predict(
            frame,
            conf=conf_thr,
            imgsz=self.config.imgsz_obstacle,
            device=self._device,
            verbose=False,
        )
        out: List[Tuple[Box, float, str]] = []
        for res in results:
            names = res.names
            for b in res.boxes:
                xyxy = b.xyxy[0].tolist()
                out.append((
                    (float(xyxy[0]), float(xyxy[1]), float(xyxy[2]), float(xyxy[3])),
                    float(b.conf[0]),
                    str(names[int(b.cls[0])]),
                ))
        return out

    def process(self, ctx: FrameContext) -> ObstacleResult:
        raw = self.detect_raw(ctx.frame)
        return build_obstacles(raw, ctx.camera, self.config.obstacle)

    def close(self) -> None:
        self._model = None
