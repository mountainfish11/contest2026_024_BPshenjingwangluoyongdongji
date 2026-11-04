"""在局域网里自动找到 ESP32 图传板。

为什么需要它
------------
板子的 IP 会变：
- 自己发热点（AP 模式）-> 固定 ``192.168.4.1``
- 连手机/路由器热点（STA 模式）-> DHCP 随便给，如 ``10.110.4.63``
每次换网络都得回串口翻 ``WiFi OK: x.x.x.x``，很烦。

用法::

    python -m tools.find_board                  # 扫本机所在网段
    python -m tools.find_board --subnet 10.110.4.0/24
    python -m tools.find_board --timeout 0.15   # 扫得更快（网络差时调大）

原理：并发扫描本网段 80 端口，对响应者再抓一次 ``GET /``，页面里含 ``ESP32``
或者 ``图传`` 才算命中——只看「端口开着」会把路由器、打印机都算进去。
"""
from __future__ import annotations

import argparse
import ipaddress
import re
import socket
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import List, Optional, Tuple


def local_ipv4() -> Optional[str]:
    """取本机在「默认出口」上的 IPv4 地址（不会真的发包）。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return None
    finally:
        sock.close()


def guess_subnet() -> Optional[str]:
    ip = local_ipv4()
    if not ip:
        return None
    if ip.startswith("127."):
        return None
    # 常见的 /24 假设（家用/热点场景足够）
    return ip.rsplit(".", 1)[0] + ".0/24"


def port_open(ip: str, port: int = 80, timeout: float = 0.25) -> bool:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        return sock.connect_ex((ip, port)) == 0
    except OSError:
        return False
    finally:
        sock.close()


def fetch_probe(ip: str, timeout: float = 1.5) -> Tuple[bool, str]:
    """抓一次 ``GET /``，返回 ``(是否是 ESP32 图传, 摘要)``。"""
    try:
        sock = socket.create_connection((ip, 80), timeout=timeout)
    except OSError:
        return False, ""

    try:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: " + ip.encode() + b"\r\n"
                     b"Connection: close\r\n\r\n")
        sock.settimeout(timeout)
        buf = b""
        while len(buf) < 32768:
            try:
                chunk = sock.recv(8192)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
    except OSError:
        return False, ""
    finally:
        try:
            sock.close()
        except OSError:
            pass

    text = buf.decode("utf-8", "replace")
    head = text.split("\r\n\r\n", 1)[0]
    match = re.search(r"HTTP/1\.[01]\s+(\d{3})", head)
    status = match.group(1) if match else "???"

    low = text.lower()
    is_board = ("esp32" in low) or ("图传" in text) or ("multipart/x-mixed-replace" in low)
    note = f"HTTP {status}"
    if "esp32-s3" in low:
        note += " · ESP32-S3"
    if "multipart/x-mixed-replace" in low:
        note += " · MJPEG"
    return is_board, note


def scan(subnet: str, timeout: float, workers: int = 96) -> List[Tuple[str, str]]:
    try:
        net = ipaddress.ip_network(subnet, strict=False)
    except ValueError as exc:
        raise SystemExit(f"网段格式不对: {subnet}（例：10.110.4.0/24）") from exc

    hosts = [str(h) for h in net.hosts()]
    if len(hosts) > 1024:
        raise SystemExit(f"网段太大（{len(hosts)} 个地址），请用 /24 这种小网段")
    print(f"扫描 {net} 共 {len(hosts)} 个地址，端口 80 …")

    found: List[Tuple[str, str]] = []
    done = 0
    lock = threading.Lock()

    def check(ip: str) -> Optional[Tuple[str, str]]:
        nonlocal done
        result = None
        if port_open(ip, timeout=timeout):
            ok, note = fetch_probe(ip)
            if ok:
                result = (ip, note)
        with lock:
            done += 1
            if done % 32 == 0:
                print(f"  进度 {done}/{len(hosts)}", flush=True)
        return result

    with ThreadPoolExecutor(max_workers=workers) as pool:
        for item in pool.map(check, hosts):
            if item:
                found.append(item)
    return found


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="自动查找局域网里的 ESP32 图传板")
    ap.add_argument("--subnet", default=None, help="网段，如 10.110.4.0/24（默认自动推断）")
    ap.add_argument("--timeout", type=float, default=0.25,
                    help="单个地址的端口探测超时（秒），网络慢可以调大到 0.5")
    args = ap.parse_args(argv)

    my_ip = local_ipv4()
    if my_ip:
        print(f"本机地址: {my_ip}")
    subnet = args.subnet or guess_subnet()
    if not subnet:
        raise SystemExit("推断不出网段，请手动指定：--subnet 10.110.4.0/24")

    found = scan(subnet, args.timeout)
    print()
    if not found:
        print("❌ 没找到。常见原因：")
        print("   1) 电脑和板子不在同一网络（板子连了热点，电脑也要连同一个热点）")
        print("   2) 手机热点开了「客户端隔离 / AP 隔离」，设备之间互相不可见")
        print("      -> 在手机热点设置里关掉它，改用路由器或随身 WiFi 更稳")
        print("   3) 板子还没连上 WiFi，退回了 AP 模式，那就用 192.168.4.1")
        print("   4) 网段猜错了，用 --subnet 手动指定")
        return 2

    print(f"✅ 找到 {len(found)} 个疑似图传设备：")
    for ip, note in found:
        print(f"   http://{ip}/stream     ({note})")
    print()
    best = found[0][0]
    print("直接用这条启动：")
    print(f"   python -m vision_assist.cli --serve --source mjpeg:http://{best}/stream "
          f"--target-fps 30 --height 1.2 --pitch 30")
    return 0


if __name__ == "__main__":
    sys.exit(main())
