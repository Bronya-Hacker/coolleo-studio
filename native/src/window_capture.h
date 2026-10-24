/**
 * Window capture - zero-driver way to put any app window on the device screen.
 *
 * Uses PrintWindow(PW_RENDERFULLCONTENT) which works for most windows including
 * Chrome/Electron (GPU-composited). Falls back gracefully when a window refuses
 * to render (exclusive fullscreen games may return black).
 *
 * This is the practical alternative to installing a virtual display driver:
 * keep the window you want open, and it gets mirrored to the device.
 */
#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>

#include "gdi_capture.h"
#include "protocol.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

namespace coolleo {

/** 可捕获窗口描述 */
struct WindowInfo {
  unsigned long long hwnd = 0;
  std::string title;
  std::string className;
  int width = 0;
  int height = 0;
  bool minimized = false;
};

class WindowCapture {
public:
  WindowCapture() = default;

  /** 枚举所有可见的顶层窗口（有标题、非工具窗口） */
  static std::vector<WindowInfo> enumerate() {
    std::vector<WindowInfo> out;

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
          auto* list = reinterpret_cast<std::vector<WindowInfo>*>(lp);

          if (!IsWindowVisible(hwnd)) return TRUE;
          if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // 跳过子窗口

          // 必须有标题
          wchar_t wtitle[512] = {0};
          GetWindowTextW(hwnd, wtitle, 511);
          if (wtitle[0] == 0) return TRUE;

          // 跳过工具窗口 / 无任务栏按钮的
          LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
          if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

          RECT rc{};
          if (!GetWindowRect(hwnd, &rc)) return TRUE;
          int w = rc.right - rc.left;
          int h = rc.bottom - rc.top;
          if (w <= 0 || h <= 0) return TRUE;

          WindowInfo wi;
          wi.hwnd = reinterpret_cast<unsigned long long>(hwnd);
          char nb[1024] = {0};
          WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, nb, sizeof(nb) - 1, nullptr, nullptr);
          wi.title = nb;

          wchar_t wcls[256] = {0};
          GetClassNameW(hwnd, wcls, 255);
          char cb[256] = {0};
          WideCharToMultiByte(CP_UTF8, 0, wcls, -1, cb, sizeof(cb) - 1, nullptr, nullptr);
          wi.className = cb;

          wi.width = w;
          wi.height = h;
          wi.minimized = IsIconic(hwnd) != 0;

          // 跳过开始菜单/桌面等系统窗口
          if (wi.className == "Progman" || wi.className == "WorkerW" ||
              wi.className == "Shell_TrayWnd" || wi.className == "Windows.UI.Core.CoreWindow") {
            return TRUE;
          }

          list->push_back(wi);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));

    return out;
  }

  /** 窗口是否仍然有效 */
  static bool isValid(unsigned long long hwndVal) {
    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndVal));
    return hwnd && IsWindow(hwnd) != 0;
  }

  /**
   * 抓取窗口内容，缩放到 (tw,th)，旋转后编码 JPEG。
   * 返回 false 表示窗口无效或抓取失败。
   */
  bool grabJpeg(unsigned long long hwndVal, int tw, int th, std::vector<uint8_t>& out) {
    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndVal));
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) return false;  // 最小化抓不到

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    // 防止超大窗口耗尽内存
    if (w > 8192 || h > 8192) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // PW_RENDERFULLCONTENT = 2，能抓到 GPU 合成的内容（Chrome/Electron）
    BOOL ok = PrintWindow(hwnd, mem, 2);

    bool encoded = false;
    if (ok) {
      // 缩放到目标尺寸
      HDC dst = CreateCompatibleDC(screen);
      HBITMAP smallBmp = CreateCompatibleBitmap(screen, tw, th);
      HGDIOBJ oldSmall = SelectObject(dst, smallBmp);
      SetStretchBltMode(dst, HALFTONE);
      StretchBlt(dst, 0, 0, tw, th, mem, 0, 0, w, h, SRCCOPY);

      encoded = encoder_.encodeHBitmap(smallBmp, out);

      SelectObject(dst, oldSmall);
      DeleteObject(smallBmp);
      DeleteDC(dst);
    }

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return encoded;
  }

private:
  GdiScreenCapture encoder_;  // 复用 JPEG 编码（含 90 度旋转）
};

}  // namespace coolleo
