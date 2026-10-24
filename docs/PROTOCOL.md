# Coolleo 散热器 HID 协议（完整破解）

> 破解方式：`SDLibrary1.dll` 静态反汇编（capstone）+ 官方进程动态抓包（Frida）双向交叉验证
> 设备：VID `0x1600` / PID `0x3006`，产品串 `HOTSPOTEKUSB HID DEMO`，HID UsagePage `0xFFA0`
> 屏幕：**320×240 横向**（设备内部按 240×320 竖向解码后旋转显示）

---

## 一、传输层

| 项 | 值 |
|---|---|
| 接口 | HID（hidapi，静态链接在 `SDLibrary1.dll`） |
| 设备路径 | `\\?\HID#VID_1600&PID_3006#...#{4d1e55b2-f16f-11cf-88cb-001111000030}` |
| 输出报告 | **固定 1025 字节** = 1 字节 ReportID(0x00) + 1024 字节数据 |
| 报告描述符 | INPUT: `0xFFA1` 1B + 511B；OUTPUT: `0xFFA1` 1B + 1023B |
| 写入 API | 官方绕过 kernel32，直接走 `NtWriteFile` |
| 设备主动性 | **无**（0 输入报告、0 feature report、0 ACK，纯单向接收） |

> 关键：`hid.write()` 会自动在缓冲区前补 ReportID 并按报告长度填充，
> 所以从 Python/Node 传入 1024 字节，实际写出 1025 字节是正常的。

---

## 二、一帧图像 = 多个包（这是最容易踩的坑）

**一帧 JPEG 约 20KB（240×320 @ q88），远大于单包容量，必须拆包。**

### 第 1 包（带头）

```
偏移    长度  内容
0x00    1     ReportID = 0x00
0x01    3     "CRT"                     魔数
0x04    2     0x00 0x00
0x06    3     "DRA"                     子命令
0x09    4     (JPEG总长 + 32) 大端 u32   ← 注意是"总长"不是本包长度
0x0D    1     0xB1                      固定标志
0x0E    8     时间戳（毫秒，小端 u64）
0x16    11    0x00 填充
0x21    992   JPEG 前 992 字节
```

### 后续第 N 包（纯数据，无头）

```
偏移    长度  内容
0x00    1     ReportID = 0x00
0x01    1024  JPEG 后续数据
```

### 包数计算

```js
packets = 1 + ceil((jpegLen - 992) / 1024)
// 20380 字节 -> 1 + ceil(19388/1024) = 1 + 19 = 20 包
```

### 实测验证

从抓包里按此规则拼接：

```
第一片 992 字节 + 19 个后续片 × 1024 字节 = 20448 字节
声明总长 20380
拼接后首个 FFD9(EOI) 位置 = 20379  ✅ 精确吻合
PIL 解码成功：240×320, 20380 字节, 41528 种颜色
```

---

## 三、命令字表（反汇编 `mov dl, imm8` 序列提取）

命令字从**偏移 1** 开始，完整形式为 `"CRT" + 00 00 + 子命令`：

| 命令 | 字节 | 用途 | 来源函数 |
|---|---|---|---|
| `HAN` | `48 41 4E` | 握手（独立 3 字节，无 CRT 前缀） | `addHandshakePack` |
| `CRT..VER` | `43 52 54 00 00 56 45 52` | 版本查询 / 心跳 | `0x1e040` |
| `CRT..DIS` | `43 52 54 00 00 44 49 53` | 进入显示模式（唤醒） | `addWakeUpScreenPack` |
| `CRT..DRA` | `43 52 54 00 00 44 52 41` | 图像数据 | `0x19900` / `0x1f222` / `0x200d5` |
| `CRT..ULEND` | `... 55 4C 45 4E 44` | 上传结束 | `0x19d25` |
| `CRT..MOD` | `... 4D 4F 44` | 模式切换 | `0x1690e` |
| `CRT..SET` | `... 53 45 54` | 设置参数 | `0x1d530` |
| `CRT..CLE` | `... 43 4C 45` | 清除 | 多处 |
| `CRT..STP` | `... 53 54 50` | 停止 / 熄屏 | `0x14f61` |
| `CRT..QUCMD` | `... 51 55 43 4D 44` | 通用命令 | `0x16ae2` / `0x194e7` |
| `CRT..LIG` | `... 4C 49 47` | 灯光控制 | `0x149f4` |
| `CRT..KEY` | `... 4B 45 59` | 密钥 | `0x15665` |
| `CRT..BAT` | `... 42 41 54` | 批量 | `0x1eaa3` |
| `CRT..LOG` | `... 4C 4F 47` | 日志 | `0x1e6d3` |

### 初始化序列

```
HAN  ->  CRT..VER  ->  CRT..DIS  ->  CRT..DRA(数据)
```

---

## 四、关键反汇编证据

### 包头长度与 ReportID（`0x1e040`）

```asm
+003C  mov edx, 0x21          ; 33 = 包头长度
+006C  mov edx, [rbx+0x30]    ; maxDataSize
+006F  inc edx                ; +1 = ReportID -> 1025
+0096  movzx edx, byte ptr [rbx+0x98]  ; ReportID 取自成员
```

### 命令字逐字节构造（`0x1e040`，VER 包）

```asm
+00BC  mov dl, 0x43   ; 'C'   索引1
+00DD  mov dl, 0x52   ; 'R'   索引2
+00FE  mov dl, 0x54   ; 'T'   索引3
+011F  mov dl, 0x56   ; 'V'   索引6   ← 跳过 4、5
+0140  mov dl, 0x45   ; 'E'   索引7
+0161  mov dl, 0x52   ; 'R'   索引8
```

### 长度字段写入（`0x19a10`，DRA 包）

```asm
+005C  mov rax, [r14+0x30]    ; QByteArray 数据
+0060  mov edi, [rax+4]       ; 长度（注意 size 在 d+4）
+0063  add edi, 0x20          ; +32
+0068  sar ebx, 0x18
+006B  mov r8d, 8             ; 索引 8 = 长度 MSB
+008F  mov r8d, 9             ; 索引 9
+00B3  mov r8d, 0xa           ; 索引 10
+00d2  mov r8d, 0xb           ; 索引 11 = 长度 LSB
```

> 索引 + 1 = 包偏移，所以长度字段落在包 `[9..12]`，大端序。

### 分块大小

```asm
+0030  mov edx, 0x400         ; 1024 = 每片数据量
```

---

## 五、参考实现（Python）

```python
REPORT_LEN  = 1025
FIRST_DATA  = REPORT_LEN - 33   # 992
NEXT_DATA   = REPORT_LEN - 1    # 1024
LEN_BIAS    = 32
FLAG_BYTE   = 0xB1

def send_frame(dev, jpeg: bytes):
    total = len(jpeg)

    # 第 1 包
    pkt = bytearray(REPORT_LEN)
    pkt[0]      = 0x00
    pkt[1:4]    = b"CRT"
    pkt[6:9]    = b"DRA"
    pkt[9:13]   = (total + LEN_BIAS).to_bytes(4, "big")
    pkt[13]     = FLAG_BYTE
    n1 = min(FIRST_DATA, total)
    pkt[33:33+n1] = jpeg[:n1]
    dev.write(bytes(pkt))

    # 后续包
    off = n1
    while off < total:
        chunk = jpeg[off:off+NEXT_DATA]
        p = bytearray(REPORT_LEN)
        p[0] = 0x00
        p[1:1+len(chunk)] = chunk
        dev.write(bytes(p))
        off += NEXT_DATA
```

---

## 六、踩过的坑（供参考）

| 错误假设 | 实际 |
|---|---|
| JPEG 从偏移 `0x10` 开始 | **偏移 `0x21`(33)** |
| 一包 = 一帧 | **一帧 = 1 + N 包**（约 20 包） |
| 偏移 11-14 是时间戳 | 那是长度字段的一部分，真正的长度在 `[9..12]` 大端 |
| 需要自造 `STA`/`END` 命令 | 实际是 `CRT..ULEND`，且初始化的关键是 `CRT..DIS` |
| 屏幕是 480×480 或 320×240 直接发 | 设备按 **240×320** 解码后旋转成 320×240 显示 |
| 打包需要自己处理 ReportID | `hid.write` 会自动补，传 1024 或 1025 都能工作 |

---

## 七、工具链

| 工具 | 用途 |
|---|---|
| `tools/disasm_sd.py` | 反汇编 SDLibrary1.dll 任意导出函数或 RVA |
| `tools/scan_mov_dl.py` | 扫描 `mov dl,imm8` 序列，批量提取命令字 |
| `tools/frida_lite.py` | 轻量抓包（仅 NtWriteFile），对官方软件影响最小 |
| `tools/frida_capture_startup.py` | 全自动抓官方启动序列（响铃引导操作） |
| `tools/push_correct.py` | **正确协议**的真机推送验证 |
| `tools/try_resolution.py` | 分辨率方向验证 |
| `tools/replay_official.py` | 回放官方原始报文 |
| `tools/hid_enum.py` / `hid_probe.py` | HID 设备枚举与能力探测 |
| `tools/peinfo.ps1` / `strings.ps1` | PE 导出表 / 字符串提取 |
| `tools/mem_scan.py` | 进程内存扫描 |

---

## 八、尚未确认

- [ ] 亮度调节的具体参数（`CRT..SET` 或 `CRT..QUCMD` 的参数布局）
- [ ] 熄屏/亮屏是否用 `CRT..STP` / `CRT..DIS`，还是另有命令
- [ ] `CRT..MOD` 模式切换的参数含义
- [ ] `CRT..ULEND` 是否必需（当前实现未发送也能正常显示）
