/**
 * Built-in dashboard renderer (GDI+ only, no browser needed).
 *
 * Why this exists: the web UI renders themes and POSTs frames to the service.
 * But when the app auto-starts at boot, no browser is open, so nobody would
 * push anything. This module draws a default gauge dashboard directly in C++
 * so the device shows something useful immediately after boot.
 *
 * Layout matches the 320x240 landscape canvas; rotation to the device's
 * 240x320 JPEG is applied at encode time (see gdi_capture.h).
 */
#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>

#include "gdi_capture.h"
#include "gdiplus_scope.h"
#include "protocol.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

namespace coolleo {

class DashboardRenderer {
public:
  DashboardRenderer() = default;

  /** 先初始化 GDI+（进程内只需一次，这里幂等） */
  bool init() {
    if (inited_) return true;
    if (!GdiPlusScope::ensure()) return false;
    inited_ = true;
    return true;
  }

  void shutdown() {
    if (inited_) {
      GdiPlusScope::release();
      inited_ = false;
    }
  }

  /**
   * 渲染一帧默认仪表盘并编码为 JPEG（含 90 度旋转）。
   *
   * @param cpu      0..100
   * @param mem      0..100
   * @param disk     0..100
   * @param title    顶部标题
   * @param subtitle 底部小字
   */
  bool render(double cpu, double mem, double disk, const std::wstring& title,
              const std::wstring& subtitle, std::vector<uint8_t>& out) {
    if (!init()) return false;

    const int W = SCREEN_WIDTH;
    const int H = SCREEN_HEIGHT;

    Gdiplus::Bitmap bmp(W, H, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    // 背景
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 10, 14, 20));
    g.FillRectangle(&bg, 0, 0, W, H);

    // 顶部标题条
    Gdiplus::SolidBrush barBg(Gdiplus::Color(255, 14, 116, 144));
    g.FillRectangle(&barBg, 0, 0, W, static_cast<INT>(H * 0.13));

    // 标题文字
    Gdiplus::FontFamily ff(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&ff, 15.f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat centered;
    centered.SetAlignment(Gdiplus::StringAlignmentCenter);
    centered.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF titleRect(0, 0, static_cast<float>(W), static_cast<float>(H * 0.13));
    g.DrawString(title.c_str(), -1, &titleFont, titleRect, &centered, &whiteBrush);

    // 三个圆环
    struct Gauge {
      double value;
      const wchar_t* label;
      COLORREF color;
      float cx, cy, r, thick;
    };
    const Gauge gauges[3] = {
        {cpu, L"CPU", RGB(0x22, 0xd3, 0xee), W * 0.5f, H * 0.40f, H * 0.17f, 9.f},
        {mem, L"MEM", RGB(0xf4, 0x72, 0xb6), W * 0.22f, H * 0.74f, H * 0.11f, 6.f},
        {disk, L"DSK", RGB(0x4a, 0xde, 0x80), W * 0.78f, H * 0.74f, H * 0.11f, 6.f},
    };

    for (const auto& gz : gauges) {
      float start = 135.f;
      float sweep = 270.f;
      double v = gz.value;
      if (v < 0) v = 0;
      if (v > 100) v = 100;

      // 底环
      Gdiplus::Pen trackPen(Gdiplus::Color(255, 40, 48, 58), gz.thick);
      trackPen.SetStartCap(Gdiplus::LineCapRound);
      trackPen.SetEndCap(Gdiplus::LineCapRound);
      g.DrawArc(&trackPen, gz.cx - gz.r, gz.cy - gz.r, gz.r * 2, gz.r * 2, start, sweep);

      // 进度环
      Gdiplus::Pen arcPen(Gdiplus::Color(255, GetRValue(gz.color), GetGValue(gz.color), GetBValue(gz.color)),
                          gz.thick);
      arcPen.SetStartCap(Gdiplus::LineCapRound);
      arcPen.SetEndCap(Gdiplus::LineCapRound);
      g.DrawArc(&arcPen, gz.cx - gz.r, gz.cy - gz.r, gz.r * 2, gz.r * 2, start,
                static_cast<float>(sweep * v / 100.0));

      // 中间数值
      Gdiplus::Font numFont(&ff, gz.r * 0.52f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
      wchar_t numBuf[32];
      swprintf_s(numBuf, L"%d%%", static_cast<int>(v + 0.5));
      Gdiplus::RectF numRect(gz.cx - gz.r, gz.cy - gz.r * 0.5f, gz.r * 2, gz.r);
      Gdiplus::SolidBrush numBrush(Gdiplus::Color(255, 255, 255, 255));
      g.DrawString(numBuf, -1, &numFont, numRect, &centered, &numBrush);

      // 标签
      Gdiplus::Font lblFont(&ff, gz.r * 0.30f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
      Gdiplus::RectF lblRect(gz.cx - gz.r, gz.cy + gz.r * 0.42f, gz.r * 2, gz.r * 0.6f);
      Gdiplus::SolidBrush lblBrush(Gdiplus::Color(255, 154, 167, 181));
      g.DrawString(gz.label, -1, &lblFont, lblRect, &centered, &lblBrush);
    }

    // 底部小字
    if (!subtitle.empty()) {
      Gdiplus::Font subFont(&ff, 10.f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
      Gdiplus::RectF subRect(0, static_cast<float>(H * 0.92), static_cast<float>(W), static_cast<float>(H * 0.08));
      Gdiplus::SolidBrush subBrush(Gdiplus::Color(255, 58, 80, 96));
      g.DrawString(subtitle.c_str(), -1, &subFont, subRect, &centered, &subBrush);
    }

    // 编码（encoder 内部会做 90 度旋转）
    HBITMAP hbmp = nullptr;
    if (bmp.GetHBITMAP(Gdiplus::Color(255, 0, 0, 0), &hbmp) != Gdiplus::Ok || !hbmp) {
      return false;
    }
    bool ok = encoder_.encodeHBitmap(hbmp, out);
    DeleteObject(hbmp);
    return ok;
  }

private:
  bool inited_ = false;
  GdiScreenCapture encoder_;
};

}  // namespace coolleo
