"""全局配置：相机标定参数 + 各检测器阈值。

设计原则
--------
1. 所有可调参数集中在这里，且能从 JSON 文件 / 环境变量 / 命令行覆盖；
2. 每个阈值都写清单位（米 / 度 / 秒 / 帧）；
3. `AppConfig.from_dict()` / `to_dict()` 必须可往返，服务器 `POST /config` 依赖它。
"""
from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass, field, fields, is_dataclass
from pathlib import Path
from typing import Any, Dict, Optional

PROJECT_ROOT = Path(__file__).resolve().parent.parent
MODELS_DIR = PROJECT_ROOT / "models"
WEB_DIR = PROJECT_ROOT / "web"
EVAL_DIR = PROJECT_ROOT / "eval"

#: 示例视频的候选位置（源码树里在 材料/，交付包里在 data/）
_SAMPLE_VIDEO_CANDIDATES = (
    PROJECT_ROOT / "data" / "Test.mp4",
    PROJECT_ROOT / "材料" / "blind_navigation-2.2.0" / "uploads" / "Test.mp4",
)
#: 原始参考固件的候选位置（交付包里放在 esp32/_原始参考/）
_REFERENCE_INO_CANDIDATES = (
    PROJECT_ROOT / "esp32" / "_原始参考" / "camera_stream_download.ino",
    PROJECT_ROOT / "材料" / "esp32s3图传" / "camera_stream_download"
    / "camera_stream_download.ino",
)


def find_sample_video() -> Optional[Path]:
    """找示例视频 Test.mp4。

    源码树里它躺在 `材料/`，交付包里在 `data/`。测试和评测都靠这个函数，
    免得把某一种目录布局写死在代码里。也可用环境变量 `VISION_TEST_VIDEO` 指定。
    """
    env = os.environ.get("VISION_TEST_VIDEO")
    if env:
        p = Path(env)
        if p.is_file():
            return p
    for candidate in _SAMPLE_VIDEO_CANDIDATES:
        if candidate.is_file():
            return candidate
    return None


def find_reference_ino() -> Optional[Path]:
    """找**未修改的**原始 ESP32 参考固件（patches 测试要用它做基准）。"""
    for candidate in _REFERENCE_INO_CANDIDATES:
        if candidate.is_file():
            return candidate
    return None


TACTILE_MODEL_PATH = MODELS_DIR / "blind_path.pt"
COCO_MODEL_PATH = MODELS_DIR / "yolov8n.pt"


# --------------------------------------------------------------------------- #
# 相机标定
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class CameraCalib:
    """针孔相机 + 地面平面标定。

    安装方式假定：胸/腰前挂，镜头略前倾向下，无滚转、无偏航。
    俯仰角 ``pitch_deg`` 定义为**光学轴低于水平线的角度**（正数=朝下看）。
    """

    width: int = 640
    height: int = 480
    hfov_deg: float = 65.0
    f_px: Optional[float] = None
    height_m: float = 1.20
    pitch_deg: float = 30.0
    cx: Optional[float] = None
    cy: Optional[float] = None

    # ---------------------------------------------------------------- 派生量
    def focal_px(self) -> float:
        """像素焦距。给定 ``f_px`` 时优先用它，否则由水平视场角推算。"""
        if self.f_px is not None:
            return float(self.f_px)
        return (self.width / 2.0) / math.tan(math.radians(self.hfov_deg) / 2.0)

    def principal_point(self) -> tuple[float, float]:
        cx = self.width / 2.0 if self.cx is None else float(self.cx)
        cy = self.height / 2.0 if self.cy is None else float(self.cy)
        return cx, cy

    def scaled(self, width: int, height: int) -> "CameraCalib":
        """按实际帧尺寸缩放内参（保持视场角不变，假定等比缩放）。"""
        if width == self.width and height == self.height:
            return self
        ratio = width / float(self.width)
        return CameraCalib(
            width=int(width),
            height=int(height),
            hfov_deg=self.hfov_deg,
            f_px=self.focal_px() * ratio,
            height_m=self.height_m,
            pitch_deg=self.pitch_deg,
            cx=None if self.cx is None else self.cx * ratio,
            cy=None if self.cy is None else self.cy * (height / float(self.height)),
        )

    # ------------------------------------------------------------ 序列化
    def to_dict(self) -> Dict[str, Any]:
        return {f.name: getattr(self, f.name) for f in fields(self)}


# --------------------------------------------------------------------------- #
# 各模块阈值
# --------------------------------------------------------------------------- #
@dataclass
class TactileThresholds:
    """盲道检测 + 行进砖/提示砖判别。"""

    conf: float = 0.25
    #: LSD 线段"够长"的门槛，相对裁剪区域对角线长度的比例
    line_len_ratio: float = 0.12
    #: 判定"行进砖"(长条状平行脊) 的最少长线段数（实测行进砖 5–35 条）
    guide_min_lines: int = 5
    #: 判定"提示砖"(圆点阵列) 的最多长线段数（实测提示砖 1–2 条）
    warning_max_lines: int = 2
    #: 判定行进砖时，最长线段至少占对角线的比例——**仅作为置信度因子**，不作硬门限
    #: （实测同一段行进砖在不同视角下该值在 0.18–0.71 间波动，作硬门限会误判）
    guide_min_maxlen: float = 0.30
    #: 线段平行度门槛：与中位角相差 ≤10° 的线段占比，用于区分"平行脊"与"零散碎片"
    parallel_angle_tol_deg: float = 10.0
    #: 横向偏移（米）小于此值视为"居中"
    aligned_abs_m: float = 0.12
    #: 进入 DRIFT 状态的横向偏移阈值（米）
    drift_enter_m: float = 0.15
    #: 退出 DRIFT 状态的横向偏移阈值（米，滞回）
    drift_exit_m: float = 0.08
    #: 进入 TURN 状态的航向角阈值（度）
    turn_enter_deg: float = 20.0
    #: 退出 TURN 状态的航向角阈值（度，滞回）
    turn_exit_deg: float = 10.0
    #: 提示砖进入此距离（米）内触发 WARNING_TILE 警报
    warning_tile_near_m: float = 2.5
    #: 提示砖近于此距离（米）视为"脚下/已踩上"，不算"前方有提示砖"，不再报警
    warning_tile_underfoot_m: float = 0.6
    #: 盲道丢失持续此秒数后进入 PATH_LOST
    lost_after_s: float = 1.0


@dataclass
class TemporalThresholds:
    """跟踪与平滑。"""

    #: EMA 平滑系数，越小越平滑但越迟钝
    ema_alpha: float = 0.35
    #: 关键判据需连续多少帧一致才确认
    confirm_frames: int = 3
    #: 需连续多少帧不一致才撤销
    release_frames: int = 5
    #: 目标匹配的最大归一化中心距（占画面对角线比例）
    match_max_dist: float = 0.35


@dataclass
class QualityThresholds:
    """画面质量评估。"""

    blur_min: float = 60.0
    brightness_min: float = 45.0
    brightness_max: float = 215.0
    overexposure_max_ratio: float = 0.25


@dataclass
class ObstacleThresholds:
    """障碍物检测。"""

    conf: float = 0.30
    #: 距离（米）小于此值触发 OBSTACLE_NEAR
    near_m: float = 2.5
    #: 近于此距离（米）的检测直接忽略。两个原因：
    #: 1. 胸挂相机必然拍到自己**的脚/鞋**，COCO 会把它识别成 person；
    #: 2. 已经在脚下的东西，提醒也来不及，报警没有价值。
    ignore_below_m: float = 0.7
    #: 框的下边缘距画面底部小于这么多像素时直接忽略：被画面底边截断的框
    #: 基本都是佩戴者自己的身体（腿/鞋），实测漏过这条会报出"前方74厘米有行人"。
    ignore_bottom_margin_px: int = 6
    #: 检测框面积占整帧比例超过此值就丢弃。单个障碍物占掉半屏，要么已经贴脸
    #: （会被 ignore_below_m 去掉），要么就是误检——实测出现过一个横跨 717px 宽的
    #: "person" 框（conf=0.72），把大片盲道纹理当成了行人。
    max_area_ratio: float = 0.5
    #: person 类别的宽高比上限（宽/高）。人站着时明显瘦高，宽大于高基本是误检。
    person_max_aspect: float = 1.2
    #: 复用的 COCO 类别名 -> 中文名
    class_map: Dict[str, str] = field(
        default_factory=lambda: {
            "person": "行人",
            "bicycle": "自行车",
            "car": "汽车",
            "motorcycle": "摩托车",
            "bus": "公交车",
            "truck": "卡车",
            "dog": "狗",
            "bench": "长椅",
            "chair": "椅子",
            "fire hydrant": "消防栓",
            "potted plant": "盆栽",
            "stop sign": "停车标志",
        }
    )


@dataclass
class TrafficLightThresholds:
    """红绿灯状态判别。"""

    conf: float = 0.25
    #: 判定某颜色需达到的最小像素占比
    min_color_ratio: float = 0.06
    #: 颜色时序投票需要的帧数
    vote_frames: int = 5


@dataclass
class CrosswalkThresholds:
    """斑马线检测。"""

    #: 白条亮度阈值（0-255 的灰度）
    white_threshold: int = 150
    #: 至少需要几条白条才判为斑马线
    min_bars: int = 4
    #: 白色像素占比下限
    min_white_ratio: float = 0.04
    #: 反投影后度量空间的分辨率（米/像素）
    metric_resolution_m: float = 0.02
    #: 度量空间有效区域：Z 范围（米）
    z_min_m: float = 0.8
    z_max_m: float = 5.0
    #: 度量空间有效区域：X 范围（米）
    x_span_m: float = 3.0
    #: 触发 CROSSWALK 警报所需的最低置信度
    alert_min_confidence: float = 0.5


@dataclass
class StairsThresholds:
    """台阶检测（规则级，保守）。"""

    #: 边缘两侧的平均亮度差（0–1）低于此值不判为台阶；0.35 ≈ 亮度跳变 35%
    edge_response_min: float = 0.35
    #: 边缘两侧的**色相**差（度）低于此值视为光影变化而非材质变化。
    #: 阴影只改亮度不改色相，真实台阶/落差通常伴随材质变化。实测 ``Test.mp4``
    #: 里的强阴影边缘会被误判成台阶，加上这条后误报被压掉。
    min_hue_jump_deg: float = 8.0
    #: 多帧确认所需帧数
    confirm_frames: int = 4
    #: 只在此距离范围（米）内检测
    z_min_m: float = 0.8
    z_max_m: float = 2.5


@dataclass
class SpeechThresholds:
    """播报策略。"""

    #: 全局限速：两次播报的最小间隔（秒）
    global_min_gap_s: float = 1.2
    #: 按事件类型独立冷却（秒）
    cooldown_s: Dict[str, float] = field(
        default_factory=lambda: {
            "OBSTACLE_NEAR": 4.0,
            "PATH_LOST": 6.0,
            "WARNING_TILE": 8.0,
            "TURN_LEFT": 3.0,
            "TURN_RIGHT": 3.0,
            "DRIFT_LEFT": 5.0,
            "DRIFT_RIGHT": 5.0,
            "CROSSING": 8.0,
            "LIGHT_RED": 4.0,
            "LIGHT_GREEN": 4.0,
            "STAIRS": 8.0,
        }
    )
    #: 各状态优先级，数字越大越优先，可抢占
    priority: Dict[str, int] = field(
        default_factory=lambda: {
            "OBSTACLE_NEAR": 100,
            "STAIRS": 90,
            "PATH_LOST": 80,
            "WARNING_TILE": 70,
            "TURN_LEFT": 60,
            "TURN_RIGHT": 60,
            "CROSSING": 55,
            "LIGHT_RED": 50,
            "LIGHT_GREEN": 50,
            "DRIFT_LEFT": 40,
            "DRIFT_RIGHT": 40,
            "ALIGNED": 0,
            "NO_PATH": 0,
        }
    )
    #: 是否启用 TTS（默认关，只输出 JSON）
    tts_enabled: bool = False


# --------------------------------------------------------------------------- #
# 顶层配置
# --------------------------------------------------------------------------- #
@dataclass
class AppConfig:
    camera: CameraCalib = field(default_factory=CameraCalib)
    tactile: TactileThresholds = field(default_factory=TactileThresholds)
    temporal: TemporalThresholds = field(default_factory=TemporalThresholds)
    quality: QualityThresholds = field(default_factory=QualityThresholds)
    obstacle: ObstacleThresholds = field(default_factory=ObstacleThresholds)
    traffic_light: TrafficLightThresholds = field(default_factory=TrafficLightThresholds)
    crosswalk: CrosswalkThresholds = field(default_factory=CrosswalkThresholds)
    stairs: StairsThresholds = field(default_factory=StairsThresholds)
    speech: SpeechThresholds = field(default_factory=SpeechThresholds)

    #: 帧源描述，形如 ``mjpeg:http://192.168.1.20/stream`` / ``file:xx.mp4`` / ``usb:0``
    source: str = "file:data/Test.mp4"
    #: 目标处理帧率上限（0 = 不限）
    target_fps: float = 15.0
    #: 盲道 YOLO 推理尺寸
    imgsz: int = 640
    #: 障碍物 YOLO 推理尺寸（COCO 模型只需认得"有人/有车"，可以更小更快）
    imgsz_obstacle: int = 320
    #: 障碍物模型每 N 帧跑一次，中间的帧沿用上一次的检测框。
    #: 默认 1（每帧都跑）。设 2 大约能把整条管线提速 15–20%，
    #: 代价是障碍物位置最多滞后 N 帧（20 FPS 下 N=2 就是 100 ms）。
    #: 注意：距离与红绿灯颜色仍是每帧重算的，滞后的只有"框"。
    obstacle_every: int = 1
    #: torch 线程数（0 = 不设置）
    #:
    #: 实测（32 核 CPU、YOLOv8n）：threads=1 最快，threads=2 反而慢一倍以上。
    #: 原因是核多时默认线程数会严重争抢。核心数较少的机器可以试 2~4。
    torch_threads: int = 1
    #: 推理设备："auto"（有 GPU 就用）/ "cpu" / "cuda:0"
    device: str = "auto"
    #: 是否在画面上叠加地面度量网格。标定时靠它对齐（浏览器里改 height/pitch
    #: 能实时看到线怎么动）；平时调参也可以留着，位置准了就是标定对了。
    show_ground_grid: bool = True

    # ------------------------------------------------------------ 序列化
    def to_dict(self) -> Dict[str, Any]:
        return _dc_to_dict(self)

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "AppConfig":
        """显式重建嵌套 dataclass。

        注意：本模块用了 ``from __future__ import annotations``，``field.type`` 是字符串，
        不能用它做 ``is_dataclass`` 判断，所以这里逐项显式构造。
        """
        return cls(
            camera=_build(CameraCalib, data.get("camera")),
            tactile=_build(TactileThresholds, data.get("tactile")),
            temporal=_build(TemporalThresholds, data.get("temporal")),
            quality=_build(QualityThresholds, data.get("quality")),
            obstacle=_build(ObstacleThresholds, data.get("obstacle")),
            traffic_light=_build(TrafficLightThresholds, data.get("traffic_light")),
            crosswalk=_build(CrosswalkThresholds, data.get("crosswalk")),
            stairs=_build(StairsThresholds, data.get("stairs")),
            speech=_build(SpeechThresholds, data.get("speech")),
            source=data.get("source", cls().source),
            target_fps=float(data.get("target_fps", cls().target_fps)),
            imgsz=int(data.get("imgsz", cls().imgsz)),
            imgsz_obstacle=int(data.get("imgsz_obstacle", cls().imgsz_obstacle)),
            obstacle_every=max(1, int(data.get("obstacle_every", cls().obstacle_every))),
            torch_threads=int(data.get("torch_threads", cls().torch_threads)),
            # 注意：新增顶层字段时**必须**在这里补一行，
            # 否则 POST /config 改这个字段会静默失效（曾经漏过 show_ground_grid
            # 和 imgsz_obstacle）。tests/test_config.py 里有通用回归测试兜底。
            device=str(data.get("device", cls().device)),
            show_ground_grid=bool(data.get("show_ground_grid",
                                           cls().show_ground_grid)),
        )

    @classmethod
    def load(cls, path: Optional[str | os.PathLike] = None) -> "AppConfig":
        """从 JSON 文件加载，再用环境变量覆盖少数常用字段。"""
        cfg = cls()
        candidate = path or os.environ.get("VISION_CONFIG")
        if candidate:
            p = Path(candidate)
            if not p.is_file():
                raise FileNotFoundError(f"配置文件不存在: {p}")
            cfg = cls.from_dict(json.loads(p.read_text(encoding="utf-8")))
        cfg._apply_env()
        return cfg

    def _apply_env(self) -> None:
        """环境变量覆盖少数常用字段。

        ``VISION_CAM_HEIGHT_M``  相机离地高度（米）
        ``VISION_CAM_PITCH``     俯仰角（度，正=朝下）
        ``VISION_CAM_HFOV``      水平视场角（度）
        ``VISION_SOURCE``        帧源描述
        ``VISION_FPS``           目标帧率
        """
        cam = self.camera.to_dict()
        changed = False
        for env, key, cast in (
            ("VISION_CAM_HEIGHT_M", "height_m", float),
            ("VISION_CAM_PITCH", "pitch_deg", float),
            ("VISION_CAM_HFOV", "hfov_deg", float),
        ):
            raw = os.environ.get(env)
            if raw is not None:
                cam[key] = cast(raw)
                changed = True
        if changed:
            self.camera = CameraCalib(**cam)

        if (raw := os.environ.get("VISION_SOURCE")) is not None:
            self.source = raw
        if (raw := os.environ.get("VISION_FPS")) is not None:
            self.target_fps = float(raw)

    def save(self, path: str | os.PathLike) -> None:
        Path(path).write_text(
            json.dumps(self.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8"
        )


# --------------------------------------------------------------------------- #
# dataclass <-> dict 通用转换
# --------------------------------------------------------------------------- #
def _dc_to_dict(obj: Any) -> Any:
    if is_dataclass(obj) and not isinstance(obj, type):
        return {f.name: _dc_to_dict(getattr(obj, f.name)) for f in fields(obj)}
    if isinstance(obj, dict):
        return {k: _dc_to_dict(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_dc_to_dict(v) for v in obj]
    return obj


def _build(cls: Any, data: Optional[Dict[str, Any]]) -> Any:
    """由 dict 构造一个**扁平** dataclass，未知键忽略、缺失键用默认值。"""
    if data is None:
        return cls()
    known = {f.name for f in fields(cls)}
    return cls(**{k: v for k, v in data.items() if k in known})


def default_config() -> AppConfig:
    return AppConfig()
