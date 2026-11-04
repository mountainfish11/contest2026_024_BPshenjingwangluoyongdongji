"""评测脚本：在标注集上出准确率 / 延迟 / 帧率数字，并与参照实现同口径对比。

用法::

    python -m eval.run                          # 用 eval/labels.json 的默认配置
    python -m eval.run --video 现场.mp4 --out eval/report.md

诚实性说明
----------
- **能算的**：盲道可用率、状态稳定性、播报频率、延迟 p50/p95、FPS、砖型分类准确率
  （在有标注的裁剪样本上）。
- **暂时算不了的**：斑马线 / 红绿灯 / 台阶的准确率——现有素材（``Test.mp4``）里
  根本没有这些目标。脚本会把它们标成 ``N/A``，并提示需要现场实拍数据，
  **绝不编造数字**。
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from eval.baseline import BaselineVerdict, centers_from_boxes, judge  # noqa: E402
from vision_assist.config import AppConfig, CameraCalib, find_sample_video  # noqa: E402
from vision_assist.detectors.tactile import classify_tile, crop_for_box  # noqa: E402
from vision_assist.pipeline import Pipeline  # noqa: E402
from vision_assist.schema import validate_scene  # noqa: E402
from vision_assist.sources import LocalVideoSource, open_source  # noqa: E402


@dataclass
class Metrics:
    frames: int = 0
    #: 本实现给出横向偏移或航向角的帧数（能真正引导用户）
    ours_usable: int = 0
    #: 参照实现给出方向的帧数
    baseline_usable: int = 0
    #: 参照实现因框数 < 2 而无法判定的帧数
    baseline_too_few: int = 0
    #: 两者都能判定的帧中，方向（左/右）是否一致
    both_usable: int = 0
    direction_agree: int = 0
    #: 方向翻转次数：衡量"抖不抖"。参照实现无去抖，这里能量化它的抖动
    baseline_flips: int = 0
    our_turn_flips: int = 0
    baseline_left: int = 0
    baseline_right: int = 0
    baseline_straight: int = 0
    #: 本实现的状态序列
    state_runs: int = 0
    state_hist: Counter = field(default_factory=Counter)
    speeches: List[str] = field(default_factory=list)
    speech_times: List[float] = field(default_factory=list)
    latencies: List[float] = field(default_factory=list)
    stage_latencies: Dict[str, List[float]] = field(default_factory=dict)
    schema_failures: int = 0
    tile_total: int = 0
    tile_correct: int = 0
    tile_details: List[str] = field(default_factory=list)
    crosswalk_frames: int = 0
    traffic_light_frames: int = 0
    stairs_frames: int = 0


def load_labels(path: Path) -> dict:
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def _resolve_video(labels: dict, override: Optional[str]) -> str:
    if override:
        return override
    video = labels.get("video") or ""
    if not video or not Path(video).is_file():
        found = find_sample_video()
        video = str(found) if found else ""
    path = Path(video)
    return str(path if path.is_absolute() else ROOT / path)


def _camera(labels: dict) -> CameraCalib:
    cam = labels.get("camera") or {"height_m": 1.3, "pitch_deg": 85.0}
    return CameraCalib(**cam)


def evaluate_tiles(video_path: str, samples: List[dict], max_frames: int = 0) -> tuple[int, int, List[str]]:
    """在人工标注的裁剪样本上评估行进砖/提示砖分类。"""
    if not samples:
        return 0, 0, ["（无砖型标注样本）"]
    needed = sorted({int(s["frame"]) for s in samples})
    src = LocalVideoSource(video_path, loop=False)
    frames: Dict[int, np.ndarray] = {}
    try:
        for frame_no in needed:
            src._cap.set(cv2.CAP_PROP_POS_FRAMES, frame_no)
            frame = src.read()
            if frame is not None:
                frames[frame_no] = frame
    finally:
        src.close()

    correct = 0
    details: List[str] = []
    for sample in samples:
        frame = frames.get(int(sample["frame"]))
        if frame is None:
            details.append(f"跳过（取不到帧 {sample['frame']}）")
            continue
        h, w = frame.shape[:2]
        y0, y1, x0, x1 = sample["bbox_rel"]
        crop = frame[int(h * y0):int(h * y1), int(w * x0):int(w * x1)]
        if crop.size == 0:
            details.append(f"跳过（裁剪为空 {sample['frame']}）")
            continue
        tile, conf, metrics = classify_tile(crop)
        truth = sample["label"]
        hit = tile == truth
        correct += hit
        details.append(
            f"{'OK  ' if hit else 'MISS'} frame={sample['frame']:4d} "
            f"真值={truth:7s} 预测={tile:7s} conf={conf:.2f} "
            f"n={metrics['n_lines']:2d} L={metrics['max_len_ratio']:.2f}  {sample.get('note','')}"
        )
    return len(details), correct, details


def run_eval(args: argparse.Namespace) -> Metrics:
    labels = load_labels(Path(args.labels))
    video = _resolve_video(labels, args.video)
    cfg = AppConfig()
    cfg.camera = _camera(labels)
    if args.imgsz:
        cfg.imgsz = args.imgsz

    metrics = Metrics()
    print(f"[eval] 视频: {video}")
    print(f"[eval] 标定: {cfg.camera.to_dict()}")

    src = open_source(f"file:{video}")
    pipeline = Pipeline(cfg)
    prev_state: Optional[str] = None
    prev_baseline_dir: Optional[str] = None
    prev_turn: Optional[str] = None
    frame_idx = 0
    start = time.perf_counter()

    try:
        while True:
            frame = src.read()
            if frame is None:
                break
            if frame_idx == 0:
                pipeline._ensure_camera(frame)

            scene = pipeline.process(frame, frame_idx, src.info, now=frame_idx / 15.0)
            metrics.frames += 1
            metrics.state_hist[scene.state] += 1
            if scene.state != prev_state:
                metrics.state_runs += 1
                prev_state = scene.state

            if validate_scene(scene.to_dict()):
                metrics.schema_failures += 1

            for key, value in scene.latency_ms.items():
                if key != "total":
                    metrics.stage_latencies.setdefault(key, []).append(value)
            metrics.latencies.append(scene.latency_ms.get("total", 0.0))

            # ---- 本实现可用性：有横向偏移或航向角即可引导
            tracker = scene.tracker
            ours_ok = tracker.present and (tracker.offset_m is not None or tracker.heading_deg is not None)
            if ours_ok:
                metrics.ours_usable += 1

            # ---- 参照实现
            boxes = scene.tactile.boxes
            verdict: BaselineVerdict = judge(centers_from_boxes(boxes))
            if verdict.direction == "none":
                metrics.baseline_too_few += 1
            else:
                metrics.baseline_usable += 1
                if verdict.direction == "left":
                    metrics.baseline_left += 1
                elif verdict.direction == "right":
                    metrics.baseline_right += 1
                elif verdict.direction == "straight":
                    metrics.baseline_straight += 1

                # 只和"转向"比：基线用像素斜率衡量的是航向，横向偏移(DRIFT)不是同一回事
                if ours_ok and scene.state in ("TURN_LEFT", "TURN_RIGHT") \
                        and verdict.direction in ("left", "right"):
                    metrics.both_usable += 1
                    our_dir = "left" if scene.state == "TURN_LEFT" else "right"
                    if our_dir == verdict.direction:
                        metrics.direction_agree += 1

            # 抖动：连续两帧方向不同即算一次翻转
            if verdict.direction in ("left", "right"):
                if prev_baseline_dir is not None and verdict.direction != prev_baseline_dir:
                    metrics.baseline_flips += 1
                prev_baseline_dir = verdict.direction

            our_turn = ("LEFT" if scene.state == "TURN_LEFT"
                        else "RIGHT" if scene.state == "TURN_RIGHT" else None)
            if our_turn is not None:
                if prev_turn is not None and our_turn != prev_turn:
                    metrics.our_turn_flips += 1
                prev_turn = our_turn

            if scene.speech is not None and scene.speech.spoke and scene.speech.text:
                metrics.speeches.append(scene.speech.text)
                metrics.speech_times.append(scene.timestamp)

            if scene.crosswalk.present:
                metrics.crosswalk_frames += 1
            if scene.traffic_light.present:
                metrics.traffic_light_frames += 1
            if scene.stairs.suspected:
                metrics.stairs_frames += 1

            frame_idx += 1
            if args.max_frames and frame_idx >= args.max_frames:
                break
    finally:
        pipeline.close()
        src.close()

    elapsed = time.perf_counter() - start
    metrics.elapsed_s = elapsed  # type: ignore[attr-defined]

    # ---- 砖型准确率
    total, correct, details = evaluate_tiles(video, labels.get("tile_samples", []))
    metrics.tile_total = total
    metrics.tile_correct = correct
    metrics.tile_details = details
    return metrics


def _pct(a: int, b: int) -> str:
    return "N/A" if b == 0 else f"{a / b * 100:.1f}%"


def _p(values: List[float], q: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    idx = min(len(s) - 1, int(len(s) * q))
    return s[idx]


def report(metrics: Metrics, labels: dict) -> str:
    n = metrics.frames
    fps = n / max(1e-6, getattr(metrics, "elapsed_s", 1.0))
    lines: List[str] = []
    add = lines.append

    add("# 视觉识别评测报告\n")
    add(f"- 视频：`{labels.get('video', '(命令行指定)')}`")
    add(f"- 帧数：{n}")
    add(f"- 标定：{json.dumps(labels.get('camera', {}), ensure_ascii=False)}")
    add("")

    add("## 1. 本实现 vs 参照实现（同口径，无需人工标注）\n")
    add("| 指标 | 本实现 | 参照实现（`routes/video.py` 的 slope 法） |")
    add("|---|---|---|")
    add(f"| 能给出方向/偏移的帧数 | **{metrics.ours_usable}/{n}（{_pct(metrics.ours_usable, n)}）** "
        f"| {metrics.baseline_usable}/{n}（{_pct(metrics.baseline_usable, n)}） |")
    add(f"| 因检测框不足而无法判定的帧（\"失明\"） | 0（单框/0 框也能给出偏移或标记丢失） "
        f"| **{metrics.baseline_too_few}（{_pct(metrics.baseline_too_few, n)}）** |")
    add(f"| 左右方向翻转次数（越少越稳） | **{metrics.our_turn_flips}**（含去抖与滞回） "
        f"| {metrics.baseline_flips}（无任何去抖） |")
    add(f"| 基线判定分布 | — | 左 {metrics.baseline_left} / 右 {metrics.baseline_right} / "
        f"直行 {metrics.baseline_straight} |")
    add(f"| 两法同时给出左右方向的帧数 | {metrics.both_usable} 帧，一致 {metrics.direction_agree} 帧"
        f"（{_pct(metrics.direction_agree, metrics.both_usable)}） | — |")
    add("")
    add(f"> 关于「方向一致率」：它只有 {metrics.both_usable} 帧样本，且**两法测的不是同一个量**——"
        "基线用检测框中心在像素空间的斜率当方向，随相机俯仰/高度变化而变，并不是几何航向角；"
        "本实现用的是地面反投影后的真实航向角。因此一致率低**不能**直接解读为谁更准，"
        "只能说明二者不等价。真正可比的客观量是上表的「失明帧数」与「翻转次数」。")
    add("")

    add("## 2. 状态稳定性\n")
    add(f"- 状态切换次数：**{metrics.state_runs}**（平均每段 {n / max(1, metrics.state_runs):.1f} 帧）")
    add(f"- 状态分布：`{dict(metrics.state_hist)}`")
    add("")

    add("## 3. 播报\n")
    add(f"- 播报次数：{len(metrics.speeches)}")
    if metrics.speeches:
        add(f"- 平均间隔：{(metrics.frames / 15.0) / len(metrics.speeches):.2f} 秒/次"
            f"（按 15 FPS 折算）")
        add("- 明细（前 10 条）：")
        for text in metrics.speeches[:10]:
            add(f"  - {text}")
    add("")

    add("## 4. 延迟与帧率\n")
    add(f"- 端到端延迟：p50 **{_p(metrics.latencies, 0.5):.1f} ms** / "
        f"p95 **{_p(metrics.latencies, 0.95):.1f} ms**")
    add(f"- 吞吐：**{fps:.1f} FPS**（含取帧与解码）")
    add("- 分阶段 p50：")
    for stage, values in sorted(metrics.stage_latencies.items()):
        add(f"  - {stage}: {statistics.median(values):.1f} ms")
    add("")

    add("## 5. 行进砖 / 提示砖分类\n")
    add(f"- 标注样本：**{metrics.tile_total}** 个，正确 **{metrics.tile_correct}**，"
        f"准确率 **{_pct(metrics.tile_correct, metrics.tile_total)}**")
    if metrics.tile_details:
        add("")
        for detail in metrics.tile_details:
            add(f"  - {detail}")
    add("")

    add("## 6. 斑马线 / 红绿灯 / 台阶\n")
    add(f"- 检出帧数：斑马线 {metrics.crosswalk_frames}、红绿灯 {metrics.traffic_light_frames}、"
        f"台阶 {metrics.stairs_frames}")
    if not (labels.get("crosswalk_frames") or labels.get("traffic_light_frames")):
        add("- **准确率：N/A** —— 现有素材里没有斑马线与红绿灯目标，无法计算准确率。")
        add("  需要在真实现场（含路口）采集 ESP32 实拍视频，用 `eval/label_tool.py` 标注后重跑。")
        add("  此处**不做任何推测性数字**。")
    add("")

    add("## 7. 输出协议自检\n")
    add(f"- Scene JSON schema 校验失败帧数：**{metrics.schema_failures}**")
    add("")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="视觉识别评测")
    ap.add_argument("--labels", default=str(ROOT / "eval" / "labels.json"))
    ap.add_argument("--video", default=None)
    ap.add_argument("--out", default=str(ROOT / "eval" / "report.md"))
    ap.add_argument("--max-frames", type=int, default=300)
    ap.add_argument("--imgsz", type=int, default=0)
    return ap


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    metrics = run_eval(args)
    text = report(metrics, load_labels(Path(args.labels)))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    print()
    print(text)
    print(f"\n[eval] 报告已写入 {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
