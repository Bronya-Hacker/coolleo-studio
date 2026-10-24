/**
 * GDI fallback capture + shared JPEG encoder.
 *
 * Used when DXGI Desktop Duplication is unavailable (RDP sessions, some VMs,
 * old drivers) or as the explicit slow path. Also exposes encodeHBitmap()
 * so the DXGI path can reuse the same JPEG encoder.
 */
#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <vector>

#include "protocol.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

namespace coolleo {

class GdiScreenCapture {
public:
  GdiScreenCapture() = default;

  /** GDI BitBlt the whole screen, downscale, then JPEG-encode. */
  bool grabJpeg(int tw, int th, std::vector<uint8_t>& out) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) {
      ReleaseDC(nullptr, screen);
      return false;
    }

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP full = CreateCompatibleBitmap(screen, sw, sh);
    HGDIOBJ oldFull = SelectObject(mem, full);

    BOOL blt = BitBlt(mem, 0, 0, sw, sh, screen, 0, 0, SRCCOPY | CAPTUREBLT);

    HDC dst = CreateCompatibleDC(screen);
    HBITMAP smallBmp = CreateCompatibleBitmap(screen, tw, th);
    HGDIOBJ oldSmall = SelectObject(dst, smallBmp);

    bool ok = false;
    if (blt) {
      SetStretchBltMode(dst, HALFTONE);
      StretchBlt(dst, 0, 0, tw, th, mem, 0, 0, sw, sh, SRCCOPY);
      ok = encodeHBitmap(smallBmp, out);
    }

    SelectObject(dst, oldSmall);
    DeleteObject(smallBmp);
    DeleteDC(dst);
    SelectObject(mem, oldFull);
    DeleteObject(full);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
  }

  /** Encode an HBITMAP as JPEG into `out` (quality 85), rotating 90 deg clockwise. */
  bool encodeHBitmap(HBITMAP bmp, std::vector<uint8_t>& out) {
    if (!bmp) return false;

    Gdiplus::Bitmap* gp = Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr);
    if (!gp) return false;

    // 画布是 320x240 横版，设备要 240x320 竖版：顺时针转 90 度
    if (coolleo::ROTATION_DEGREES == 90) {
      gp->RotateFlip(Gdiplus::Rotate90FlipNone);
    } else if (coolleo::ROTATION_DEGREES == 180) {
      gp->RotateFlip(Gdiplus::Rotate180FlipNone);
    } else if (coolleo::ROTATION_DEGREES == 270) {
      gp->RotateFlip(Gdiplus::Rotate270FlipNone);
    }

    bool ok = false;
    CLSID clsid;
    if (getEncoderClsid(L"image/jpeg", clsid)) {
      Gdiplus::EncoderParameters params{};
      ULONG quality = 85;
      params.Count = 1;
      params.Parameter[0].Guid = Gdiplus::EncoderQuality;
      params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
      params.Parameter[0].NumberOfValues = 1;
      params.Parameter[0].Value = &quality;

      HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 0);
      IStream* stream = nullptr;
      if (CreateStreamOnHGlobal(hMem, TRUE, &stream) == S_OK) {
        if (gp->Save(stream, &clsid, &params) == Gdiplus::Ok) {
          SIZE_T sz = GlobalSize(hMem);
          BYTE* p = static_cast<BYTE*>(GlobalLock(hMem));
          if (p && sz > 0) {
            out.assign(p, p + sz);
            ok = true;
          }
          if (p) GlobalUnlock(hMem);
        }
        stream->Release();
      }
      if (hMem) GlobalFree(hMem);
    }
    delete gp;
    return ok;
  }

private:
  static bool getEncoderClsid(const WCHAR* format, CLSID& clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    Gdiplus::ImageCodecInfo* info = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
    if (!info) return false;
    Gdiplus::GetImageEncoders(num, size, info);
    bool found = false;
    for (UINT i = 0; i < num; i++) {
      if (wcscmp(info[i].MimeType, format) == 0) {
        clsid = info[i].Clsid;
        found = true;
        break;
      }
    }
    free(info);
    return found;
  }
};

}  // namespace coolleo
