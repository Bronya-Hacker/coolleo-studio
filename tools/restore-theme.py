# -*- coding: utf-8 -*-
"""把已保存主题推回后端（重启服务后内存里的主题会丢），并验证后端渲染在跑。"""
import json
import sys
import time
import urllib.request

BASE = "http://127.0.0.1:8765"
TARGET = sys.argv[1] if len(sys.argv) > 1 else "theme_mvi69pa9"


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=15) as r:
        return r.read().decode("utf-8")


def post(path, obj):
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(BASE + path, data=data,
                                 headers={"Content-Type": "application/json; charset=utf-8"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=15) as r:
        return r.read().decode("utf-8")


# 1. 取已保存主题
raw = post("/api/themes/load", {"id": TARGET})
theme = json.loads(raw)
print("[1] loaded from lib:", theme.get("name"), "widgets=", len(theme.get("widgets") or []))

# 2. 推给后端
res = post("/api/theme/sync", theme)
print("[2] sync ->", res)

# 3. 回读确认
back = json.loads(get("/api/theme/get"))
print("[3] backend theme:", back.get("name"), "widgets=", len(back.get("widgets") or []))

# 4. 观察帧速率（浏览器没开时后端应自己渲染）
s1 = json.loads(get("/api/status"))
time.sleep(2.0)
s2 = json.loads(get("/api/status"))
df = s2["frames"] - s1["frames"]
print("[4] frames +%d in 2s => %.1f fps, errors=%d, bytes=%d" %
      (df, df / 2.0, s2["errors"], s2["bytes"] - s1["bytes"]))

ok = (back.get("name") == theme.get("name")
      and len(back.get("widgets") or []) == len(theme.get("widgets") or [])
      and df >= 8 and s2["errors"] == s1["errors"])
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
