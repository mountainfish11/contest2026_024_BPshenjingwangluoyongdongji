"""处理管线：帧进 → 场景结果出。

完整链路::

    取帧 → 画面质量
         → 盲道检测 ─┐
         → COCO 检测 ─┼→ 障碍物 / 红绿灯
         → 斑马线（度量空间）
         → 台阶（度量空间，保守）
                     ↓
              时序平滑 → 引导状态机 → 警报评估 → 播报决策
                     ↓
                  Scene JSON
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Dict, Optional

import numpy as np

from . import render
from .alerts import AlertEvaluator, AlertResult
from .config import AppConfig
from .device import resolve_device
from .detectors.base import FrameContext
from .detectors.crosswalk import CrosswalkDetector, CrosswalkResult
from .detectors.obstacle import CocoDetector, ObstacleResult, build_obstacles
from .detectors.stairs import StairsDetector, StairsResult
from .detectors.tactile import TactileDetector, TactileResult
from .detectors.traffic_light import TrafficLightAnalyzer, TrafficLightResult
from .events import SpeechDecision, SpeechPolicy
from .geometry import CameraModel, camera_model_for
from .quality import QualityResult, assess_quality
from .state import GuideState, StateMachine
from .temporal import TactileTracker, TrackerState


@dataclass
class Scene:
    """一帧的完整识别结果。"""

    index: int = 0
    timestamp: float = 0.0
    source: dict = field(default_factory=dict)
    quality: QualityResult = field(default_factory=QualityResult)
    tactile: TactileResult = field(default_factory=TactileResult)
    tracker: TrackerState = field(default_factory=TrackerState)
    state: str = GuideState.NO_PATH.value
    state_reason: str = ""
    obstacles: ObstacleResult = field(default_factory=ObstacleResult)
    crosswalk: CrosswalkResult = field(default_factory=CrosswalkResult)
    traffic_light: TrafficLightResult = field(default_factory=TrafficLightResult)
    stairs: StairsResult = field(default_factory=StairsResult)
    alerts: AlertResult = field(default_factory=AlertResult)
    speech: Optional[SpeechDecision] = None
    latency_ms: Dict[str, float] = field(default_factory=dict)
    fps: Optional[float] = None

    def to_dict(self) -> dict:
        return {
            "ts": round(self.timestamp, 3),
            "frame": self.index,
            "source": self.source,
            "quality": self.quality.to_dict(),
            "tactile": self.tactile.to_dict(),
            "tracker": self.tracker.to_dict(),
            "state": self.state,
            "state_reason": self.state_reason,
            "obstacles": self.obstacles.to_dict(),
            "crosswalk": self.crosswalk.to_dict(),
            "traffic_light": self.traffic_light.to_dict(),
            "stairs": self.stairs.to_dict(),
            "alerts": self.alerts.to_dict(),
            "speech": None if self.speech is None else self.speech.to_dict(),
            "latency_ms": self.latency_ms,
            "fps": None if self.fps is None else round(self.fps, 1),
        }


class Pipeline:
    """把检测器、时序、状态机、播报策略串起来的处理管线。"""

    def __init__(
        self,
        config: Optional[AppConfig] = None,
        weights: Optional[str] = None,
        coco_weights: Optional[str] = None,
        enable_tactile: bool = True,
        enable_obstacle: bool = True,
        enable_crosswalk: bool = True,
        enable_stairs: bool = True,
    ):
        self.config = config or AppConfig()
        self.enable_tactile = enable_tactile
        self.enable_obstacle = enable_obstacle
        self.enable_crosswalk = enable_crosswalk
        self.enable_stairs = enable_stairs
        self.device, self.device_description = resolve_device(
            getattr(self.config, "device", "auto"))

        self._camera: Optional[CameraModel] = None
        self._camera_size = (0, 0)
        self._tactile: Optional[TactileDetector] = None
        self._coco: Optional[CocoDetector] = None
        self._crosswalk: Optional[CrosswalkDetector] = None
        self._stairs: Optional[StairsDetector] = None
        self._weights = weights
        self._coco_weights = coco_weights

        self.traffic_light = TrafficLightAnalyzer(self.config)
        self.tracker = TactileTracker(
            self.config.temporal,
            lost_after_s=self.config.tactile.lost_after_s,
        )
        self.state_machine = StateMachine(self.config.tactile)
        self.alert_evaluator = AlertEvaluator(self.config)
        self.speech_policy = SpeechPolicy(self.config.speech)
        self._grid_renderer = render.GroundGridRenderer()
        self._last_coco_raw = None
        self._apply_torch_threads()

    def _apply_torch_threads(self) -> None:
        """限制 torch 线程数（只对 CPU 有意义）。

        实测在 32 核机器上：threads=1 时 YOLOv8n(imgsz=640) 约 31ms，
        threads=2 时反而涨到 66ms——核多时默认线程数会严重争抢。
        用 GPU 时这个设置无关紧要，但也要限制一下 CPU 侧的算子线程。
        """
        n = int(getattr(self.config, "torch_threads", 0) or 0)
        if n <= 0:
            return
        try:
            import torch

            torch.set_num_threads(n)
        except Exception:  # noqa: BLE001 - torch 不在时不致命
            pass

    # ------------------------------------------------------------------ 组件
    @property
    def tactile(self) -> Optional[TactileDetector]:
        if not self.enable_tactile:
            return None
        if self._tactile is None:
            self._tactile = TactileDetector(self.config, weights=self._weights)
        return self._tactile

    @property
    def coco(self) -> Optional[CocoDetector]:
        if not self.enable_obstacle:
            return None
        if self._coco is None:
            self._coco = CocoDetector(self.config, weights=self._coco_weights)
        return self._coco

    @property
    def crosswalk(self) -> Optional[CrosswalkDetector]:
        if not self.enable_crosswalk:
            return None
        if self._crosswalk is None:
            self._crosswalk = CrosswalkDetector(self.config)
        return self._crosswalk

    @property
    def stairs(self) -> Optional[StairsDetector]:
        if not self.enable_stairs:
            return None
        if self._stairs is None:
            self._stairs = StairsDetector(self.config)
        return self._stairs

    @property
    def camera(self) -> Optional[CameraModel]:
        return self._camera

    def _ensure_camera(self, frame: np.ndarray) -> CameraModel:
        size = (int(frame.shape[1]), int(frame.shape[0]))
        if self._camera is None or self._camera_size != size:
            self._camera = camera_model_for(frame, self.config.camera)
            self._camera_size = size
        return self._camera

    # ------------------------------------------------------------------ 主流程
    def process(self, frame: np.ndarray, index: int = 0,
                source_info: Optional[dict] = None,
                now: Optional[float] = None) -> Scene:
        now = time.time() if now is None else float(now)
        t_start = time.perf_counter()
        camera = self._ensure_camera(frame)
        ctx = FrameContext.create(frame, camera, self.config, index)
        ctx.timestamp = now
        latency: Dict[str, float] = {}

        t0 = time.perf_counter()
        quality = assess_quality(frame, self.config.quality)
        latency["quality"] = round((time.perf_counter() - t0) * 1000.0, 2)

        # ---- 盲道
        tactile = TactileResult()
        det = self.tactile
        if det is not None:
            t0 = time.perf_counter()
            tactile = det.process(ctx)
            latency["tactile"] = round((time.perf_counter() - t0) * 1000.0, 2)

        # ---- COCO：一次推理，供障碍物与红绿灯共用
        obstacles = ObstacleResult()
        traffic = TrafficLightResult()
        coco = self.coco
        if coco is not None:
            t0 = time.perf_counter()
            every = max(1, int(getattr(self.config, "obstacle_every", 1) or 1))
            if self._last_coco_raw is None or every == 1 or (index % every) == 0:
                self._last_coco_raw = coco.detect_raw(frame)
            raw = self._last_coco_raw or []
            # 距离用当前相机每帧重算；红绿灯颜色也每帧重看，只有"框"可能滞后
            obstacles = build_obstacles(raw, camera, self.config.obstacle)
            traffic = self.traffic_light.update(frame, raw)
            latency["coco"] = round((time.perf_counter() - t0) * 1000.0, 2)

        # ---- 斑马线
        crosswalk = CrosswalkResult()
        cw = self.crosswalk
        if cw is not None:
            t0 = time.perf_counter()
            crosswalk = cw.process(ctx)
            latency["crosswalk"] = round((time.perf_counter() - t0) * 1000.0, 2)

        # ---- 台阶
        stairs = StairsResult()
        st = self.stairs
        if st is not None:
            t0 = time.perf_counter()
            stairs = st.process(ctx)
            latency["stairs"] = round((time.perf_counter() - t0) * 1000.0, 2)

        # ---- 时序 → 状态 → 警报 → 播报
        t0 = time.perf_counter()
        tracker = self.tracker.update(tactile, now)
        decision = self.state_machine.update(tracker)
        alerts = self.alert_evaluator.evaluate(
            tactile, tracker, obstacles, crosswalk, stairs, traffic
        )
        speech = self.speech_policy.decide(decision.state, tracker, alerts.alerts, now)
        latency["logic"] = round((time.perf_counter() - t0) * 1000.0, 2)
        latency["total"] = round((time.perf_counter() - t_start) * 1000.0, 2)

        return Scene(
            index=index,
            timestamp=now,
            source=source_info or {},
            quality=quality,
            tactile=tactile,
            tracker=tracker,
            state=decision.state.value,
            state_reason=decision.reason,
            obstacles=obstacles,
            crosswalk=crosswalk,
            traffic_light=traffic,
            stairs=stairs,
            alerts=alerts,
            speech=speech,
            latency_ms=latency,
        )

    # ------------------------------------------------------------------ 绘制
    def annotate(self, frame: np.ndarray, scene: Scene) -> np.ndarray:
        img = frame.copy()
        if getattr(self.config, "show_ground_grid", True):
            self._grid_renderer.render(img, self._camera)
        render.draw_obstacles(img, scene.obstacles)
        render.draw_tactile(img, scene.tactile, self._camera)
        render.draw_perception(img, scene)
        # 状态条最后画，保证始终可读
        render.draw_state(img, scene)
        render.draw_quality(img, scene.quality)
        return img

    def close(self) -> None:
        for component in (self._tactile, self._coco):
            if component is not None:
                component.close()
        self._tactile = None
        self._coco = None

    def apply_config(self, config: AppConfig) -> None:
        """就地换配置（不重新加载模型）。

        标定参数可能变了，因此把相机模型置空，下一帧用新参数重建。
        """
        self.config = config
        self.device, self.device_description = resolve_device(
            getattr(config, "device", "auto"))
        for component in (self._tactile, self._coco):
            if component is not None:
                component.config = config
                component.refresh_device()
        if self._crosswalk is not None:
            self._crosswalk.config = config
            self._crosswalk.th = config.crosswalk
        if self._stairs is not None:
            self._stairs.config = config
            self._stairs.th = config.stairs
        self.traffic_light.config = config
        self.tracker.th = config.temporal
        self.tracker._lost_after_s = float(config.tactile.lost_after_s)
        self.state_machine.th = config.tactile
        self.alert_evaluator.config = config
        self.speech_policy.th = config.speech
        self._camera = None

    def reset(self) -> None:
        self.tracker.reset()
        self.state_machine.reset()
        self.speech_policy.reset()
        self.traffic_light.reset()
        self._last_coco_raw = None
        if self._stairs is not None:
            self._stairs.reset()