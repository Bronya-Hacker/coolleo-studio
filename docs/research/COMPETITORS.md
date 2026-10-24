# 同类副屏软件调研

> 目标：找出别人的做法，把能用的吸收进 Coolleo Studio。
> 调研日期：2025-10

## 一、竞品技术栈对比

| 软件 | 接收端 | 传输 | 虚拟屏 | 关键做法 |
|---|---|---|---|---|
| **SpaceDesk** | 客户端 App | 局域网 TCP/UDP | ✅ 自研 IDD 驱动 | 驱动级虚拟显示器 + 自有压缩协议 |
| **Duet Display** | 客户端 App | USB / WiFi | ✅ 驱动 | USB 走自有驱动，延迟最低 |
| **Deskreen** | **浏览器** | WebRTC | ❌ | Electron 主机 + WebRTC 推流，接收端零安装 |
| **Virtual Display Driver** | 无（纯驱动） | — | ✅ 开源 IDD | 给系统加虚拟显示器，配合 OBS/Sunshine/串流用 |
| **Sunshine + Moonlight** | 客户端 | 局域网 | ✅ 配 VDD | H.264/H.265 **硬件编码**，低延迟串流 |
| **AIDA64 LCD** | 硬件屏 | USB HID | ❌ | 直接推传感器数据，只发数值不发图 |
| **Turing Smart Screen** | 小屏 | USB 串口 | ❌ | 类 MJPEG 推流 + 硬件解码 |

## 二、可借鉴的四条路线

### 路线 1：虚拟显示器驱动（SpaceDesk / VDD 的做法）

**核心**：装一个 IDD（Indirect Display Driver），Windows 就多一块"显示器"，可以把窗口拖进去，程序再捕获它。

**开源方案：[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**

```powershell
winget install --id=VirtualDrivers.Virtual-Display-Driver -e
```

| 项 | 情况 |
|---|---|
| 签名 | **有官方签名版本，不需要开测试模式** |
| 支持 | 自定义分辨率/刷新率/HDR/EDID 模拟 |
| 配套 | VDC（Virtual Driver Control）图形管理 |
| 风险 | 与重大 GPU 驱动更新可能冲突，极端情况开机黑屏（可用 Safe Mode 卸载恢复） |

**对我们的价值**：装上后我的软件「整个显示器」下拉里会直接出现这块虚拟屏，用户就能把任意窗口拖进去 → 真正的副屏体验，不用改我的代码。

### 路线 2：浏览器做接收端（Deskreen 的做法）

Deskreen = Electron + WebRTC，接收端只要浏览器。

**我们已经在用这个模型**（C++ 后台 + 浏览器 UI + HTTP/WebSocket）。差别在于 Deskreen 用 WebRTC 传输视频，我们用 HID 直推。

### 路线 3：硬件编码 + 低延迟串流（Sunshine 的做法）

用 NVENC / QSV / AMF 硬编码 H.264，延迟可到 10ms 级。

**对我们的价值：有限**。因为设备固件只认 JPEG，解码端是屏幕自己的芯片，换不了 H.264。这条路只能用于"局域网投到另一台设备"，不适用当前硬件。

### 路线 4：只发数据不发图（AIDA64 LCD 的做法）

AIDA64 推小屏不传图像，只传传感器数值，屏幕端自己画。

**对我们的价值**：这是**带宽受限场景的最优解**，但前提是设备固件支持"接收数值并自绘"。我们的设备只吃 JPEG，所以只能退而求其次：**在软件侧做画面变化检测，静止时不重发**。

## 三、我们的真实约束

| 项 | 数值 | 说明 |
|---|---|---|
| 屏幕 | 320×240 (转 90° 后 240×320) | 已确认 |
| 单帧 JPEG | 8~20 KB | 取决于画面复杂度 |
| 每帧包数 | 9~20 包 | 每包 1025 字节 |
| **实测上限** | **~20 fps** | HID 中断传输带宽决定，这是硬天花板 |
| 单帧耗时 | ~50 ms | 20 包 × 2.5ms |

**结论**：20fps 是硬件上限，任何软件优化都突破不了。优化方向只能是：
1. **同等带宽下提升观感**（自适应质量）
2. **减少无意义传输**（静止帧跳过）
3. **让交互更顺畅**（状态机、热键）

## 四、决定采纳的改进（按优先级）

### P0 — 已有，但要补齐

| # | 改进 | 现状 |
|---|---|---|
| 1 | DXGI 静止帧复用上帧 | ✅ 已做（`AcquireNextFrame` 超时复用 `lastJpeg_`） |
| 2 | 网页推送的静止帧跳过 | ❌ **没做** — 网页每帧都编码+POST，白费带宽 |
| 3 | 自适应 JPEG 质量 | ❌ 没做 — 场景复杂时帧过大导致掉帧 |

### P1 — 立刻做

| # | 改进 | 收效 |
|---|---|---|
| 4 | 帧差分（变化幅度低于阈值不发） | 省 30~60% 带宽，静止画面几乎零传输 |
| 5 | 带宽自适应质量（帧太大降 q，太小升 q） | 稳定帧率，避免大帧卡顿 |
| 6 | 副屏暂停/恢复（不关闭，保留配置） | 比"关闭再开"快，体验好 |

### P2 — 加分项

| # | 改进 | 收效 |
|---|---|---|
| 7 | 全局热键（Ctrl+Alt+1/2/3 切换模式） | 不用回浏览器 |
| 8 | 场景预设（仪表盘/副屏/双拼） | 一键切换 |
| 9 | 虚拟屏自动检测（认出 VDD 的显示器） | 装 VDD 后自动可用 |
| 10 | 托盘菜单补全（当前只有开关） | 常驻体验完整 |

## 五、参考链接

- [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) — 开源 IDD 驱动，winget 可装
- [Deskreen](https://github.com/pavlobu/deskreen) — Electron + WebRTC 浏览器副屏
- [Deskreen 架构文档](https://deepwiki.com/pavlobu/deskreen/2-architecture)
- [Sunshine](https://github.com/LizardByte/Sunshine) — 硬件编码低延迟串流
