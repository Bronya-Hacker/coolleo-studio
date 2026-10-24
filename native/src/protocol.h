/**
 * Coolleo 散热器屏幕 —— 协议定义（C++，零依赖）
 *
 * 与 src/shared/protocol.ts 保持一致，两处改动需同步。
 *
 * 一帧图像 = 多个 1025 字节 HID 输出报告：
 *
 *   第 1 包（带头）：
 *     [0]      ReportID = 0x00
 *     [1..3]   "CRT"
 *     [4..5]   0x00 0x00
 *     [6..8]   "DRA"
 *     [9..12]  (JPEG 总长 + 32)  4 字节大端 u32
 *     [13]     0xB1               固定标志
 *     [14..21] 8 字节毫秒时间戳
 *     [22..32] 0x00 填充
 *     [33..]   JPEG 前 992 字节
 *
 *   后续 N 包（纯数据，无头）：
 *     [0]      ReportID = 0x00
 *     [1..]    JPEG 后续数据，每包 1024 字节
 */
#pragma once

#include <cstdint>
#include <vector>

namespace coolleo {

constexpr int REPORT_LEN = 1025;
constexpr int FIRST_DATA_OFFSET = 33;
constexpr int FIRST_DATA_MAX = REPORT_LEN - FIRST_DATA_OFFSET;  // 992
constexpr int NEXT_DATA_MAX = REPORT_LEN - 1;                  // 1024

constexpr uint16_t VID = 0x1600;
constexpr uint16_t PID = 0x3006;

/**
 * 画布逻辑尺寸（横版设计）。
 * 发送前会旋转 ROTATION_DEGREES 度，得到设备实际接受的 JPEG 尺寸。
 */
constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 240;

/**
 * 发送前顺时针旋转的角度（实测=90）。
 * 320x240 横版画布 --旋转90°--> 240x320 竖版 JPEG -> 设备显示正确。
 */
constexpr int ROTATION_DEGREES = 90;

/** 长度字段附加常量（总长 + 32） */
constexpr uint32_t LEN_BIAS = 32;
/** 第 1 包偏移 13 的固定标志 */
constexpr uint8_t FLAG_BYTE = 0xB1;

/** 命令字（从偏移 1 开始写） */
namespace cmd {
inline constexpr uint8_t HAN[3] = {'H', 'A', 'N'};                                              // 握手
inline constexpr uint8_t VER[8] = {'C', 'R', 'T', 0, 0, 'V', 'E', 'R'};                       // 版本
inline constexpr uint8_t DIS[8] = {'C', 'R', 'T', 0, 0, 'D', 'I', 'S'};                       // 进入显示
inline constexpr uint8_t DRA[8] = {'C', 'R', 'T', 0, 0, 'D', 'R', 'A'};                       // 图像数据
inline constexpr uint8_t ULEND[10] = {'C', 'R', 'T', 0, 0, 'U', 'L', 'E', 'N', 'D'};          // 上传结束
inline constexpr uint8_t STP[8] = {'C', 'R', 'T', 0, 0, 'S', 'T', 'P'};                       // 停止/熄屏
inline constexpr uint8_t CLE[8] = {'C', 'R', 'T', 0, 0, 'C', 'L', 'E'};                       // 清除
}  // namespace cmd

/** 构造一个只有命令字、无数据的控制包 */
inline std::vector<uint8_t> buildCmd(const uint8_t* cmd, int len) {
  std::vector<uint8_t> buf(REPORT_LEN, 0);
  buf[0] = 0x00;
  for (int i = 0; i < len && 1 + i < REPORT_LEN; i++) buf[1 + i] = cmd[i];
  return buf;
}

template <size_t N>
inline std::vector<uint8_t> buildCmd(const uint8_t (&c)[N]) {
  return buildCmd(c, static_cast<int>(N));
}

/** 把一帧 JPEG 拆成若干 1025 字节报文 */
inline std::vector<std::vector<uint8_t>> splitFrame(const uint8_t* data, size_t len, uint64_t ts) {
  std::vector<std::vector<uint8_t>> out;

  // ---- 第 1 包 ----
  std::vector<uint8_t> pkt(REPORT_LEN, 0);
  pkt[0] = 0x00;
  pkt[1] = 'C';
  pkt[2] = 'R';
  pkt[3] = 'T';
  pkt[6] = 'D';
  pkt[7] = 'R';
  pkt[8] = 'A';

  uint32_t declared = static_cast<uint32_t>(len + LEN_BIAS);
  pkt[9] = static_cast<uint8_t>((declared >> 24) & 0xFF);
  pkt[10] = static_cast<uint8_t>((declared >> 16) & 0xFF);
  pkt[11] = static_cast<uint8_t>((declared >> 8) & 0xFF);
  pkt[12] = static_cast<uint8_t>(declared & 0xFF);
  pkt[13] = FLAG_BYTE;

  // 8 字节时间戳（小端放在 [14..21]）
  for (int i = 0; i < 8; i++) pkt[14 + i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xFF);

  size_t firstN = len < static_cast<size_t>(FIRST_DATA_MAX) ? len : static_cast<size_t>(FIRST_DATA_MAX);
  for (size_t i = 0; i < firstN; i++) pkt[FIRST_DATA_OFFSET + i] = data[i];
  out.push_back(std::move(pkt));

  // ---- 后续包 ----
  size_t off = firstN;
  while (off < len) {
    size_t n = (len - off) < static_cast<size_t>(NEXT_DATA_MAX) ? (len - off) : static_cast<size_t>(NEXT_DATA_MAX);
    std::vector<uint8_t> p(REPORT_LEN, 0);
    p[0] = 0x00;
    for (size_t i = 0; i < n; i++) p[1 + i] = data[off + i];
    out.push_back(std::move(p));
    off += NEXT_DATA_MAX;
  }
  return out;
}

/** 一帧需要多少包 */
inline int packetsFor(size_t len) {
  if (len == 0) return 1;
  if (len <= static_cast<size_t>(FIRST_DATA_MAX)) return 1;
  return 1 + static_cast<int>((len - FIRST_DATA_MAX + NEXT_DATA_MAX - 1) / NEXT_DATA_MAX);
}

}  // namespace coolleo
