# -*- coding: utf-8 -*-
"""验证：表达式管道 / hidden / visibleWhen（后端预览图 + 前端 DOM 两层）"""
import io
import json
import sys
import urllib.request

from PIL import Image
from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:8765"
RESTORE = "theme_mvi69pa9"
fails = []


def get(p):
    with urllib.request.urlopen(BASE + p, timeout=20) as r:
        return r.read()


def post(p, o):
    d = json.dumps(o, ensure_ascii=False).encode("utf-8")
    q = urllib.request.Request(BASE + p, data=d, method="POST",
                               headers={"Content-Type": "application/json; charset=utf-8"})
    with urllib.request.urlopen(q, timeout=20) as r:
        return r.read()


def mk(tpl, hidden=False, cond=""):
    return {
        "name": "__条件测试", "category": "custom",
        "canvas": {"width": 320, "height": 240, "fps": 20, "background": "#000000"},
        "widgets": [
            {"id": "bg", "type": "panel", "rect": {"x": 0, "y": 0, "w": 320, "h": 240},
             "z": 0, "props": {"color": "#000000", "radius": 0}, "bindings": {}},
            {"id": "t1", "type": "text", "rect": {"x": 10, "y": 90, "w": 300, "h": 60},
             "z": 2, "hidden": hidden, "visibleWhen": cond,
             "props": {"template": tpl, "color": "#ffffff",
                       "font": "bold 18px Microsoft YaHei", "align": "left", "decimals": "1"},
             "bindings": {"value": "cpu.load"}},
        ],
    }


def prev(theme):
    post("/api/theme/sync", theme)
    return Image.open(io.BytesIO(get("/api/theme/preview"))).convert("RGB")


def diff(a, b):
    pa, pb, n = a.load(), b.load(), 0
    for y in range(a.size[1]):
        for x in range(a.size[0]):
            if pa[x, y] != pb[x, y]:
                n += 1
    return n


def check(name, ok, info=""):
    print(("  PASS " if ok else "  FAIL ") + name + ("  -> " + str(info) if info else ""))
    if not ok:
        fails.append(name)


print("=== 后端（预览图） ===")
try:
    base = prev(mk(""))
    plain = prev(mk("${cpu.load}"))
    check("未隐藏时有文字", diff(base, plain) > 40, diff(base, plain))

    check("hidden=true 不渲染", diff(base, prev(mk("${cpu.load}", hidden=True))) == 0)

    check("visibleWhen 不满足 -> 不渲染",
          diff(base, prev(mk("${cpu.load}", cond="cpu.load>200"))) == 0,
          diff(base, prev(mk("${cpu.load}", cond="cpu.load>200"))))
    check("visibleWhen 满足 -> 正常渲染",
          diff(base, prev(mk("${cpu.load}", cond="cpu.load>=0"))) > 40)

    # 表达式：clamp(0,5) 应把 ~10 压到 5
    clamped = prev(mk("${cpu.load | clamp(0,5)}"))
    check("clamp(0,5) 改变数值显示", diff(plain, clamped) > 20, diff(plain, clamped))
    r0 = prev(mk("${cpu.load | round(0)}"))
    check("round(0) 改变数值显示", diff(plain, r0) > 0, diff(plain, r0))
    # 用恒定指标 mem.totalGB 比较，避开 cpu.load 两次采样不同的干扰
    d1 = prev(mk("${mem.totalGB}"))
    d2 = prev(mk("${mem.totalGB | round(1)}"))
    check("round(1) 与默认 1 位小数一致", diff(d1, d2) == 0, diff(d1, d2))
    noval = prev(mk("${no.such.metric}"))
    check("未知指标显示无值占位", diff(base, noval) > 5, diff(base, noval))
finally:
    post("/api/theme/sync", json.loads(post("/api/themes/load", {"id": RESTORE}).decode("utf-8")))

print("=== 前端（DOM） ===")
with sync_playwright() as pw:
    br = pw.chromium.launch(channel="msedge", headless=True)
    pg = br.new_page(viewport={"width": 1440, "height": 900})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(BASE + "/#editor", wait_until="domcontentloaded", timeout=25000)
    pg.wait_for_function("() => !!window.__coolleo", timeout=20000)
    pg.wait_for_timeout(1500)

    check("无 JS 异常", not errs, errs[:1])
    check("__coolleo 有锁接口", pg.evaluate("!!(__coolleo.setLocked&&__coolleo.setHidden&&__coolleo.setCondition)"))

    check("condOk 空 = true", pg.evaluate("__coolleo.condOk('')") is True)
    check("condOk 不满足 = false", pg.evaluate("__coolleo.condOk('cpu.load>999')") is False)
    check("condOk 满足 = true", pg.evaluate("__coolleo.condOk('cpu.load>=0')") is True)
    check("condOk 非法写法不误伤", pg.evaluate("__coolleo.condOk('garbage')") is True)

    # 图层里的锁/隐按钮
    n_lbtn = pg.evaluate("document.querySelectorAll('#layers .lbtn').length")
    check("图层有锁/隐按钮", n_lbtn >= 2, n_lbtn)

    # 属性面板控件状态
    pg.evaluate("(()=>{const w=__coolleo.getTheme().widgets[0]; __coolleo.selectIds([w.id]);})()")
    pg.wait_for_timeout(300)
    has_state = pg.evaluate("!!document.querySelector('#props .prop-sec') && document.querySelectorAll('#props .chk-row').length>=2")
    check("属性面板有控件状态段", has_state)

    # 隐藏生效：画布像素变化
    wid = pg.evaluate("__coolleo.getTheme().widgets.find(w=>w.type!=='panel').id")
    before = pg.evaluate("document.querySelector('#cv').toDataURL().length")
    pg.evaluate("__coolleo.setHidden('%s', true)" % wid)
    pg.wait_for_timeout(300)
    after = pg.evaluate("document.querySelector('#cv').toDataURL().length")
    check("隐藏后画布重绘", before != after, [before, after])
    pg.evaluate("__coolleo.setHidden('%s', false)" % wid)

    # 锁定后拖动无效
    pg.evaluate("__coolleo.setLocked('%s', true)" % wid)
    pg.wait_for_timeout(200)
    check("锁定状态写回", pg.evaluate("__coolleo.getTheme().widgets.find(w=>w.id==='%s').locked" % wid) is True)
    pg.evaluate("__coolleo.setLocked('%s', false)" % wid)

    # 右键菜单
    box = pg.evaluate("(()=>{const r=document.querySelector('#cv').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2];})()")
    pg.mouse.click(box[0], box[1], button="right")
    pg.wait_for_timeout(300)
    vis = pg.evaluate("(()=>{const m=document.getElementById('ctxMenu');return m? getComputedStyle(m).display : 'none';})()")
    nbtn = pg.evaluate("(()=>{const m=document.getElementById('ctxMenu');return m? m.querySelectorAll('button').length:0;})()")
    check("右键菜单弹出", vis == "block", vis)
    check("右键菜单有菜单项", nbtn >= 4, nbtn)
    pg.keyboard.press("Escape")
    pg.mouse.click(10, 10)
    br.close()

print("=== 汇总 ===")
print("通过 %d / %d" % (0 if fails else 1, 1) if False else ("全部通过" if not fails else "失败: " + ", ".join(fails)))
sys.exit(0 if not fails else 1)
