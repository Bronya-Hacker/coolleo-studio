#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""主题编辑器增强验收：指标/格式化/阈值/字体/批量/网格/平滑/快捷键/导入导出"""
import json
import urllib.request

from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:8765"
OK = []
BAD = []


def chk(name, cond, detail=""):
    (OK if cond else BAD).append(name)
    print(("  PASS " if cond else "  FAIL ") + name + (("  -> " + str(detail)) if detail else ""))


def api_get(path):
    with urllib.request.urlopen(BASE + path, timeout=10) as r:
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
        pg.wait_for_timeout(1500)
        pg.evaluate("window.__coolleo.switchPage('editor')")
        pg.wait_for_timeout(500)

        print("=== 0. 加载 ===")
        chk("无 JS 异常", not errs, errs)
        chk("__coolleo 存在", pg.evaluate("typeof window.__coolleo==='object'"))
        # 测试会改动当前主题并同步给后端，先拍快照，收尾时还原
        SNAP = pg.evaluate("() => JSON.stringify(window.__coolleo.getTheme())")

        # ---------------------------------------------------------- 1 指标清单
        print("\n=== 1. 指标绑定补全 ===")
        ml = pg.evaluate("window.__coolleo.metricList()")
        chk("指标 12 个", len(ml) == 12, len(ml))
        for k in ["cpu.temp", "gpu.temp", "gpu.load", "disk.totalGB"]:
            chk("含 " + k, k in ml)
        print("   ", ml)

        # ---------------------------------------------------------- 2 单选属性面板
        print("\n=== 2. 单选属性面板 ===")
        tid = pg.evaluate("""() => {
          const t = window.__coolleo.getTheme();
          let w = t.widgets.find(x=>x.type==='text');
          if(!w){
            w = {id:'__testtext', type:'text', rect:{x:10,y:10,w:220,h:40}, z:99,
                 props:{template:'${cpu.load}', color:'#ffffff', font:'bold 18px sans-serif', align:'left', decimals:'1'},
                 bindings:{value:'cpu.load'}};
            t.widgets.push(w);
          }
          window.__coolleo.selectIds([w.id]);
          return w.id;
        }""")
        pg.wait_for_timeout(400)
        labels = pg.evaluate("()=>[...document.querySelectorAll('#props .row label')].map(l=>l.textContent)")
        print("   属性行:", labels)
        chk("有 绑定指标 行", "绑定指标" in labels)
        chk("有 警戒值 行", "警戒值" in labels)
        chk("有 危险色 行", "危险色" in labels)
        chk("有 小数位 行", "小数位" in labels)
        chk("有 后缀 行", "后缀" in labels)
        chk("有 字体 行", "字体" in labels)

        n_opt = pg.evaluate("""() => {
          const rows=[...document.querySelectorAll('#props .row')];
          const r=rows.find(r=>r.querySelector('label') && r.querySelector('label').textContent==='绑定指标');
          return r ? r.querySelectorAll('select option').length : -1;
        }""")
        chk("绑定下拉 13 项(含不绑定)", n_opt == 13, n_opt)

        font_ui = pg.evaluate("""() => {
          const fr=document.querySelector('#props .font-row');
          if(!fr) return null;
          return {
            famIsSelect: fr.children[0].tagName==='SELECT',
            famOpts: fr.children[0].options.length,
            sizeType: fr.children[1].type,
            toggles: [...fr.querySelectorAll('button.toggle')].length
          };
        }""")
        chk("字体控件是下拉", font_ui and font_ui["famIsSelect"], font_ui)
        chk("字族有多个选项", font_ui and font_ui["famOpts"] >= 10, font_ui)
        chk("字号是 number", font_ui and font_ui["sizeType"] == "number", font_ui)
        chk("B/I 两个开关", font_ui and font_ui["toggles"] == 2, font_ui)

        # ---------------------------------------------------------- 3 字体写回
        print("\n=== 3. 字体选择器写回 ===")
        newf = pg.evaluate("""(id) => {
          const fr=document.querySelector('#props .font-row');
          const sel=fr.children[0], size=fr.children[1];
          const bI=[...fr.querySelectorAll('button.toggle')][1];
          sel.value='Consolas'; sel.onchange();
          size.value=28; size.onchange();
          bI.click();
          const t=window.__coolleo.getTheme();
          return t.widgets.find(w=>w.id===id).props.font;
        }""", tid)
        print("   字体串:", newf)
        chk("字体串含 Consolas", "Consolas" in str(newf), newf)
        chk("字体串含 28px", "28px" in str(newf), newf)
        chk("字体串含 italic", "italic" in str(newf), newf)

        # ---------------------------------------------------------- 4 阈值函数
        print("\n=== 4. 阈值配色 ===")
        r = pg.evaluate("""() => ({
          none: thColor({thWarn:'',thDanger:''}, 80),
          warn: thColor({thWarn:60,thWarnColor:'#f59e0b',thDangerColor:'#ef4444'}, 70),
          danger: thColor({thWarn:60,thWarnColor:'#f59e0b',thDanger:80,thDangerColor:'#ef4444'}, 90),
          below: thColor({thWarn:60,thWarnColor:'#f59e0b'}, 30)
        })""")
        chk("无阈值不染色", r["none"] is None, r)
        chk("超警戒 -> 琥珀", r["warn"] == "#f59e0b", r["warn"])
        chk("超危险 -> 红", r["danger"] == "#ef4444", r["danger"])
        chk("未超 -> 不染色", r["below"] is None, r["below"])

        # 真实绘制：临时插一个进度条，超阈值必须变红
        bar = pg.evaluate("""() => {
          const t=window.__coolleo.getTheme();
          theme.widgets.push({ id:'__testbar', type:'bar', z:99,
            rect:{x:20,y:200,w:120,h:12},
            props:{ value:90, fillColor:'#22d3ee', bgColor:'#27272a', radius:3,
                    direction:'h', thWarn:'', thWarnColor:'#f59e0b',
                    thDanger:50, thDangerColor:'#ef4444' },
            bindings:{} });
          window.__coolleo.setSmooth(false);
          renderFrame();
          const d=cvEl.getContext('2d').getImageData(23, 206, 1, 1).data;
          // 再对比不超阈值的情况
          const w=theme.widgets.find(x=>x.id==='__testbar');
          w.props.value=10;
          renderFrame();
          const d2=cvEl.getContext('2d').getImageData(23, 206, 1, 1).data;
          // 收尾
          theme.widgets=theme.widgets.filter(x=>x.id!=='__testbar');
          window.__coolleo.setSmooth(true);
          renderFrame();
          return { danger:[d[0],d[1],d[2]], normal:[d2[0],d2[1],d2[2]] };
        }""")
        chk("超阈值画成红色", bar and bar["danger"][0] > 180 and bar["danger"][1] < 90, bar)
        chk("未超阈值仍是青色", bar and bar["normal"][1] > 150 and bar["normal"][0] < 120, bar)

        # ---------------------------------------------------------- 5 数值格式化
        print("\n=== 5. 数值格式化 ===")
        fmt = pg.evaluate("""() => ({
          d0: numText(12.345, '0'),
          d1: numText(12.345, ''),
          d2: numText(12.345, '2'),
          empty: numText(undefined, 1)
        })""")
        chk("0 位小数 -> 12", fmt["d0"] == "12", fmt["d0"])
        chk("自动 -> 12.3", fmt["d1"] == "12.3", fmt["d1"])
        chk("2 位 -> 12.35", fmt["d2"] == "12.35", fmt["d2"])
        chk("空值 -> 空串", fmt["empty"] == "", fmt["empty"])

        tpl = pg.evaluate("""() => {
          const t=window.__coolleo.getTheme();
          const w=t.widgets.find(x=>x.type==='text');
          if(!w) return null;
          w.props.template='CPU ${cpu.load}';
          w.props.decimals='1'; w.props.suffix='';
          renderFrame();
          return true;
        }""")
        # 直接验证模板替换结果
        txt = pg.evaluate("""() => {
          const w=window.__coolleo.getTheme().widgets.find(x=>x.type==='text');
          const tpl=String(w.props.template||'');
          const m=(typeof metrics!=='undefined'&&metrics)||{};
          return tpl.replace(/\\$\\{([\\w.]+)\\}/g,(s,k)=>{
            return (k in m) ? numText(m[k], w.props.decimals)+(w.props.suffix||'') : '<miss:'+k+'>';
          });
        }""")
        print("   模板渲染:", txt)
        chk("${cpu.load} 被替换（不是 <miss>）", "<miss" not in str(txt), txt)

        # ---------------------------------------------------------- 6 批量
        print("\n=== 6. 多选批量编辑 ===")
        pg.evaluate("""() => {
          theme.widgets.push({id:'__p1',type:'panel',z:80,rect:{x:10,y:150,w:40,h:20},
                              props:{color:'#333333',radius:4},bindings:{}});
          theme.widgets.push({id:'__p2',type:'panel',z:81,rect:{x:60,y:150,w:40,h:20},
                              props:{color:'#333333',radius:4},bindings:{}});
          window.__coolleo.selectIds(['__p1','__p2']);
        }""")
        pg.wait_for_timeout(400)
        batch = pg.evaluate("""() => {
          const box=$('props');
          const grids=box.querySelectorAll('.btn-grid');
          const rows=[...box.querySelectorAll('.row label')].map(l=>l.textContent);
          return { grids:grids.length, rows, hasBind: rows.includes('绑定指标'),
                   hasAlign: rows.includes('对齐'), hasZ: rows.includes('层级/分布') };
        }""")
        print("   批量行:", batch["rows"])
        chk("批量面板出现", batch["grids"] >= 2, batch["grids"])
        chk("有对齐按钮组", batch["hasAlign"])
        chk("有层级/分布", batch["hasZ"])
        chk("有批量绑定", batch["hasBind"])
        chk("列出共有属性(圆角)", "圆角" in batch["rows"], batch["rows"])

        # 批量改属性真的生效
        batch_apply = pg.evaluate("""() => {
          const rows=[...$('props').querySelectorAll('.row')];
          const r=rows.find(x=>x.querySelector('label') && x.querySelector('label').textContent==='圆角');
          if(!r) return { skipped:true, rows: rows.map(x=>x.querySelector('label')?x.querySelector('label').textContent:'') };
          const inp=r.querySelector('input');
          inp.value='11'; inp.onchange();
          const t=window.__coolleo.getTheme();
          const ids=window.__coolleo.selIds();
          return { vals: ids.map(id=>t.widgets.find(w=>w.id===id).props.radius) };
        }""")
        print("   批量圆角:", batch_apply)
        if not batch_apply.get("skipped"):
            chk("批量改圆角全部生效", all(str(v) == "11" for v in batch_apply["vals"]), batch_apply)

        # 对齐按钮真的对齐
        aligned = pg.evaluate("""() => {
          const btns=[...$('props').querySelectorAll('.btn-grid')][0].querySelectorAll('button');
          btns[0].click();   // 左对齐
          const t=window.__coolleo.getTheme();
          const xs=['__p1','__p2'].map(id=>resolveRect(t.widgets.find(w=>w.id===id).rect).x);
          return xs;
        }""")
        chk("左对齐后 X 相同", aligned[0] == aligned[1], aligned)

        pg.evaluate("""() => {
          theme.widgets=theme.widgets.filter(w=>w.id!=='__p1'&&w.id!=='__p2');
          window.__coolleo.selectIds([]);
        }""")
        pg.wait_for_timeout(200)

        # ---------------------------------------------------------- 7 快捷键
        print("\n=== 7. 快捷键 ===")
        nudge = pg.evaluate("""() => {
          const t=window.__coolleo.getTheme();
          const w=t.widgets[0];
          window.__coolleo.selectIds([w.id]);
          const r=resolveRect(w.rect);
          return { id:w.id, x:r.x, y:r.y, n:t.widgets.length };
        }""")
        pg.wait_for_timeout(200)
        pg.click("#cv", position={"x": 5, "y": 5})
        pg.evaluate("window.__coolleo.selectIds(['%s'])" % nudge["id"])
        pg.wait_for_timeout(150)
        pg.keyboard.press("ArrowRight")
        pg.keyboard.press("ArrowDown")
        pg.wait_for_timeout(300)
        after = pg.evaluate("""(id) => {
          const w=window.__coolleo.getTheme().widgets.find(x=>x.id===id);
          const r=resolveRect(w.rect); return {x:r.x,y:r.y};
        }""", nudge["id"])
        chk("方向键右移 1px", after["x"] == nudge["x"] + 1, (nudge, after))
        chk("方向键下移 1px", after["y"] == nudge["y"] + 1, (nudge, after))

        cp = pg.evaluate("""() => {
          const t=window.__coolleo.getTheme();
          const before=t.widgets.length;
          window.__coolleo.copySel();
          window.__coolleo.pasteSel();
          return { before, after: window.__coolleo.getTheme().widgets.length };
        }""")
        chk("复制粘贴新增控件", cp["after"] == cp["before"] + 1, cp)

        allsel = pg.evaluate("""() => {
          document.dispatchEvent(new KeyboardEvent('keydown',{key:'a',ctrlKey:true,bubbles:true}));
          return window.__coolleo.selIds().length;
        }""")
        print("   Ctrl+A 选中:", allsel)
        chk("Ctrl+A 全选 >= 3", allsel >= 3, allsel)

        # ---------------------------------------------------------- 8 网格 / 平滑
        print("\n=== 8. 网格与平滑开关 ===")
        g = pg.evaluate("""() => {
          window.__coolleo.setGrid(16);
          const a=window.__coolleo.getGrid();
          const sn=$('snapSize');
          const ui=sn?sn.value:null;
          window.__coolleo.setGrid(0);
          const b=window.__coolleo.getGrid();
          const snapOff=snapEnabled();     // 必须在 0 状态下判断
          window.__coolleo.setGrid(8);
          return {a,ui,b,snapOff,snapBack:snapEnabled()};
        }""")
        chk("网格可设为 16", g["a"] == 16, g)
        chk("输入框同步", str(g["ui"]) == "16", g)
        chk("网格 0 = 不吸附", g["b"] == 0 and g["snapOff"] is False, g)
        chk("网格 8 = 恢复吸附", g["snapBack"] is True, g)

        sm = pg.evaluate("""() => {
          window.__coolleo.setSmooth(false);
          const off=window.__coolleo.getSmooth();
          window.__coolleo.setSmooth(true);
          return {off, on:window.__coolleo.getSmooth()};
        }""")
        chk("平滑可关闭", sm["off"] is False, sm)
        chk("平滑可开启", sm["on"] is True, sm)

        smoothworks = pg.evaluate("""() => {
          SMOOTH.state.clear();
          const a=smoothTo('t', 100);   // 首次直接到位
          const b=smoothTo('t', 50);    // 之后缓动
          return {a, b};
        }""")
        chk("平滑首帧直达", smoothworks["a"] == 100, smoothworks)
        chk("平滑后续缓动(介于50~100)", 50 < smoothworks["b"] < 100, smoothworks)

        # ---------------------------------------------------------- 9 导入导出
        print("\n=== 9. 主题导入 / 导出 ===")
        io_chk = pg.evaluate("""() => ({
          expBtn: !!$('btnExportTheme'),
          impBtn: !!$('btnImportTheme'),
          file: !!$('themeFile'),
          fn: typeof window.__coolleo.exportTheme === 'function'
        })""")
        chk("导出按钮存在", io_chk["expBtn"], io_chk)
        chk("导入按钮存在", io_chk["impBtn"], io_chk)
        chk("隐藏 file input 存在", io_chk["file"], io_chk)
        chk("exportTheme 可调用", io_chk["fn"], io_chk)

        imp = pg.evaluate("""() => {
          const before=window.__coolleo.getTheme().widgets.length;
          window.__coolleo.importTheme({
            name:'导入测试',
            canvas:{width:320,height:240,fps:20,background:'#000000'},
            widgets:[
              {type:'text',z:0,rect:{x:10,y:10,w:120,h:24},
               props:{template:'HELLO ${cpu.load}',color:'#ffffff',font:'bold 16px sans-serif',
                      align:'left',decimals:1,suffix:''},bindings:{}},
              {type:'bar',z:1,rect:{x:10,y:40,w:120,h:10},
               props:{value:40,fillColor:'#22d3ee',bgColor:'#27272a',radius:3,direction:'h'},
               bindings:{}}
            ]
          });
          const t=window.__coolleo.getTheme();
          return {before, after:t.widgets.length, ids:t.widgets.map(w=>w.id),
                  name:t.name, canvas:t.canvas};
        }""")
        print("   导入结果:", imp)
        chk("导入替换为 2 控件", imp["after"] == 2, imp)
        chk("导入后控件都有 id", all(imp["ids"]), imp["ids"])
        chk("导入后 name 生效", imp["name"] == "导入测试", imp["name"])
        chk("导入后 canvas 补齐", imp["canvas"].get("width") == 320, imp["canvas"])

        # ---------------------------------------------------------- 10 布局未被打乱
        print("\n=== 10. 布局回归 ===")
        pg.reload(wait_until="networkidle")
        pg.wait_for_timeout(1500)
        lay = pg.evaluate("""() => {
          const mid=document.querySelector('.mid'), right=document.querySelector('.right');
          const cv=$('cv'), box=$('props');
          const r=el=>{const b=el.getBoundingClientRect();return {x:Math.round(b.x),w:Math.round(b.width),h:Math.round(b.height)};};
          return { mid:r(mid), right:r(right), cv:r(cv),
                   props:r(box), toolbar:document.querySelectorAll('.layer-tools').length,
                   scroll: document.documentElement.scrollWidth - document.documentElement.clientWidth };
        }""")
        print("   布局:", lay)
        chk("无横向溢出", lay["scroll"] <= 0, lay["scroll"])
        chk("属性面板有宽度", lay["props"]["w"] > 150, lay["props"])
        chk("画布可见", lay["cv"]["w"] > 100, lay["cv"])
        chk("两行工具条", lay["toolbar"] == 2, lay["toolbar"])
        chk("重载后无 JS 异常", not errs, errs)

        # ---------------------------------------------------------- 收尾：还原主题
        pg.evaluate("""(snap) => {
          const t=JSON.parse(snap);
          window.__coolleo.importTheme(t);
        }""", SNAP)
        pg.wait_for_timeout(400)
        back = pg.evaluate("""(snap) => {
          const want=JSON.parse(snap);
          const now=window.__coolleo.getTheme();
          return { wantN: want.widgets.length, nowN: now.widgets.length, name: now.name };
        }""", SNAP)
        chk("收尾已还原原主题", back["nowN"] == back["wantN"], back)

        print("\n================ 汇总 ================")
        print("通过 %d / %d" % (len(OK), len(OK) + len(BAD)))
        if BAD:
            print("失败：")
            for x in BAD:
                print("  - " + x)
        ctx.close()
        b.close()
        return 1 if BAD else 0


if __name__ == "__main__":
    raise SystemExit(main())
