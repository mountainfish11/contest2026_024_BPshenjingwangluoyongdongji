"""叠加绘制：把检测结果画回画面上，供调试页与离线导出使用。

注意：绘制是**兜底**手段。真正的调试页会在浏览器端用 canvas 叠加，
这样不会拖慢主循环；这里的函数用于离线导图和快照。
"""
from __future__ import annotations

from typing import List, Optional, Sequence, Tuple

import cv2
import numpy as np

from .detectors.tactile import TactileResult
from .geometry import CameraModel

GUIDE_COLOR = (60, 200, 255)     # 行进砖：橙黄
WARNING_COLOR = (60, 60, 255)    # 提示砖：红
UNKNOWN_COLOR = (200, 200, 200)
BOX_COLOR = (80, 220, 80)
CENTER_COLOR = (255, 200, 60)
OBSTACLE_COLOR = (255, 160, 60)


def _put(img: np.ndarray, text: str, org: Tuple[int, int],
         color=(255, 255, 255), scale: float = 0.55,
         thickness: Optional[int] = None) -> None:
    """带描边的文字，保证在任何背景上都看得清。

    描边粗细随字号自适应：小字号用粗描边会把字糊成一团（实测多行时互相压字）。
    """
    if thickness is None:
        thickness = max(1, int(round(scale * 2.5)))
    cv2.putText(img, text, org, cv2.FONT_HERSHEY_SIMPLEX, scale,
                (0, 0, 0), thickness + 2, cv2.LINE_AA)
    cv2.putText(img, text, org, cv2.FONT_HERSHEY_SIMPLEX, scale,
                color, thickness, cv2.LINE_AA)


def _panel(img: np.ndarray, x: int, y: int, w: int, h: int,
           alpha: float = 0.55) -> None:
    """画一块半透明深色底，提升文字可读性。"""
    x0, y0 = max(0, x), max(0, y)
    x1 = min(img.shape[1], x + w)
    y1 = min(img.shape[0], y + h)
    if x1 <= x0 or y1 <= y0:
        return
    roi = img[y0:y1, x0:x1]
    dark = np.zeros_like(roi)
    cv2.addWeighted(dark, alpha, roi, 1.0 - alpha, 0, roi)


def tile_display_name(tile_type: str) -> str:
    return {"guide": "行进砖", "warning": "提示砖"}.get(tile_type, "未知")


def draw_tactile(
    frame: np.ndarray,
    result: TactileResult,
    camera: Optional[CameraModel] = None,
) -> np.ndarray:
    """把盲道检测结果画到画面上（就地修改并返回）。"""
    img = frame
    h, w = img.shape[:2]

    color = {"guide": GUIDE_COLOR, "warning": WARNING_COLOR}.get(
        result.tile_type, UNKNOWN_COLOR
    )

    for box in result.boxes:
        x1 = int(round(box[0])); y1 = int(round(box[1]))
        x2 = int(round(box[2])); y2 = int(round(box[3]))
        cv2.rectangle(img, (x1, y1), (x2, y2), BOX_COLOR, 2)

    # 逐框标注砖型（同帧可能同时有行进砖和提示砖）
    for item in getattr(result, "boxes_detail", []) or []:
        if item.tile_type == "unknown":
            continue
        bx1 = int(round(item.box[0])); by1 = int(round(item.box[1]))
        color = GUIDE_COLOR if item.tile_type == "guide" else WARNING_COLOR
        _put(img, f"{tile_display_name(item.tile_type)} {item.tile_type_conf:.2f}",
             (max(4, bx1), max(16, by1 - 6)), color, 0.45)

    # 中心线（地面点投回画面）
    if camera is not None and result.centerline:
        pts = camera.ground_polyline_to_px(np.asarray(result.centerline, dtype=np.float64))
        valid = ~np.isnan(pts).any(axis=1)
        if valid.sum() >= 2:
            p = pts[valid].round().astype(np.int32)
            cv2.polylines(img, [p], False, CENTER_COLOR, 3, cv2.LINE_AA)

    # 指标面板
    lines: List[str] = []
    if result.present:
        lines.append(f"tactile conf={result.confidence:.2f}  tile={result.tile_type}")
        if result.lateral_offset_m is not None:
            lines.append(f"offset={result.lateral_offset_m:+.2f}m")
        if result.heading_deg is not None:
            lines.append(f"heading={result.heading_deg:+.1f}deg")
        if result.near_m is not None and result.far_m is not None:
            lines.append(f"Z={result.near_m:.2f}..{result.far_m:.2f}m")
        if getattr(result, "warning_near_m", None) is not None:
            lines.append(f"WARNING TILE ahead {result.warning_near_m:.2f}m")
        elif getattr(result, "on_warning_tile", False):
            lines.append("ON WARNING TILE (脚下)")
    else:
        lines.append("NO TACTILE PATH")

    line_h = 26
    scale = 0.62
    block_h = line_h * len(lines) + 8
    panel_w = 8 + int(max(len(t) for t in lines) * scale * 14) + 12
    _panel(img, 0, h - block_h - 4, panel_w, block_h + 4, alpha=0.6)

    y = h - 10 - line_h * (len(lines) - 1)
    for text in lines:
        _put(img, text, (8, y), (255, 255, 255), scale)
        y += line_h
    return img


def draw_obstacles(frame: np.ndarray, result) -> np.ndarray:
    """画障碍物框与距离。"""
    if result is None:
        return frame
    for ob in getattr(result, "obstacles", []) or []:
        x1, y1, x2, y2 = (int(round(v)) for v in ob.box)
        cv2.rectangle(frame, (x1, y1), (x2, y2), OBSTACLE_COLOR, 2)
        dist = "-" if ob.distance_m is None else f"{ob.distance_m:.1f}m"
        _put(frame, f"{ob.label_cn} {dist}", (max(4, x1), max(16, y2 + 16)),
             OBSTACLE_COLOR, 0.5)
    return frame


def draw_perception(frame: np.ndarray, scene) -> np.ndarray:
    """右下角画斑马线 / 红绿灯 / 台阶的识别结果。"""
    lines: List[str] = []
    cw = getattr(scene, "crosswalk", None)
    if cw is not None and getattr(cw, "present", False):
        lines.append(f"CROSSWALK bars={cw.bar_count} conf={cw.confidence:.2f}")
    tl = getattr(scene, "traffic_light", None)
    if tl is not None and getattr(tl, "present", False):
        lines.append(f"LIGHT {tl.state} conf={tl.conf:.2f}")
    st = getattr(scene, "stairs", None)
    if st is not None and getattr(st, "suspected", False):
        d = "" if st.distance_m is None else f" {st.distance_m:.1f}m"
        lines.append(f"STAIRS?{d} conf={st.confidence:.2f}")
    ob = getattr(scene, "obstacles", None)
    if ob is not None and getattr(ob, "nearest_m", None) is not None:
        lines.append(f"obstacles={len(ob.obstacles)} nearest={ob.nearest_m:.1f}m")

    if not lines:
        return frame
    h, w = frame.shape[:2]
    y = h - 12 - 20 * (len(lines) - 1)
    for text in lines:
        _put(frame, text, (max(4, w - 8 - 10 * len(text)), y), (180, 200, 255), 0.5)
        y += 20
    return frame


def draw_quality(frame: np.ndarray, quality, origin: Optional[Tuple[int, int]] = None) -> np.ndarray:
    """在右上角画画面质量提示（只在有问题时显示醒目信息）。"""
    if quality is None or quality.ok:
        return frame
    h, w = frame.shape[:2]
    text = "画面质量: " + ", ".join(quality.warnings)
    x = origin[0] if origin else max(4, w - 8 - 11 * len(text))
    y = origin[1] if origin else 24
    _put(frame, text, (x, y), (80, 80, 255), 0.6)
    return frame


#: 各状态对应的显示颜色（BGR）
STATE_COLORS = {
    "ALIGNED": (120, 230, 120),
    "DRIFT_LEFT": (80, 200, 255),
    "DRIFT_RIGHT": (80, 200, 255),
    "TURN_LEFT": (60, 130, 255),
    "TURN_RIGHT": (60, 130, 255),
    "PATH_LOST": (80, 80, 255),
    "WARNING_TILE": (60, 60, 255),
    "NO_PATH": (200, 200, 200),
}


def draw_state(frame: np.ndarray, scene) -> np.ndarray:
    """在画面顶部画状态条、警报与当前播报文本。"""
    state = getattr(scene, "state", "NO_PATH")
    color = STATE_COLORS.get(state, (220, 220, 220))
    h, w = frame.shape[:2]

    # 状态条
    cv2.rectangle(frame, (0, 0), (w, 34), (25, 25, 28), -1)
    _put(frame, state, (10, 24), color, 0.7)
    reason = getattr(scene, "state_reason", "") or ""
    if reason:
        _put(frame, reason[:44], (170, 24), (200, 200, 200), 0.5)

    # 警报：右上角列出所有活动警报
    alerts = getattr(scene, "alerts", None)
    if alerts is not None and alerts.alerts:
        x = max(0, w - 330)
        for i, alert in enumerate(sorted(alerts.alerts, key=lambda a: -a.priority)[:3]):
            dist = "" if alert.distance_m is None else f" {alert.distance_m:.2f}m"
            _put(frame, f"! {alert.kind}{dist}", (x, 24 + i * 20),
                 (80, 120, 255), 0.5)
    elif alerts is not None and getattr(alerts, "on_warning_tile", False):
        _put(frame, "on warning tile", (max(0, w - 190), 24), (120, 200, 255), 0.5)

    # 播报文本：紧贴状态条下方，避免与左下角的指标面板重叠
    speech = getattr(scene, "speech", None)
    if speech is not None and speech.spoke and speech.text:
        cv2.rectangle(frame, (0, 36), (w, 68), (25, 25, 28), -1)
        _put(frame, f">> {speech.text}", (10, 60), (120, 255, 180), 0.62)
    return frame


class GroundGridRenderer:
    """地面网格的**缓存绘制器**。

    网格只跟帧尺寸和标定参数有关，逐帧重画 13 条折线 + 十几个标签，实测要 8–12 ms，
    在 640×480、目标 20 FPS 的预算里是很大一块。这里把网格预渲染成一张跟背景等大的
    覆盖层，每帧只做一次 ``np.copyto`` 合成（约 0.3 ms）。
    """

    def __init__(self) -> None:
        self._key = None
        self._overlay: Optional[np.ndarray] = None
        self._mask: Optional[np.ndarray] = None

    @staticmethod
    def _make_key(frame: np.ndarray, camera) -> tuple:
        calib = getattr(camera, "calib", None)
        return (
            int(frame.shape[0]), int(frame.shape[1]),
            round(float(camera.fx), 3), round(float(camera.cx), 3),
            round(float(camera.cy), 3), round(float(camera.h), 4),
            round(float(camera.pitch), 6),
            None if calib is None else round(float(calib.pitch_deg), 4),
        )

    def render(self, frame: np.ndarray, camera) -> np.ndarray:
        if camera is None:
            return frame
        key = self._make_key(frame, camera)
        if key != self._key or self._overlay is None:
            canvas = np.zeros_like(frame)
            draw_ground_grid(canvas, camera)
            self._overlay = canvas
            self._mask = canvas.any(axis=2)
            self._key = key
        # 用布尔索引合成：2D 掩码对 3D 数组不能直接 np.copyto（where 不广播）
        frame[self._mask] = self._overlay[self._mask]
        return frame

    def invalidate(self) -> None:
        self._key = None
        self._overlay = None
        self._mask = None


def contact_sheet(frames: Sequence[np.ndarray], cols: int = 4,
                  tile_size: Tuple[int, int] = (320, 240)) -> np.ndarray:
    """把多帧拼成一张缩略图墙，便于一次性肉眼核对。"""
    if not frames:
        return np.zeros((10, 10, 3), np.uint8)
    tw, thh = tile_size
    rows = (len(frames) + cols - 1) // cols
    sheet = np.zeros((rows * thh, cols * tw, 3), np.uint8)
    for i, f in enumerate(frames):
        r, c = divmod(i, cols)
        sheet[r * thh:(r + 1) * thh, c * tw:(c + 1) * tw] = cv2.resize(f, (tw, thh))
    return sheet


# --------------------------------------------------------------------------- #
# 地面网格（标定用）
# --------------------------------------------------------------------------- #
#: 画哪些"前方距离"横线（米）
GROUND_Z_TICKS = (0.5, 1.0, 2.0, 3.0, 5.0, 8.0)
#: 画哪些"横向偏移"纵线（米）
GROUND_X_TICKS = (-1.5, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5)


def _clip_polyline(pts: np.ndarray, img: np.ndarray, color, thickness: int) -> None:
    """画一条可能被地平线截断的地面投影线（自动跳过 nan 断点）。"""
    valid = ~np.isnan(pts).any(axis=1)
    if valid.sum() < 2:
        return
    idx = np.flatnonzero(valid)
    breaks = np.flatnonzero(np.diff(idx) > 1)
    limits = 20_000
    for seg in np.split(idx, breaks + 1):
        if seg.size < 2:
            continue
        p = pts[seg].round().astype(np.int32)
        p[:, 0] = np.clip(p[:, 0], -limits, limits)
        p[:, 1] = np.clip(p[:, 1], -limits, limits)
        cv2.polylines(img, [p], False, color, thickness, cv2.LINE_AA)


def draw_ground_grid(frame: np.ndarray, camera, labels: bool = True) -> np.ndarray:
    """把地面度量网格画到画面上，用于**肉眼标定**。

    参数正确时，那些"1m / 2m / 3m"的横线应该正好落在真实地面上的对应位置。
    在浏览器调试页里实时调 ``height`` / ``pitch`` 看线往哪动，对齐了就是标定好了。
    """
    if camera is None:
        return frame
    img = frame
    h, w = img.shape[:2]
    z_near, z_far = camera.visible_ground_range()
    z_near = max(z_near, 0.05)
    z_far = min(z_far if np.isfinite(z_far) else 30.0, 30.0)

    # ---- 前方距离横线
    for z in GROUND_Z_TICKS:
        if not (z_near * 0.9 <= z <= z_far * 1.02):
            continue
        xs = np.linspace(-max(2.0, z * 0.9), max(2.0, z * 0.9), 120)
        pts = camera.ground_polyline_to_px(
            np.stack([xs, np.full_like(xs, z)], axis=1))
        major = z in (1.0, 2.0, 3.0)
        _clip_polyline(pts, img, (70, 200, 70) if major else (45, 120, 45),
                       2 if major else 1)
        if labels:
            c = camera.ground2px(0.0, z)
            if c and 0 <= c[0] < w and 0 <= c[1] < h:
                _put(img, f"{z:.1f}m", (int(c[0]) + 6, int(c[1]) - 6),
                     (70, 200, 70), 0.5)

    # ---- 横向偏移纵线
    zs = np.linspace(z_near, z_far, 160)
    for x in GROUND_X_TICKS:
        pts = camera.ground_polyline_to_px(
            np.stack([np.full_like(zs, x), zs], axis=1))
        color = (70, 190, 255) if x == 0.0 else (120, 110, 180)
        _clip_polyline(pts, img, color, 2 if x == 0.0 else 1)
        if labels:
            far = camera.ground2px(x, min(1.2, z_far))
            if far and 0 <= far[0] < w and 0 <= far[1] < h:
                _put(img, f"{x:+.1f}", (int(far[0]) - 12, int(far[1]) + 16),
                     (120, 110, 180), 0.42)

    # ---- 地平线
    hv = camera.horizon_v()
    if 0 <= hv < h:
        cv2.line(img, (0, int(hv)), (w, int(hv)), (60, 60, 200), 1)
    return img
