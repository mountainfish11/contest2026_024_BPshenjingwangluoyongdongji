"""推理设备解析。

为什么要单独一个模块
--------------------
1. 检测器需要知道往哪跑（``cpu`` / ``cuda:0``），但"用户没指定"和"用户指定了 CPU"
   是两件不同的事，必须区分对待；
2. 装了 CPU 版 torch 时 ``torch.cuda.is_available()`` 是 False —— 用户明明有显卡却
   在跑 CPU，这是最容易踩的坑，必须给出可执行的提示而不是静默降级；
3. 这些提示只该打印一次，不该每帧刷屏。
"""
from __future__ import annotations

import warnings
from typing import Optional, Tuple

#: 已经打印过提示，避免重复刷屏
_warned = False


def cuda_status() -> dict:
    """返回当前 torch 的 CUDA 能力描述。"""
    info = {
        "torch": None,
        "cuda_build": None,
        "cuda_available": False,
        "gpu_name": None,
        "gpu_count": 0,
    }
    try:
        import torch
    except Exception:  # noqa: BLE001
        return info
    info["torch"] = torch.__version__
    info["cuda_build"] = torch.version.cuda
    try:
        info["cuda_available"] = bool(torch.cuda.is_available())
        info["gpu_count"] = int(torch.cuda.device_count()) if info["cuda_available"] else 0
        if info["gpu_count"]:
            info["gpu_name"] = torch.cuda.get_device_name(0)
    except Exception:  # noqa: BLE001
        pass
    return info


def resolve_device(spec: Optional[str] = "auto") -> Tuple[str, str]:
    """把配置里的 ``device`` 解析成 ultralytics 能用的设备串。

    返回 ``(设备串, 人类可读描述)``。设备串是 ``"cpu"`` 或 ``"cuda:0"`` 这种。
    """
    global _warned
    status = cuda_status()
    spec = (spec or "auto").strip().lower()

    # ---- 明确要求 CPU：尊重用户
    if spec in ("cpu",):
        return "cpu", f"CPU（{status['torch']}）"

    # ---- 明确要求 CUDA，但用不了：必须大声提醒，不能静默退回 CPU
    if spec.startswith("cuda") or spec.isdigit():
        if status["cuda_available"]:
            index = 0
            if ":" in spec:
                try:
                    index = int(spec.split(":", 1)[1])
                except ValueError:
                    index = 0
            elif spec.isdigit():
                index = int(spec)
            return f"cuda:{index}", f"GPU {index}: {status['gpu_name'] or '未知型号'}"
        if not _warned:
            _warned = True
            warnings.warn(
                f"你指定了 device={spec}，但 torch 用不了 CUDA。\n"
                f"  当前 torch: {status['torch']}（CUDA 编译版本: {status['cuda_build'] or '无，这是 CPU 专用版'}）\n"
                "  装了 CPU 版 torch 时，即使机器有显卡也用不上。\n"
                "  解决办法见 README「用上 GPU」一节，或先跑 `python -m vision_assist.cli --check-gpu`。\n"
                "  本次将退回 CPU 运行。",
                RuntimeWarning,
                stacklevel=2,
            )
        return "cpu", "CPU（已指定 GPU 但不可用，已退回）"

    # ---- auto：有 GPU 就用
    if status["cuda_available"]:
        return "cuda:0", f"GPU: {status['gpu_name'] or '未知型号'}"
    return "cpu", f"CPU（{status['torch']}）"


def gpu_report() -> str:
    """给 ``--check-gpu`` 用的诊断报告（多行文本）。"""
    s = cuda_status()
    lines = ["推理后端检查", "=" * 46]
    lines.append(f"torch 版本      : {s['torch'] or '(未安装)'}")
    lines.append(f"CUDA 编译版本   : {s['cuda_build'] or '无 —— 这是 CPU 专用版 torch'}")

    # 直接问 nvidia-smi 机器上到底有没有 N 卡（torch 看不到时也能查）
    gpu_names = _nvidia_smi_names()
    if gpu_names:
        lines.append(f"机器上的 N 卡   : {', '.join(gpu_names)}")
    else:
        lines.append("机器上的 N 卡   : 未检测到（或没有 nvidia-smi）")

    lines.append(f"torch.cuda 可用 : {s['cuda_available']}")
    if s["cuda_available"]:
        lines.append(f"可见 GPU        : {s['gpu_count']} 个，0 号是 {s['gpu_name']}")
        lines.append("")
        lines.append("✅ 可以用 GPU。启动时加 --device cuda:0（或不加，auto 会自动选 GPU）")
        return "\n".join(lines)

    lines.append("")
    if gpu_names:
        lines.append("❌ 有显卡但 torch 用不上 —— 装的是 CPU 专用版。")
        lines.append("")
        lines.append("   在【你自己的终端】里执行（注意用 --index-url，否则还是装到 CPU 版）：")
        lines.append("")
        lines.append("     python -m pip install \"torch==2.6.0+cu124\" \"torchvision==0.21.0+cu124\" ^")
        lines.append("         --index-url https://download.pytorch.org/whl/cu124")
        lines.append("")
        lines.append("   （约 2.4 GB，装完重开终端再跑本检查）")
        lines.append("")
        lines.append("   备选：驱动比较新（≥560）可以装更新的 2.9.1+cu126，速度差别不大。")
    else:
        lines.append("❌ 没检测到 NVIDIA 显卡。")
        lines.append("   如果是 Intel 核显/AMD 显卡，可以走 OpenVINO 或 DirectML，但迁移成本更高；")
        lines.append("   本项目的实时性在纯 CPU 上也够用（VGA 下约 16 FPS）。")
    return "\n".join(lines)


def _nvidia_smi_names() -> list:
    """用 nvidia-smi 查显卡型号（不依赖 torch，CPU 版 torch 也能查到）。"""
    import shutil
    import subprocess

    exe = shutil.which("nvidia-smi")
    if not exe:
        return []
    try:
        out = subprocess.run(
            [exe, "--query-gpu=name", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=10,
        )
        if out.returncode != 0:
            return []
        return [line.strip() for line in out.stdout.splitlines() if line.strip()]
    except Exception:  # noqa: BLE001
        return []
