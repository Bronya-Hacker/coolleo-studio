# -*- coding: utf-8 -*-
"""生成 README 用截图：docs/img/*.png"""
import io
import os
import sys

from PIL import Image
from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:8765"
OUT = "docs/img"
os.makedirs(OUT, exist_ok=True)

PAGES = [
    ("home", "首页"),
    ("editor", "主题编辑"),
    ("mirror", "副屏"),
    ("settings", "设置"),
]

os.makedirs(OUT, exist_ok=True)
with sync_playwright() as pw:
    br = pw.chromium.launch(channel="msedge", headless=True)
    pg = br.new_page(viewport={"width": 1440, "height": 900}, device_scale_factor=1)
    pg.goto(BASE + "/#home", wait_until="domcontentloaded", timeout=30000)
    pg.wait_for_function("() => !!window.__coolleo", timeout=20000)
    pg.wait_for_timeout(2500)
    for key, label in PAGES:
        pg.evaluate("(k)=>__coolleo.switchPage(k)", key)
        pg.wait_for_timeout(1200)
        p = os.path.join(OUT, key + ".png")
        pg.screenshot(path=p)
        im = Image.open(p).convert("RGB")
        ex = im.convert("L").getextrema()
        print("%-9s %s  %dx%d  %d KB  luma=%s" % (
            key, os.path.basename(p), im.size[0], im.size[1],
            os.path.getsize(p) // 1024, ex))
        if ex[1] - ex[0] < 20:
            print("  [WARN] 疑似空白页")
    br.close()
print("[OK] 截图完成 ->", OUT)
