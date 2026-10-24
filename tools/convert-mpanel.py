#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把官方 .mPanelTheme 的 manifest.json 转换成我们编辑器的主题 JSON。

官方坐标系 490x367.5 -> 我们 320x240（等比缩放）
类型映射：
  progressBar          -> bar
  time                 -> clock
  cpuTemperature       -> text (绑定 cpu.temp)
  gpuTemperature       -> text (绑定 gpu.temp)
  memoryUsage          -> text (绑定 mem.usedPercent)
  titleText            -> text (静态文字)
背景 mp4 加密不可用 -> 用纯色
"""
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_MANIFEST = os.path.join(ROOT, "主题", "_extracted", "manifest.json")
OUT = os.path.join(ROOT, "主题", "像素场景.converted.json")

TARGET_W, TARGET_H = 320, 240


def main():
    m = json.load(io.open(SRC_MANIFEST, encoding="utf-8"))

    src_w = float(m.get("ThemeBackgroundWidth") or 490)
    src_h = float(m.get("ThemeBackgroundHeight") or 367.5)
    sx = TARGET_W / src_w
    sy = TARGET_H / src_h
    print(f"源画布 {src_w}x{src_h} -> 目标 {TARGET_W}x{TARGET_H}  缩放 {sx:.3f}/{sy:.3f}")

    acts = m.get("Actions", {})
    # 按 SortedText 顺序（图层顺序），取不到就按字典顺序
    order = (m.get("SortedText") or "").split(",")
    order = [x.strip() for x in order if x.strip()]
    items = []
    for uid in order:
        if uid in acts:
            items.append((uid, acts[uid]))
    for uid, a in acts.items():
        if all(uid != x[0] for x in items):
            items.append((uid, a))

    widgets = []
    z = 0
    for uid, a in items:
        s = a.get("Settings", {})
        name = a.get("Name", "")
        uuid = a.get("UUID", "")
        pi = (a.get("PropertyInspectorPath") or "").lower()

        x = float(s.get("x") or 0) * sx
        y = float(s.get("y") or 0) * sy
        w = float(s.get("width") or 40) * sx
        h = float(s.get("height") or 20) * sy

        color = s.get("color") or "#00557f"
        # 字号：官方原始字号缩放后会超出框（35px 字配 23px 框），
        # 这里取 min(原始缩放值, 框高*0.82)，保证字落在框内
        raw_font = float(s.get("fontSize") or 20) * min(sx, sy)
        font_size = min(raw_font, h * 0.82) if h > 0 else raw_font

        wid = f"w{len(widgets)+1:02d}"
        rect = {"x": round(x, 1), "y": round(y, 1), "w": round(w, 1), "h": round(h, 1)}

        # ---- 进度条 ----
        if "progressbar" in uuid.lower() or "progressbar" in pi:
            widgets.append({
                "id": wid, "type": "bar", "rect": rect,
                "props": {
                    "value": 50, "fillColor": color, "bgColor": "#0d1a24",
                    "radius": 3, "direction": "h"
                },
                "z": z,
            })
            # 绑定延后处理：等所有控件收集完，按 y 排序依次分配 CPU/GPU/RAM
            widgets[-1]["_isBar"] = True
            z += 1

        # ---- 时钟 ----
        elif "time" in uuid.lower():
            widgets.append({
                "id": wid, "type": "clock", "rect": rect,
                "props": {
                    "fmt": "HH:mm", "color": color,
                    "font": f"bold {round(font_size)}px sans-serif",
                    "showDate": "1", "dateColor": color,
                    "dateFont": f"{round(font_size*0.28)}px sans-serif",
                    "align": "center"
                },
                "z": z,
            })
            z += 1

        # ---- CPU 温度 ----
        elif "temperature" in uuid.lower() and "gpu" not in uuid.lower():
            widgets.append({
                "id": wid, "type": "text", "rect": rect,
                "props": {
                    "template": "${value}°", "color": color,
                    "font": f"bold {max(9, round(font_size))}px sans-serif",
                    "align": "center"
                },
                "bindings": {"value": "cpu.temp"}, "z": z,
            })
            z += 1

        # ---- GPU 温度 ----
        elif "gpu_temperature" in uuid.lower() or ("temperature" in uuid.lower() and "gpu" in uuid.lower()):
            widgets.append({
                "id": wid, "type": "text", "rect": rect,
                "props": {
                    "template": "${value}°", "color": color,
                    "font": f"bold {max(9, round(font_size))}px sans-serif",
                    "align": "center"
                },
                "bindings": {"value": "gpu.temp"}, "z": z,
            })
            z += 1

        # ---- 内存 ----
        elif "memory" in uuid.lower():
            widgets.append({
                "id": wid, "type": "text", "rect": rect,
                "props": {
                    "template": "${value}%", "color": color,
                    "font": f"bold {max(9, round(font_size))}px sans-serif",
                    "align": "center"
                },
                "bindings": {"value": "mem.usedPercent"}, "z": z,
            })
            z += 1

        # ---- 纯文字（CPU/GPU/RAM 标签）----
        else:
            text = s.get("text") or name or ""
            if not text:
                continue
            widgets.append({
                "id": wid, "type": "text", "rect": rect,
                "props": {
                    "template": text, "color": color,
                    "font": f"bold {max(9, round(font_size))}px sans-serif",
                    "align": "left"
                },
                "z": z,
            })
            z += 1

    # 进度条按 y 从上到下依次绑定 CPU / GPU / 内存
    bars = sorted([w for w in widgets if w.get("_isBar")], key=lambda w: w["rect"]["y"])
    seq = ["cpu.load", "gpu.load", "mem.usedPercent"]
    for i, b in enumerate(bars):
        b["bindings"] = {"value": seq[i] if i < len(seq) else "cpu.load"}
    for w in widgets:
        w.pop("_isBar", None)

    theme = {
        "id": "official_pixel_scene",
        "name": m.get("Name") or "像素场景",
        "canvas": {"width": TARGET_W, "height": TARGET_H, "fps": 20, "background": "#000000"},
        "widgets": widgets,
    }

    io.open(OUT, "w", encoding="utf-8").write(json.dumps(theme, ensure_ascii=False, indent=2))
    print(f"OK -> {OUT}")
    print(f"  控件 {len(widgets)} 个:")
    for w in widgets:
        b = (w.get("bindings") or {}).get("value", "-")
        tpl = w["props"].get("template") or w["props"].get("label") or w["type"]
        print(f"    {w['id']} {w['type']:6} pos=({w['rect']['x']},{w['rect']['y']}) "
              f"size={w['rect']['w']}x{w['rect']['h']} bind={b:16} {str(tpl)[:20]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
