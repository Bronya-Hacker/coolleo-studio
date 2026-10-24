/**
 * System metrics - CPU / memory / disk / network
 *
 * IMPORTANT: PDH's "\Processor(_Total)\% Processor Time" is a RATE counter.
 * It needs >=1s between PdhCollectQueryData() calls to produce a sane value.
 * Calling it every frame (builtin dashboard runs at 20fps) makes it report
 * garbage like 100% -> 0% -> 12% -> 100% within one second.
 *
 * Fix: internal throttle (<=1 sample/sec) + EMA smoothing so the displayed
 * value moves smoothly instead of spiking.
 *
 * Temperature/GPU need LibreHardwareMonitor's WMI namespace; 0 if absent.
 */
#pragma once

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>

#include <map>
#include <string>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace coolleo {

using Metrics = std::map<std::string, double>;

/** 单调时钟毫秒（稳定，不受系统时间调整影响） */
inline uint64_t metricsNowMs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<uint64_t>(now.QuadPart * 1000 / freq.QuadPart);
}

/**
 * 采集系统指标。
 *
 * 内部节流：真实采集最多每秒一次，1 秒内重复调用直接返回缓存。
 * 这样无论调用方多快（内置仪表盘 20fps、网页轮询 1Hz），
 * PDH 速率计数器都能拿到合理间隔。
 *
 * CPU 值额外做 EMA 平滑（0.7 旧 + 0.3 新），避免单点抖动。
 */
inline const Metrics& sampleMetrics() {
  static Metrics cached;
  static uint64_t lastSampleMs = 0;
  static bool firstRun = true;
  static double cpuSmoothed = 0.0;

  const uint64_t now = metricsNowMs();

  // ---- 节流：900ms 内复用上次结果 ----
  if (!firstRun && (now - lastSampleMs) < 900) {
    return cached;
  }
  lastSampleMs = now;

  Metrics m;

  // ---------------- CPU（GetSystemTimes 差值法，比 PDH 更可靠） ----------------
  // PDH 的速率计数器在短间隔/重复采集时会给出垃圾值（跳到 100%）。
  // GetSystemTimes 直接给内核累计时间，自己算差值，间隔由外层节流保证。
  {
    static uint64_t prevIdle = 0;
    static uint64_t prevTotal = 0;

    FILETIME ftIdle{}, ftKernel{}, ftUser{};
    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
      auto toU64 = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
      };
      const uint64_t idle = toU64(ftIdle);
      const uint64_t kernel = toU64(ftKernel);   // 已包含 idle 时间
      const uint64_t user = toU64(ftUser);
      const uint64_t total = kernel + user;

      if (prevTotal != 0 && total > prevTotal) {
        const uint64_t dTotal = total - prevTotal;
        const uint64_t dIdle = idle - prevIdle;
        double raw = (1.0 - static_cast<double>(dIdle) / static_cast<double>(dTotal)) * 100.0;
        if (raw < 0) raw = 0;
        if (raw > 100) raw = 100;

        if (firstRun) {
          cpuSmoothed = raw;                    // 首帧直接采用
        } else {
          // EMA：0.7 旧 + 0.3 新，平滑但不迟钝
          cpuSmoothed = cpuSmoothed * 0.7 + raw * 0.3;
        }
      }
      prevIdle = idle;
      prevTotal = total;
    }
  }
  m["cpu.load"] = cpuSmoothed;

  // ---------------- 内存 ----------------
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    m["mem.totalGB"] = mem.ullTotalPhys / 1073741824.0;
    m["mem.usedGB"] = (mem.ullTotalPhys - mem.ullAvailPhys) / 1073741824.0;
    m["mem.usedPercent"] = static_cast<double>(mem.dwMemoryLoad);
  } else {
    m["mem.totalGB"] = 0;
    m["mem.usedGB"] = 0;
    m["mem.usedPercent"] = 0;
  }

  // ---------------- 磁盘 ----------------
  ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFree{};
  if (GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &totalBytes, &totalFree) && totalBytes.QuadPart > 0) {
    m["disk.freeGB"] = freeBytes.QuadPart / 1073741824.0;
    m["disk.totalGB"] = totalBytes.QuadPart / 1073741824.0;
    m["disk.usedPercent"] =
        100.0 * (1.0 - static_cast<double>(freeBytes.QuadPart) / static_cast<double>(totalBytes.QuadPart));
  } else {
    m["disk.freeGB"] = 0;
    m["disk.totalGB"] = 0;
    m["disk.usedPercent"] = 0;
  }

  // ---------------- 网络（累计字节，网页端做差分） ----------------
  MIB_IFTABLE* table = nullptr;
  ULONG size = 0;
  m["net.recvTotal"] = 0;
  m["net.sendTotal"] = 0;
  if (GetIfTable(nullptr, &size, TRUE) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
    table = reinterpret_cast<MIB_IFTABLE*>(malloc(size));
    if (table && GetIfTable(table, &size, TRUE) == NO_ERROR) {
      DWORD inBytes = 0, outBytes = 0;
      for (DWORD i = 0; i < table->dwNumEntries; i++) {
        MIB_IFROW& row = table->table[i];
        if (row.dwType == IF_TYPE_ETHERNET_CSMACD || row.dwType == IF_TYPE_IEEE80211) {
          inBytes += row.dwInOctets;
          outBytes += row.dwOutOctets;
        }
      }
      m["net.recvTotal"] = static_cast<double>(inBytes);
      m["net.sendTotal"] = static_cast<double>(outBytes);
    }
    if (table) free(table);
  }

  // ---------------- 温度（0 除非装了 LibreHardwareMonitor） ----------------
  m["cpu.temp"] = 0;
  m["gpu.temp"] = 0;
  m["gpu.load"] = 0;

  cached = m;
  firstRun = false;
  return cached;
}

}  // namespace coolleo
