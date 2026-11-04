"""盲道检测器：复用 YOLOv8 盲道模型 + LSD 长线段判别行进砖/提示砖。

输出的是**度量空间**的量（米 / 度），而不是像素斜率——这是与参照实现
（``routes/video.py`` 里的 ``polyfit(ys→xs)`` 斜率法）最本质的区别。

判定流程
--------
1. YOLO 检出盲道框；
2. 在每个框内用"框内颜色中位值"做自适应分割，得到盲道像素而非整个矩形；
3. 把盲道像素反投影到地面，得到真实的 (X, Z) 米；
4. 近场窗口内的 X 中位数 → 横向偏移；X 对 Z 的鲁棒线性拟合斜率 → 航向角；
5. 取最大的框做 LSD 长线段统计 → 行进砖 / 提示砖。
"""
from __future__ import annotations

import math
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

import cv2
import numpy as np

from ..config import TACTILE_MODEL_PATH, AppConfig, TactileThresholds
from .base import Box, Detector, FrameContext, subsample_points

#: 近场窗口长度（米）：在这个范围内取横向偏移
NEAR_WINDOW_M = 1.2
#: 参与拟合的地面距离上限（米），太远的部分像素太少、噪声大
FIT_Z_MAX_M = 8.0
#: 地面点有效性下限（米）：相机正下方及更近处不参与
FIT_Z_MIN_M = 0.05
#: 颜色分割的最大处理面积（像素）；超过则按整数倍降采样，控制单帧耗时
MAX_MASK_AREA = 60_000
#: 纹理判别裁剪的最长边（像素）；LSD 耗时随像素数线性增长
MAX_TILE_SIDE = 320
#: 逐框做纹理判别的最大框数（控制单帧耗时）
MAX_TILE_BOXES = 3


# --------------------------------------------------------------------------- #
# 结果结构
# --------------------------------------------------------------------------- #
@dataclass
class TactileBox:
    """单个盲道框的详细信息。"""

    box: Box
    conf: float
    tile_type: str = "unknown"
    tile_type_conf: float = 0.0
    near_m: Optional[float] = None
    far_m: Optional[float] = None

    def to_dict(self) -> dict:
        return {
            "box": [round(float(v), 1) for v in self.box],
            "conf": round(float(self.conf), 3),
            "tile_type": self.tile_type,
            "tile_type_conf": round(float(self.tile_type_conf), 3),
            "near_m": None if self.near_m is None else round(self.near_m, 3),
            "far_m": None if self.far_m is None else round(self.far_m, 3),
        }


@dataclass
class TactileResult:
    present: bool = False
    confidence: float = 0.0
    boxes: List[Box] = field(default_factory=list)
    #: 脚下（最近）那个框的砖型
    tile_type: str = "unknown"          # guide / warning / unknown
    tile_type_conf: float = 0.0
    lateral_offset_m: Optional[float] = None   # 负=偏左，正=偏右
    heading_deg: Optional[float] = None        # 正=盲道向前方右侧偏
    near_m: Optional[float] = None
    far_m: Optional[float] = None
    #: 前方最近的提示砖距离（米）；没有"前方"提示砖时为 None（脚下提示砖不计）
    warning_near_m: Optional[float] = None
    #: 是否正踩在提示砖上（位置事实，不触发"前方有提示砖"警报）
    on_warning_tile: bool = False
    boxes_detail: List[TactileBox] = field(default_factory=list)
    centerline: List[Tuple[float, float]] = field(default_factory=list)  # [(X,Z), ...]
    ground_point_count: int = 0
    tile_metrics: dict = field(default_factory=dict)

    def to_dict(self) -> dict:
        def r(v, nd=3):
            return None if v is None else round(float(v), nd)

        return {
            "present": self.present,
            "confidence": r(self.confidence),
            "tile_type": self.tile_type,
            "tile_type_conf": r(self.tile_type_conf),
            "lateral_offset_m": r(self.lateral_offset_m),
            "heading_deg": r(self.heading_deg),
            "near_m": r(self.near_m),
            "far_m": r(self.far_m),
            "warning_near_m": r(self.warning_near_m),
            "on_warning_tile": self.on_warning_tile,
            "ground_point_count": self.ground_point_count,
        }


# --------------------------------------------------------------------------- #
# 纯函数部分（可脱离模型单独测试）
# --------------------------------------------------------------------------- #
def color_mask_in_box(
    frame: np.ndarray,
    box: Box,
    color_tol: float = 22.0,
    morph: bool = True,
) -> Tuple[np.ndarray, np.ndarray]:
    """在给定框内做自适应颜色分割。

    以框内像素的 **Lab 中位值** 作为盲道主色，选出与之足够接近的像素。
    这样不依赖"盲道一定是某一种黄色"这种硬编码假设（国内既有标准亮黄，也有偏
    米黄的老砖）。

    返回 ``(像素坐标 (行, 列) 相对全图的两个数组, 二值掩码)``。
    """
    h, w = frame.shape[:2]
    x1 = max(0, min(w - 1, int(round(box[0]))))
    y1 = max(0, min(h - 1, int(round(box[1]))))
    x2 = max(0, min(w, int(round(box[2]))))
    y2 = max(0, min(h, int(round(box[3]))))
    if x2 - x1 < 4 or y2 - y1 < 4:
        return np.empty(0, np.int32), np.empty(0, np.int32), np.zeros((0, 0), np.uint8)

    crop = frame[y1:y2, x1:x2]
    area = (x2 - x1) * (y2 - y1)
    scale = 1
    if area > MAX_MASK_AREA:
        scale = int(math.ceil(math.sqrt(area / float(MAX_MASK_AREA))))
    work = crop if scale == 1 else cv2.resize(
        crop, None, fx=1.0 / scale, fy=1.0 / scale, interpolation=cv2.INTER_AREA
    )

    lab = cv2.cvtColor(work, cv2.COLOR_BGR2Lab).astype(np.float32)
    median = np.median(lab.reshape(-1, 3), axis=0)
    dist = np.linalg.norm(lab - median.reshape(1, 1, 3), axis=2)
    mask = (dist <= color_tol).astype(np.uint8) * 255
    if morph:
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
        mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)

    rows, cols = np.nonzero(mask)
    # 还原到全图坐标
    return ((rows * scale + y1).astype(np.int32),
            (cols * scale + x1).astype(np.int32),
            mask)


def robust_line_fit(z: np.ndarray, x: np.ndarray) -> Tuple[float, int]:
    """对 ``X = a*Z + b`` 做一次带离群剔除的鲁棒拟合，返回 ``(a, 用到的点数)``。"""
    if z.size < 3:
        return 0.0, int(z.size)
    # Z 几乎没有跨度时斜率没有意义，而且 np.polyfit 会报 RankWarning 刷屏
    # （近景俯视时容易发生：所有地面点挤在 0.05–0.2 m 这一小段里）
    if float(np.ptp(z)) < 1e-3:
        return 0.0, int(z.size)

    rank_warning = getattr(getattr(np, "exceptions", np), "RankWarning")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", rank_warning)
        a, b = np.polyfit(z, x, 1)
        resid = np.abs(x - (a * z + b))
        mad = float(np.median(resid))
        keep = resid <= max(3.0 * mad, 1e-3)
        if keep.sum() >= 3 and float(np.ptp(z[keep])) >= 1e-3:
            a, _b = np.polyfit(z[keep], x[keep], 1)
            return float(a), int(keep.sum())
    return float(a), int(z.size)


def classify_tile(
    crop: np.ndarray, th: Optional[TactileThresholds] = None
) -> Tuple[str, float, dict]:
    """用 LSD 长线段统计判别行进砖(guide)/提示砖(warning)。

    依据（对 Test.mp4 实测审计）：
    - 行进砖是长条状平行脊：长线段 **5–35 条**，且方向高度一致；
    - 提示砖是圆点阵列：长线段只有 **1–2 条**（圆点边缘太短，过不了长度门槛）。

    注意 ``max_len_ratio``（最长线段占对角线比例）在同一段行进砖上会随视角在
    0.18–0.71 间波动，因此它**只参与置信度计算，不作为硬门限**——早期版本把它
    当硬门限，导致 16 个全为行进砖的样本里有 6 个被误判成 unknown。
    """
    th = th or TactileThresholds()
    metrics: dict = {
        "n_lines": 0,
        "max_len_ratio": 0.0,
        "mean_len_ratio": 0.0,
        "parallel_ratio": 0.0,
        "median_angle_deg": None,
    }
    if crop is None or crop.size == 0 or crop.shape[0] < 16 or crop.shape[1] < 16:
        metrics["reason"] = "crop_too_small"
        return "unknown", 0.0, metrics

    # LSD 耗时随像素数线性增长，先限制最长边；线段门槛是相对对角线定义的，
    # 因此等比缩放不改变判别结论。
    h0, w0 = crop.shape[:2]
    if max(h0, w0) > MAX_TILE_SIDE:
        s = MAX_TILE_SIDE / float(max(h0, w0))
        crop = cv2.resize(crop, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)

    gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
    gray = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)

    h, w = gray.shape[:2]
    diag = math.hypot(w, h)
    try:
        lines = cv2.createLineSegmentDetector(cv2.LSD_REFINE_STD).detect(gray)[0]
    except cv2.error:
        lines = None

    lengths: List[float] = []
    angles: List[float] = []
    if lines is not None:
        for seg in np.asarray(lines).reshape(-1, 4):
            x1, y1, x2, y2 = seg
            length = math.hypot(x2 - x1, y2 - y1)
            if length < th.line_len_ratio * diag:
                continue
            lengths.append(length)
            angles.append(math.degrees(math.atan2(y2 - y1, x2 - x1)) % 180.0)

    n_lines = len(lengths)
    if n_lines:
        ratios = np.array(lengths) / diag
        max_len_ratio = float(ratios.max())
        mean_len_ratio = float(ratios.mean())
        med_angle = float(np.median(angles))
        diff = np.abs(((np.asarray(angles) - med_angle + 90.0) % 180.0) - 90.0)
        parallel_ratio = float((diff <= th.parallel_angle_tol_deg).mean())
    else:
        max_len_ratio = mean_len_ratio = parallel_ratio = 0.0
        med_angle = None

    metrics.update(
        n_lines=n_lines,
        max_len_ratio=round(max_len_ratio, 3),
        mean_len_ratio=round(mean_len_ratio, 3),
        parallel_ratio=round(parallel_ratio, 3),
        median_angle_deg=None if med_angle is None else round(med_angle, 1),
    )

    # ---- 主判据：长线段条数
    if n_lines >= th.guide_min_lines:
        conf = 0.55
        conf += 0.20 * min(1.0, (n_lines - th.guide_min_lines) / 12.0)
        conf += 0.15 * parallel_ratio
        conf += 0.10 * min(1.0, max_len_ratio / 0.5)
        return "guide", round(min(0.99, conf), 3), metrics

    if n_lines <= th.warning_max_lines:
        # 线段极少时，"长度"才是有区分力的信息：
        # 圆点的边缘碎片很短（实测 L≈0.14–0.20），远处条纹砖残留的长脊明显更长
        # （实测 L≈0.42）。此时给低置信度，交给时序确认，而不是硬判。
        if max_len_ratio >= th.guide_min_maxlen:
            return "guide", 0.45, metrics
        conf = 0.55
        conf += 0.20 * min(1.0, (th.warning_max_lines - n_lines) / 2.0)
        conf += 0.15 * (1.0 - min(1.0, max_len_ratio / 0.5))
        return "warning", round(min(0.99, conf), 3), metrics

    # ---- 中间地带（3–4 条）：靠平行度和平均长度决断，并压低置信度表明不确定
    if parallel_ratio >= 0.6 and mean_len_ratio >= 0.25:
        return "guide", 0.40, metrics
    return "warning", 0.40, metrics


def ground_from_mask(
    camera,
    u: np.ndarray,
    v: np.ndarray,
    z_min_m: float = FIT_Z_MIN_M,
    z_max_m: float = FIT_Z_MAX_M,
    near_window_m: float = NEAR_WINDOW_M,
) -> dict:
    """把盲道像素反投影成地面点，并算出横向偏移 / 航向角 / 远近范围。"""
    out = {
        "lateral_offset_m": None,
        "heading_deg": None,
        "near_m": None,
        "far_m": None,
        "centerline": [],
        "count": 0,
    }
    if u.size == 0:
        return out

    X, Z, valid = camera.px2ground_batch(u.astype(np.float64), v.astype(np.float64))
    valid &= np.isfinite(X) & np.isfinite(Z) & (Z >= z_min_m) & (Z <= z_max_m) & (np.abs(X) <= 6.0)
    if not np.any(valid):
        return out
    X = X[valid]
    Z = Z[valid]
    out["count"] = int(X.size)
    out["near_m"] = float(np.percentile(Z, 3))
    out["far_m"] = float(np.percentile(Z, 97))

    # 近场横向偏移
    near_hi = float(np.percentile(Z, 3)) + near_window_m
    near_sel = Z <= near_hi
    if near_sel.sum() >= 3:
        out["lateral_offset_m"] = float(np.median(X[near_sel]))

    # 航向角：X = a*Z + b
    a, used = robust_line_fit(Z, X)
    if used >= 3:
        out["heading_deg"] = float(math.degrees(math.atan(a)))

    # 中心线（用于叠加显示）：按 Z 分箱取 X 中位数
    n_bins = 12
    z_lo, z_hi = float(Z.min()), float(Z.max())
    if z_hi - z_lo > 1e-3:
        edges = np.linspace(z_lo, z_hi, n_bins + 1)
        pts: List[Tuple[float, float]] = []
        for i in range(n_bins):
            sel = (Z >= edges[i]) & (Z < edges[i + 1] if i < n_bins - 1 else Z <= edges[i + 1])
            if sel.sum() >= 2:
                pts.append((float(np.median(X[sel])), float(np.median(Z[sel]))))
        out["centerline"] = pts
    return out


def pick_tile_crop(frame: np.ndarray, boxes: Sequence[Box]) -> Optional[np.ndarray]:
    """从所有盲道框中挑一个用于纹理判别的裁剪（选面积最大的）。"""
    if not boxes:
        return None
    h, w = frame.shape[:2]
    best = None
    best_area = 0.0
    for box in boxes:
        x1 = max(0, min(w - 1, int(round(box[0]))))
        y1 = max(0, min(h - 1, int(round(box[1]))))
        x2 = max(0, min(w, int(round(box[2]))))
        y2 = max(0, min(h, int(round(box[3]))))
        area = float((x2 - x1) * (y2 - y1))
        if area > best_area and x2 - x1 >= 24 and y2 - y1 >= 24:
            best_area = area
            best = (y1, y2, x1, x2)
    if best is None:
        return None
    y1, y2, x1, x2 = best
    return frame[y1:y2, x1:x2]


def crop_for_box(frame: np.ndarray, box: Box,
                 min_side: int = 24) -> Optional[np.ndarray]:
    """裁剪单个框；太小（远处）时返回 ``None``，避免在像素不足时瞎猜砖型。"""
    h, w = frame.shape[:2]
    x1 = max(0, min(w - 1, int(round(box[0]))))
    y1 = max(0, min(h - 1, int(round(box[1]))))
    x2 = max(0, min(w, int(round(box[2]))))
    y2 = max(0, min(h, int(round(box[3]))))
    if x2 - x1 < min_side or y2 - y1 < min_side:
        return None
    return frame[y1:y2, x1:x2]


def box_near_far(camera, box: Box, samples: int = 5) -> Tuple[Optional[float], Optional[float]]:
    """框的近处 / 远处地面距离（米）。

    沿框中轴线取若干采样点反投影。**注意**：近景俯视（如手机近乎垂直向下拍）
    时，框的底边会投影到相机后方，算出的 Z 是负数——这在"距离"语义里等价于
    "就在脚下 0 米"，因此在这里统一截断到 0，绝不把负距离传给上层。
    """
    x_mid = (box[0] + box[2]) / 2.0
    zs: List[float] = []
    for y in np.linspace(box[1], box[3], samples):
        point = camera.px2ground(x_mid, float(y))
        if point is not None:
            zs.append(float(point[1]))
    if not zs:
        return None, None
    arr = np.maximum(np.asarray(zs, dtype=np.float64), 0.0)
    return float(arr.min()), float(arr.max())


# --------------------------------------------------------------------------- #
# 检测器
# --------------------------------------------------------------------------- #
class TactileDetector(Detector):
    """复用预训练 YOLOv8 盲道模型，输出度量空间的盲道信息。"""

    name = "tactile"

    def __init__(self, config: Optional[AppConfig] = None,
                 weights: Optional[str | Path] = None):
        super().__init__(config)
        self._weights = str(weights or TACTILE_MODEL_PATH)
        self._model = None

    def _ensure_model(self):
        if self._model is None:
            # 先检查权重再导入 ultralytics：缺权重时能快速失败，不必加载 torch
            if not Path(self._weights).is_file():
                raise FileNotFoundError(
                    f"盲道模型权重不存在: {self._weights}（预期位于 models/blind_path.pt）"
                )
            from ultralytics import YOLO  # 延迟导入，纯几何测试不需要它

            self._model = YOLO(self._weights)
        return self._model

    def detect_boxes(self, frame: np.ndarray) -> List[Tuple[Box, float, int]]:
        model = self._ensure_model()
        results = model.predict(
            frame,
            conf=self.config.tactile.conf,
            imgsz=self.config.imgsz,
            device=self._device,
            verbose=False,
        )
        out: List[Tuple[Box, float, int]] = []
        for res in results:
            names = res.names
            for b in res.boxes:
                xyxy = b.xyxy[0].tolist()
                out.append((
                    (float(xyxy[0]), float(xyxy[1]), float(xyxy[2]), float(xyxy[3])),
                    float(b.conf[0]),
                    int(b.cls[0]),
                ))
        return out

    def process(self, ctx: FrameContext) -> TactileResult:
        th = self.config.tactile
        dets = self.detect_boxes(ctx.frame)
        result = TactileResult()
        if not dets:
            return result

        result.present = True
        result.confidence = max(d[1] for d in dets)
        result.boxes = [d[0] for d in dets]

        # ---- 颜色自适应分割 → 地面点
        us: List[np.ndarray] = []
        vs: List[np.ndarray] = []
        for box, _conf, _cls in dets:
            rows, cols, _mask = color_mask_in_box(ctx.frame, box)
            if rows.size:
                us.append(cols)
                vs.append(rows)
        if us:
            u = np.concatenate(us)
            v = np.concatenate(vs)
            u, v = subsample_points(u, v, stride=2)
            geo = ground_from_mask(ctx.camera, u, v)
        else:
            geo = {"lateral_offset_m": None, "heading_deg": None, "near_m": None,
                   "far_m": None, "centerline": [], "count": 0}

        # 掩码为空时退化为"框底边中点"，保证单框场景也有输出
        if geo["near_m"] is None:
            bottoms = []
            for box in result.boxes:
                g = ctx.camera.px2ground((box[0] + box[2]) / 2.0, box[3])
                if g is not None:
                    bottoms.append(g)
            if bottoms:
                Xs = np.array([g[0] for g in bottoms])
                Zs = np.array([g[1] for g in bottoms])
                geo["near_m"] = float(Zs.min())
                geo["far_m"] = float(Zs.max())
                geo["lateral_offset_m"] = float(np.median(Xs))
                geo["count"] = len(bottoms)

        result.lateral_offset_m = geo["lateral_offset_m"]
        result.heading_deg = geo["heading_deg"]
        result.near_m = geo["near_m"]
        result.far_m = geo["far_m"]
        result.centerline = geo["centerline"]
        result.ground_point_count = geo["count"]

        # ---- 逐框判别砖型
        # 一帧里可能同时出现行进砖和提示砖，所以不能只挑"最大的框"：
        #   * 脚下是什么砖 → 取最近的框（底边最靠画面下方）
        #   * 前方多远有提示砖 → 取距离最小的提示砖框
        order = sorted(range(len(dets)), key=lambda i: -dets[i][0][3])
        detail: List[TactileBox] = []
        for rank, i in enumerate(order):
            box, conf, _cls = dets[i]
            near_z, far_z = box_near_far(ctx.camera, box)
            item = TactileBox(box=box, conf=conf, near_m=near_z, far_m=far_z)
            if rank < MAX_TILE_BOXES:
                crop = crop_for_box(ctx.frame, box)
                if crop is not None:
                    tile, tconf, metrics = classify_tile(crop, th)
                    item.tile_type = tile
                    item.tile_type_conf = tconf
                    if rank == 0:
                        result.tile_metrics = metrics
            detail.append(item)
        result.boxes_detail = detail

        # 脚下方砖型：优先取最近且能判定的那个框
        classified = [d for d in detail if d.tile_type != "unknown"]
        chosen = classified[0] if classified else (detail[0] if detail else None)
        if chosen is not None:
            result.tile_type = chosen.tile_type
            result.tile_type_conf = chosen.tile_type_conf

        # 前方最近的提示砖：路口/台阶/站台边缘的关键预警信号。
        # 近于 underfoot 阈值的算"脚下"，不参与"前方"距离，避免站在提示砖上持续误报。
        underfoot = float(th.warning_tile_underfoot_m)
        ahead_dists: List[float] = []
        for d in detail:
            if d.tile_type != "warning" or d.near_m is None:
                continue
            if d.near_m < underfoot:
                result.on_warning_tile = True
            else:
                ahead_dists.append(float(d.near_m))
        result.warning_near_m = min(ahead_dists) if ahead_dists else None
        return result

    def close(self) -> None:
        self._model = None
