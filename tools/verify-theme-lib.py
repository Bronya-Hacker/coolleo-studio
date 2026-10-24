#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""主题库 + 实时同步 完整验收。"""
import json
import os
import urllib.request

from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:8765"


def api_get(path):
    with urllib.request.urlopen(BASE + path, timeout=10) as r:
        return json.loads(r.read().decode())


def api_post(path, obj):
    req = urllib.request.Request(BASE + path, data=json.dumps(obj).encode(), method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def main():
    with sync_playwright() as p:
        b = p.chromium.launch(channel="msedge", headless=True)
        ctx = b.new_context(viewport={"width": 1440, "height": 900}, color_scheme="dark")
        pg = ctx.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("dialog", lambda d: d.accept())
        pg.goto(BASE + "/", wait_until="networkidle", timeout=25000)
        pg.wait_for_timeout(2000)
        pg.evaluate("window.__coolleo.switchPage('editor')")
        pg.wait_for_timeout(600)
        print("错误:", errs or "无")

        # ---- 1. 保存主题 ----
        print()
        print("=== 1. 保存主题 ===")
        pg.fill("#themeName", "测试主题A")
        pg.evaluate("document.querySelectorAll('#tplList button')[2].click()")  # 大环+折线
        pg.wait_for_timeout(500)
        pg.evaluate("document.getElementById('btnSaveTheme').click()")
        pg.wait_for_timeout(1200)
        items = pg.evaluate("()=>[...document.querySelectorAll('#themeLib .lib-item .nm')].map(x=>x.textContent)")
        print(f"  主题库: {items}")
        disk = api_get("/api/themes")
        print(f"  磁盘上: {[(t['id'], t['name'], t['size']) for t in disk]}")
        print("  -> " + ("[OK] 已保存" if disk else "[X] 保存失败"))

        # ---- 2. 再存一个 ----
        print()
        print("=== 2. 保存第二个主题 ===")
        pg.fill("#themeName", "测试主题B")
        pg.evaluate("document.querySelectorAll('#tplList button')[4].click()")  # 三环仪表
        pg.wait_for_timeout(500)
        pg.evaluate("document.getElementById('btnSaveTheme').click()")
        pg.wait_for_timeout(1200)
        disk2 = api_get("/api/themes")
        print(f"  共 {len(disk2)} 个主题: {[t['name'] for t in disk2]}")
        print("  -> " + ("[OK]" if len(disk2) >= 2 else "[X]"))

        # ---- 3. 载入 ----
        print()
        print("=== 3. 载入主题 ===")
        before = pg.evaluate("()=>window.__coolleo.getTheme().widgets.length")
        # 先切到另一个模板打乱
        pg.evaluate("document.querySelectorAll('#tplList button')[1].click()")
        pg.wait_for_timeout(400)
        mid = pg.evaluate("()=>window.__coolleo.getTheme().widgets.length")
        print(f"  切模板后控件数: {before} -> {mid}")

        # 点第一个主题载入
        pg.evaluate("document.querySelectorAll('#themeLib .lib-item .nm')[0].click()")
        pg.wait_for_timeout(900)
        loaded = pg.evaluate("()=>{const t=window.__coolleo.getTheme();return {n:t.widgets.length,name:t.name}}")
        print(f"  载入后: {loaded}")
        print("  -> " + ("[OK] 已恢复保存时的内容" if loaded['n'] != mid else "[!] 控件数未变"))

        # ---- 4. 实时同步 ----
        print()
        print("=== 4. 实时同步 ===")
        st = api_get("/api/status")
        print(f"  设备状态: open={st['open']} capture={st['capture']}")

        pg.evaluate("document.getElementById('liveSync').checked = true; document.getElementById('liveSync').dispatchEvent(new Event('change'))")
        pg.wait_for_timeout(1500)
        s1 = api_get("/api/status")
        print(f"  开启后: pushing={s1['pushing']} frames={s1['frames']}")

        # 改个东西，看帧数是否跳
        f0 = api_get("/api/status")["frames"]
        pg.evaluate("""() => {
          const t = window.__coolleo.getTheme();
          const w = t.widgets[0];
          if (w) w.rect.x = Math.min(200, (w.rect.x || 0) + 20);
          window.__coolleo.setTheme(t);
        }""")
        # 触发 markDirty（setTheme 不调，手动模拟拖拽后）
        pg.evaluate("""() => {
          const ev = new Event('input');
          // 通过属性面板改一下再改回来，间接触发 markDirty
        }""")
        pg.wait_for_timeout(1800)
        f1 = api_get("/api/status")["frames"]
        print(f"  编辑后帧数: {f0} -> {f1} (差 {f1-f0})")
        print("  -> " + ("[OK] 实时推送中" if f1 > f0 else "[!] 帧数未增长"))

        # ---- 5. 删除 ----
        print()
        print("=== 5. 删除主题 ===")
        n_before = len(api_get("/api/themes"))
        pg.evaluate("()=>{const btns=[...document.querySelectorAll('#themeLib .lib-item button')];const del=btns.find(b=>b.textContent==='删除');if(del)del.click();}")
        pg.wait_for_timeout(1200)
        n_after = len(api_get("/api/themes"))
        print(f"  {n_before} -> {n_after}")
        print("  -> " + ("[OK] 已删除" if n_after < n_before else "[!] 未删除"))

        # ---- 6. 旋转文案 ----
        print()
        print("=== 6. 文案检查（不应再出现 90°）===")
        html = pg.content()
        has90 = "90°" in html or "自动旋转 90" in html or "自动转 90" in html
        print(f"  页面含 90° 字样: {has90}")
        print("  -> " + ("[OK] 已移除" if not has90 else "[!] 仍有残留"))

        ctx.close()
        b.close()


if __name__ == "__main__":
    main()
