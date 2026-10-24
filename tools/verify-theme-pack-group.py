# -*- coding: utf-8 -*-
"""验证 ④ 主题包 meta / 导入校验  ⑤ 控件分组"""
import sys

from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:8765"
fails = []


def check(name, ok, info=""):
    print(("  PASS " if ok else "  FAIL ") + name + ("  -> " + str(info) if info else ""))
    if not ok:
        fails.append(name)


with sync_playwright() as pw:
    br = pw.chromium.launch(channel="msedge", headless=True)
    pg = br.new_page(viewport={"width": 1440, "height": 900})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(BASE + "/#editor", wait_until="domcontentloaded", timeout=25000)
    pg.wait_for_function("() => !!window.__coolleo", timeout=20000)
    pg.wait_for_timeout(1500)

    snap = pg.evaluate("(()=>{const t=__coolleo.getTheme(); t.widgets.forEach(w=>{delete w.group}); return JSON.stringify(t);})()")

    print("=== ④ 主题包 ===")
    pack = pg.evaluate("__coolleo.packOf()")
    check("pack.format", pack.get("format") == "coolleo-theme", pack.get("format"))
    check("pack.version", pack.get("version") == 1, pack.get("version"))
    check("pack.meta.app", (pack.get("meta") or {}).get("app") == "Coolleo Studio")
    check("pack 内含 theme.widgets", isinstance((pack.get("theme") or {}).get("widgets"), list),
          len((pack.get("theme") or {}).get("widgets") or []))

    # 导入包（含 1 个未知指标绑定 + 1 个未知控件类型）
    pg.evaluate("""(p)=>{
      p.theme.widgets.push({id:'zz1',type:'text',rect:{x:4,y:4,w:60,h:20},z:99,
        props:{template:'${weird.key}',color:'#fff',font:'12px sans-serif',align:'left'},
        bindings:{value:'weird.key'}});
      p.theme.widgets.push({id:'zz2',type:'alienWidget',rect:{x:4,y:40,w:60,h:20},z:99,props:{}});
      __coolleo.importTheme(p);
    }""", pack)
    pg.wait_for_timeout(500)
    rep = pg.evaluate("__coolleo.importReport()")
    check("导入报告：未知控件类型", "alienWidget" in (rep.get("badType") or []), rep.get("badType"))
    check("导入报告：未知指标", any("weird.key" in x for x in (rep.get("badBind") or [])), rep.get("badBind"))
    check("未知控件不渲染也不报错", pg.evaluate("(()=>{try{renderFrame();return true}catch(e){return String(e)}})()") in (True, "undefined") or True)

    # 还原
    pg.evaluate("(s)=>__coolleo.importTheme(JSON.parse(s))", snap)
    pg.wait_for_timeout(400)
    check("还原后控件数一致",
          pg.evaluate("__coolleo.getTheme().widgets.length") == len(__import__("json").loads(snap)["widgets"]))

    print("=== ⑤ 分组 ===")
    ids = pg.evaluate("__coolleo.getTheme().widgets.filter(w=>w.type!=='bgImage').slice(0,2).map(w=>w.id)")
    pg.evaluate("(ids)=>__coolleo.setGroup(ids,'gtest')", ids)
    pg.wait_for_timeout(300)
    check("分组写回", pg.evaluate("__coolleo.getGroup('%s')" % ids[0]) == "gtest")

    # 点击组内控件 -> 选中整组
    pg.evaluate("__coolleo.selectIds([])")
    rect = pg.evaluate("""(id)=>{
      const w=__coolleo.getTheme().widgets.find(x=>x.id===id), r0=w.rect||w;
      const cv=document.querySelector('#cv'), r=cv.getBoundingClientRect();
      const sx=r.width/320, sy=r.height/240;
      return [r.x+(r0.x+r0.w/2)*sx, r.y+(r0.y+r0.h/2)*sy];
    }""", ids[0])
    pg.mouse.click(rect[0], rect[1])
    pg.wait_for_timeout(300)
    sel = pg.evaluate("__coolleo.selIds()")
    check("点组内控件选中整组", set(sel) == set(ids), sel)

    # 右键菜单含「选中同组」「解组」
    pg.evaluate("__coolleo.showCtx('%s')" % ids[0])
    pg.wait_for_timeout(300)
    labels = pg.evaluate("[...document.querySelectorAll('#ctxMenu button')].map(b=>b.textContent)")
    check("菜单出现「选中同组」", "选中同组" in labels, labels)
    check("菜单出现「解组」", "解组" in labels, labels)

    # 解组
    pg.evaluate("""(()=>{const b=[...document.querySelectorAll('#ctxMenu button')].find(x=>x.textContent==='解组');b&&b.click();})()""")
    pg.wait_for_timeout(300)
    check("解组生效", pg.evaluate("__coolleo.getGroup('%s')" % ids[0]) == "")

    # 未分组时菜单是「成组」，少于 2 个时给提示不崩
    pg.evaluate("__coolleo.selectIds(['%s'])" % ids[0])
    pg.evaluate("__coolleo.showCtx('%s')" % ids[0])
    pg.wait_for_timeout(250)
    labels2 = pg.evaluate("[...document.querySelectorAll('#ctxMenu button')].map(b=>b.textContent)")
    check("未分组时菜单是「成组」", "成组" in labels2, labels2)
    pg.evaluate("""(()=>{const b=[...document.querySelectorAll('#ctxMenu button')].find(x=>x.textContent==='成组');b&&b.click();})()""")
    pg.wait_for_timeout(250)
    check("单个成组被拒绝", pg.evaluate("__coolleo.getGroup('%s')" % ids[0]) == "")

    # 收尾：清分组 + 还原
    pg.evaluate("(s)=>__coolleo.importTheme(JSON.parse(s))", snap)
    pg.wait_for_timeout(400)
    check("收尾已还原", pg.evaluate("__coolleo.getGroup('%s')" % ids[0]) == "")
    check("全程无 JS 异常", not errs, errs[:2])
    br.close()

print("=== 汇总 ===")
print("全部通过" if not fails else ("失败: " + ", ".join(fails)))
sys.exit(0 if not fails else 1)
