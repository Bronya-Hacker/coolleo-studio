/**
 * Global GDI+ lifetime - initialized once per process.
 *
 * Why: ScreenCapture / DashboardRenderer / WindowCapture all need GDI+, but
 * calling GdiplusStartup multiple times allocates extra font/image caches.
 * This keeps a single reference and shuts down only at process exit.
 */
#pragma once

#include <windows.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace coolleo {

class GdiPlusScope {
public:
  /** 进程内首次调用时初始化，之后幂等 */
  static bool ensure() {
    if (s_refs == 0) {
      Gdiplus::GdiplusStartupInput si;
      si.SuppressBackgroundThread = FALSE;
      if (Gdiplus::GdiplusStartup(&s_token, &si, nullptr) != Gdiplus::Ok) {
        s_token = 0;
        return false;
      }
    }
    s_refs++;
    return true;
  }

  static void release() {
    if (s_refs > 0) {
      s_refs--;
      if (s_refs == 0 && s_token) {
        Gdiplus::GdiplusShutdown(s_token);
        s_token = 0;
      }
    }
  }

  /** 进程退出时强制清理，避免析构顺序问题 */
  static void shutdownAll() {
    s_refs = 0;
    if (s_token) {
      Gdiplus::GdiplusShutdown(s_token);
      s_token = 0;
    }
  }

  static bool ready() { return s_token != 0; }

private:
  inline static ULONG_PTR s_token = 0;
  inline static int s_refs = 0;
};

}  // namespace coolleo
