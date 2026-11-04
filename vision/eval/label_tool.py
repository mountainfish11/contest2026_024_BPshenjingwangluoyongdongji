"""标注工具：为现场 ESP32 实拍视频补充标注，供 ``eval/run.py`` 使用。

为什么需要它
------------
现有素材 ``Test.mp4`` 里没有斑马线、红绿灯、台阶，所以这些能力的准确率**无法评估**。
要拿到真实数字，必须用现场实拍视频标注后重跑评测。这里提供一个够用的命令行工具。

常用命令
--------
导出抽帧图（先肉眼看一眼，确定要标哪些帧）::

    python -m eval.label_tool dump --video 现场.mp4 --count 24 --out eval/dump

标注一个砖型裁剪样本::

    python -m eval.label_tool tile --frame 120 --bbox 0.10,0.35,0.25,0.70 \
        --label guide --note "路口前条纹砖"

标注红绿灯 / 斑马线出现的帧::

    python -m eval.label_tool mark --kind traffic_light --frame 300 --value red --note "路口东侧灯"
    python -m eval.label_tool mark --kind crosswalk --frame 320 --value present

查看当前标注概览::

    python -m eval.label_tool summary
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import List, Optional

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LABELS = ROOT / "eval" / "labels.json"

TILE_LABELS = ("guide", "warning", "unknown")
MARK_KINDS = ("crosswalk", "traffic_light", "stairs")


def load(path: Path) -> dict:
    if path.is_file():
        return json.loads(path.read_text(encoding="utf-8"))
    return {"video": "", "camera": {}, "tile_samples": [], "frame_labels": [],
            "crosswalk_frames": [], "traffic_light_frames": []}


def save(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _open_video(video: str) -> cv2.VideoCapture:
    path = Path(video)
    if not path.is_absolute():
        path = ROOT / path
    cap = cv2.VideoCapture(str(path))
    if not cap.isOpened():
        raise SystemExit(f"打不开视频: {path}")
    return cap


def cmd_dump(args: argparse.Namespace) -> int:
    cap = _open_video(args.video)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    if total <= 0:
        raise SystemExit("拿不到视频帧数")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    step = max(1, total // max(1, args.count))
    saved = 0
    tiles: List[np.ndarray] = []
    for idx in range(0, total, step):
        cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
        ok, frame = cap.read()
        if not ok:
            continue
        annotated = frame.copy()
        cv2.putText(annotated, f"frame={idx}", (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2, cv2.LINE_AA)
        name = out / f"frame_{idx:06d}.jpg"
        ok2, buf = cv2.imencode(".jpg", annotated, [cv2.IMWRITE_JPEG_QUALITY, 88])
        if ok2:
            name.write_bytes(buf.tobytes())
            saved += 1
        h, w = frame.shape[:2]
        if w > 0 and h > 0:
            tiles.append(cv2.resize(annotated, (240, int(240 * h / w))))
        if saved >= args.count:
            break
    cap.release()

    if tiles:
        cols = 4
        rows = (len(tiles) + cols - 1) // cols
        th = max(t.shape[0] for t in tiles)
        sheet = np.zeros((rows * th, cols * 240, 3), np.uint8)
        for i, t in enumerate(tiles):
            r, c = divmod(i, cols)
            sheet[r * th:r * th + t.shape[0], c * 240:(c + 1) * 240] = t
        sheet_path = out / "_sheet.jpg"
        ok, buf = cv2.imencode(".jpg", sheet, [cv2.IMWRITE_JPEG_QUALITY, 85])
        if ok:
            sheet_path.write_bytes(buf.tobytes())
        print(f"[label] 缩略图墙: {sheet_path}")
    print(f"[label] 导出 {saved} 张抽帧到 {out}")
    print("[label] 下一步：看 _sheet.jpg 选帧，再用 tile / mark 子命令标注")
    return 0


def cmd_tile(args: argparse.Namespace) -> int:
    if args.label not in TILE_LABELS:
        raise SystemExit(f"--label 必须是 {TILE_LABELS} 之一")
    parts = [float(v) for v in args.bbox.split(",")]
    if len(parts) != 4:
        raise SystemExit("--bbox 需要 4 个数: y0,y1,x0,x1（0–1 的相对坐标）")
    path = Path(args.labels)
    data = load(path)
    data.setdefault("tile_samples", []).append({
        "frame": int(args.frame),
        "bbox_rel": parts,
        "label": args.label,
        "note": args.note or "",
    })
    save(path, data)
    print(f"[label] 已加入砖型标注，共 {len(data['tile_samples'])} 条 -> {path}")
    return 0


def cmd_mark(args: argparse.Namespace) -> int:
    if args.kind not in MARK_KINDS:
        raise SystemExit(f"--kind 必须是 {MARK_KINDS} 之一")
    path = Path(args.labels)
    data = load(path)
    key = f"{args.kind}_frames"
    data.setdefault(key, []).append({
        "frame": int(args.frame),
        "value": args.value,
        "note": args.note or "",
    })
    save(path, data)
    print(f"[label] 已记录 {args.kind} @ frame {args.frame} = {args.value}"
          f"，该类别共 {len(data[key])} 条")
    return 0


def cmd_summary(args: argparse.Namespace) -> int:
    data = load(Path(args.labels))
    print(f"标注文件: {args.labels}")
    print(f"  视频: {data.get('video') or '(未设置)'}")
    print(f"  相机: {data.get('camera') or '(未设置)'}")
    print(f"  砖型样本: {len(data.get('tile_samples', []))} 条")
    for key in ("crosswalk_frames", "traffic_light_frames", "stairs_frames"):
        print(f"  {key}: {len(data.get(key, []))} 条")
    if not data.get("crosswalk_frames") and not data.get("traffic_light_frames"):
        print("\n提示：斑马线/红绿灯仍无标注，评测报告里这两项会显示 N/A。")
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="label_tool", description="评测标注工具")
    ap.add_argument("--labels", default=str(DEFAULT_LABELS))
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("dump", help="导出抽帧图")
    d.add_argument("--video", required=True)
    d.add_argument("--count", type=int, default=24)
    d.add_argument("--out", default=str(ROOT / "eval" / "dump"))
    d.set_defaults(func=cmd_dump)

    t = sub.add_parser("tile", help="标注砖型裁剪样本")
    t.add_argument("--frame", type=int, required=True)
    t.add_argument("--bbox", required=True, help="y0,y1,x0,x1（0–1 相对坐标）")
    t.add_argument("--label", required=True, choices=TILE_LABELS)
    t.add_argument("--note", default="")
    t.set_defaults(func=cmd_tile)

    m = sub.add_parser("mark", help="标记斑马线/红绿灯/台阶出现的帧")
    m.add_argument("--kind", required=True, choices=MARK_KINDS)
    m.add_argument("--frame", type=int, required=True)
    m.add_argument("--value", default="present")
    m.add_argument("--note", default="")
    m.set_defaults(func=cmd_mark)

    s = sub.add_parser("summary", help="查看标注概览")
    s.set_defaults(func=cmd_summary)
    return ap


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    args.labels = args.labels
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
