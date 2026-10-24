#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""测量进程资源占用（psutil 版）。

用法:
    py -X utf8 tools/measure_usage.py <进程名> [秒数] [标签]

输出: 每个进程的 平均CPU% / 工作集MB / 私有MB / 线程 / 句柄 + 合计
CPU% 按全核归一（N 核满载 = 100%）。
"""
import os
import sys
import time

import psutil


def measure(name, seconds=10, tag=""):
    cores = psutil.cpu_count(logical=True) or 1
    targets = [p for p in psutil.process_iter(["pid", "name"])
               if p.info["name"] and p.info["name"].lower() == f"{name.lower()}.exe"]

    if not targets:
        print(f"  [{name}] 未运行")
        return None

    # 首轮快照
    first = {}
    for p in targets:
        try:
            p.cpu_percent(None)          # 建立基线
            first[p.pid] = time.time()
        except psutil.Error:
            pass
    if not first:
        print(f"  [{name}] 无法采样")
        return None

    t0 = time.time()
    time.sleep(seconds)

    rows = []
    for p in targets:
        if p.pid not in first:
            continue
        try:
            cpu = p.cpu_percent(None)    # 距上次调用的平均占用（单核百分比）
            cpu /= cores                 # 归一为占全核的百分比
            mi = p.memory_info()
            rows.append({
                "pid": p.pid,
                "cpu": round(cpu, 2),
                "ws": round(mi.rss / 1048576, 1),
                "priv": round(mi.private / 1048576, 1) if hasattr(mi, "private") else round(mi.rss / 1048576, 1),
                "thr": p.num_threads(),
                "hnd": p.num_handles() if hasattr(p, "num_handles") else 0,
            })
        except psutil.Error:
            pass

    if not rows:
        print(f"  [{name}] 采样期间退出")
        return None

    label = tag or name
    print(f"\n  ── {label} ──  {seconds}s  {cores} 逻辑核")
    print(f"  {'PID':>7} {'CPU%':>7} {'工作集MB':>9} {'私有MB':>8} {'线程':>5} {'句柄':>6}")
    for r in rows:
        print(f"  {r['pid']:>7} {r['cpu']:>7.2f} {r['ws']:>9.1f} {r['priv']:>8.1f} {r['thr']:>5} {r['hnd']:>6}")

    total = {
        "tag": label,
        "cpu": round(sum(r["cpu"] for r in rows), 2),
        "ws": round(sum(r["ws"] for r in rows), 1),
        "priv": round(sum(r["priv"] for r in rows), 1),
        "thr": sum(r["thr"] for r in rows),
        "hnd": sum(r["hnd"] for r in rows),
        "n": len(rows),
    }
    print(f"  {'合计':>7} {total['cpu']:>7.2f} {total['ws']:>9.1f} {total['priv']:>8.1f} "
          f"{total['thr']:>5} {total['hnd']:>6}  ({total['n']} 进程)")
    return total


if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "coolleo-svc"
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    tag = sys.argv[3] if len(sys.argv) > 3 else ""
    measure(name, secs, tag)
