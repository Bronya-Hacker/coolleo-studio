# 主题格式

一份主题 = 一个 JSON 文件。编辑器里保存的主题放在 `native/build/themes/*.json`，
导出的 `.json` 是「主题包」，格式略有不同（见最后一节）。

## 顶层结构

```json
{
  "id":        "theme_abc123",
  "name":      "我的表盘",
  "category":  "custom",
  "canvas":    { "width": 320, "height": 240, "fps": 20, "background": "#000000" },
  "widgets":   [ ... ],
  "meta":      { "app": "Coolleo Studio", "schema": 1, "author": "" },
  "thumb":     "<base64 JPEG，编辑器自动生成>"
}
```

| 字段 | 说明 |
|---|---|
| `id` | 主题唯一 id，ASCII（字母/数字/`_`/`-`）。同名保存会覆盖同一个 `id` |
| `name` | 显示名，可以中文 |
| `category` | 分类 key，内置 `dashboard` / `clock` / `minimal` / `game` / `custom`，也可自定义 |
| `canvas` | 固定 320×240；`fps` 上限 20（HID 带宽决定）；`background` 为兜底色 |
| `thumb` | 128×96 JPEG 的 base64，用于主题库缩略图；手写主题可以不给 |

> `id` / `name` / `category` 必须写在文件**最前面**。后端读分类时只看文件头部一段，
> 而 `thumb` 有几十 KB，写在前面会把它们挤出窗口。

## 控件（widget）

```json
{
  "id":   "w1",
  "type": "arcGauge",
  "rect": { "x": 20, "y": 20, "w": 120, "h": 120 },
  "z":    2,
  "props":      { ... },
  "bindings":   { "value": "cpu.load" },
  "visibleWhen": "cpu.load > 50",
  "locked":  false,
  "hidden":  false,
  "group":   "g1a2"
}
```

| 字段 | 说明 |
|---|---|
| `rect` | 画布坐标，可用数字或 `"50%"` 百分比字符串 |
| `z` | 层级，小的先画（在下面）。背景图层会自动被压到最底 |
| `bindings` | 别名 → 指标键。例 `{"value":"cpu.load"}` 让 `props.value` 每帧取 CPU 占用 |
| `visibleWhen` | 条件表达式，空 = 始终显示 |
| `locked` | 锁定后可选可改属性，但拖不动缩不了 |
| `hidden` | 不渲染，也不响应点击 |
| `group` | 同组控件点任选全组，一起拖动 |

## 控件类型与属性

### `arcGauge` 环形仪表

| 属性 | 默认 | 说明 |
|---|---|---|
| `value` | 50 | 0–100 的百分比（由 `bindings.value` 覆盖） |
| `label` | `"CPU"` | 中间小字 |
| `arcColor` | `#22d3ee` | 弧色 |
| `trackColor` | `#27272a` | 轨道色 |
| `thickness` | 10 | 弧宽 px |
| `font` | `bold 20px sans-serif` | 数值字体 |
| `decimals` | 0 | 小数位 |

### `text` 文字

| 属性 | 默认 | 说明 |
|---|---|---|
| `template` | `CPU ${value}%` | 支持 `${别名}` 与 `${指标键}`，可带管道 |
| `color` | `#ffffff` | |
| `font` | `bold 18px sans-serif` | |
| `align` | `center` | `left` / `center` / `right` |
| `decimals` | 1 | 小数位；留空 = 1 位 |
| `suffix` | `""` | 数值后缀，如 `"%"`
| `noValue` | `"-"` | 指标缺失时显示的占位 |

### `bar` 进度条

`value`(50) `fillColor`(#22d3ee) `bgColor`(#27272a) `radius`(4) `direction`(`h` / `v`)

### `panel` 色块

`color`(#18181b) `radius`(8)

### `image` 图片

`src`(dataURL) `fit`(`cover` / `contain` / `fill`) `radius`(6)

### `video` 视频

`src`(dataURL) `fit` `radius` `muted`(`1`) `loop`(`1`)

> 视频以 base64 存进 JSON，大于 2 MB 编辑器会提示。官方 `.mPanelTheme` 里的 mp4 无法导入。

### `bgImage` 背景图层

`src` `fit` `dim`(0–1 变暗) `radius`。**全局唯一**，添加时会复用已有的那个，导入图片时替换。

### `clock` 时钟

| 属性 | 默认 |
|---|---|
| `fmt` | `HH:mm`（支持 `HH` `mm` `ss`） |
| `color` / `font` | `#ffffff` / `bold 34px sans-serif` |
| `showDate` | 空 = 不显示日期，任意非空 = 显示 |
| `dateColor` / `dateFont` | `#94a3b8` / `12px sans-serif` |

### `sparkline` 折线图

`value`(50) `lineColor`(#22d3ee) `fillColor`(#22d3ee22) `lineWidth`(2) `samples`(40)

历史按 `widget.id` 记账，拖动控件不会清空。

### `icon` 图标

`shape`(`circle` / `square` / `triangle` / `cross` / `bolt`) `color` `bgColor` `size`(0 = 自动填满)

### `divider` 分隔线

`color`(#27272a) `thickness`(1) `style`(`solid` / `dash` / `dot`)

## 阈值预警（`arcGauge` / `bar` / `text` / `sparkline`）

任何带数值的控件都可以加这四个属性：

```json
{ "thWarn": 70, "thWarnColor": "#f59e0b",
  "thDanger": 90, "thDangerColor": "#ef4444" }
```

值 ≥ `thDanger` → 用 `thDangerColor`；否则 ≥ `thWarn` → 用 `thWarnColor`。
留空 = 关闭该级（`Number("") === 0`，所以别写 `0`）。

## 表达式

### 数值管道

```
${cpu.temp | round(1) | clamp(0, 100)}
```

| 函数 | 说明 |
|---|---|
| `round(n)` | 保留 n 位小数（0–6） |
| `floor` / `ceil` / `abs` | 取整 / 向上 / 绝对值 |
| `clamp(lo, hi)` | 夹到区间，默认 0–100 |
| `min(n)` / `max(n)` | 与常数取小 / 取大 |
| `mul(n)` / `div(n)` | 乘 / 除 |

管道可串联，按从左到右执行；`div(0)` 会被忽略。

### 显隐条件

```
cpu.load > 80
cpu.temp >= 70 && gpu.load < 50
```

- 用 `&&` 连接多个比较，全部成立才显示
- 支持 `>` `<` `>=` `<=` `==` `!=`
- 表达式非法时一律当作「显示」，不会把控件吞掉

## 主题包（导出 / 导入）

导出的文件是带信封的：

```json
{
  "format": "coolleo-theme",
  "version": 1,
  "meta":   { "app": "Coolleo Studio", "schema": 1, "exportedAt": "..." },
  "theme":  { ...上面那份主题... }
}
```

导入时：

- 自动拆信封，也能直接吃裸主题 JSON
- 补齐缺失的 `id` / `z` / `props` / `bindings` / `canvas`
- `version` 高于本程序 → 提示「部分属性可能被忽略」
- 有本程序不认的 `type` → 提示哪几种，导入后不显示（不会报错）
- 有本机没有的指标键 → 提示哪几个，显示为 `noValue` 占位

## 后端渲染的支持范围

浏览器负责画预览；浏览器关掉后由后端（GDI+）按同一份 JSON 出图。
后端支持上述全部类型，但两端字体度量略有差异，字号特别小的排版可能有 1–2px 偏差。

后端不认识的字段会被忽略，不会导致加载失败。
