/**
 * Screen capture via DXGI Desktop Duplication - multi-monitor aware.
 *
 * Supports capturing any monitor by index (0 = primary / first output).
 * Falls back to GDI when DXGI is unavailable (RDP, old drivers, protected
 * content) so the feature never hard-fails.
 */
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>

#include "gdi_capture.h"
#include "gdiplus_scope.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")

namespace coolleo {

/** 显示器描述 */
struct DisplayInfo {
  int index = 0;          ///< DXGI output index
  int width = 0;          ///< 当前分辨率宽
  int height = 0;         ///< 当前分辨率高
  int left = 0;           ///< 桌面坐标 left
  int top = 0;            ///< 桌面坐标 top
  bool primary = false;   ///< 是否主屏
  std::string name;       ///< 设备名
};

class ScreenCapture {
public:
  ScreenCapture() = default;
  ~ScreenCapture() { shutdown(); }

  /** 初始化 GDI+，并尝试按 outputIndex 建立 DXGI 复制会话 */
  bool init(int outputIndex = 0) {
    if (!inited_) {
      if (!GdiPlusScope::ensure()) return false;
      inited_ = true;
    }
    if (outputIndex != currentOutput_) {
      releaseDxgi();
      currentOutput_ = outputIndex;
    }
    if (!dup_) initDxgi(outputIndex);
    return true;
  }

  /** 只释放 D3D 会话，保留 GDI+ 引用（关副屏时调用，能省 20~30MB） */
  void releaseSession() {
    releaseDxgi();
    currentOutput_ = -1;
  }

  void shutdown() {
    releaseDxgi();
    if (inited_) {
      GdiPlusScope::release();
      inited_ = false;
    }
  }

  bool usingDxgi() const { return dup_ != nullptr; }

  /** 枚举所有显示器（DXGI output） */
  static std::vector<DisplayInfo> enumerateDisplays() {
    std::vector<DisplayInfo> out;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
      return out;
    }

    int globalIdx = 0;
    for (UINT ai = 0;; ai++) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      if (!adapter) break;

      // 判断这块适配器是否连接到主桌面（LUID 匹配）
      for (UINT oi = 0;; oi++) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (!output) break;

        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc))) {
          DisplayInfo di;
          di.index = globalIdx++;
          di.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
          di.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
          di.left = desc.DesktopCoordinates.left;
          di.top = desc.DesktopCoordinates.top;
          di.primary = (desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0);
          // 宽字符转窄
          char nb[64] = {0};
          WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, nb, sizeof(nb) - 1, nullptr, nullptr);
          di.name = nb;
          if (desc.AttachedToDesktop) out.push_back(di);
        }
        output->Release();
      }
      adapter->Release();
    }
    factory->Release();
    return out;
  }

  /**
   * 抓取指定显示器，缩放到 (tw,th)，编码 JPEG。
   * 优先 DXGI，失败自动回退 GDI（回退时只抓主屏）。
   */
  bool grabJpeg(int tw, int th, std::vector<uint8_t>& out) {
    if (!inited_) return false;
    if (dup_ && grabViaDxgi(tw, th, out)) return true;
    return gdi_.grabJpeg(tw, th, out);
  }

  /** 当前捕获的显示器尺寸（调试用） */
  int captureWidth() const { return width_; }
  int captureHeight() const { return height_; }

private:
  void initDxgi(int outputIndex) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL lvl = D3D_FEATURE_LEVEL_11_0;

    // 选一个适配器来创建 D3D 设备：用能枚举到目标 output 的那块
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return;

    // 先定位目标 output 所属的适配器 LUID
    LUID targetLuid{};
    bool found = false;
    int globalIdx = 0;
    for (UINT ai = 0; !found; ai++) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      if (!adapter) break;
      DXGI_ADAPTER_DESC1 ad{};
      adapter->GetDesc1(&ad);
      for (UINT oi = 0;; oi++) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (!output) break;
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(output->GetDesc(&od)) && od.AttachedToDesktop) {
          if (globalIdx == outputIndex) {
            targetLuid = ad.AdapterLuid;
            found = true;
          }
          globalIdx++;
        }
        output->Release();
        if (found) break;
      }
      adapter->Release();
    }

    // 用对应适配器创建 D3D11 设备
    IDXGIAdapter1* targetAdapter = nullptr;
    if (found) {
      for (UINT ai = 0;; ai++) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (ad.AdapterLuid.LowPart == targetLuid.LowPart &&
            ad.AdapterLuid.HighPart == targetLuid.HighPart) {
          targetAdapter = adapter;
          break;
        }
        adapter->Release();
      }
    }

    HRESULT hr = D3D11CreateDevice(targetAdapter, targetAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, &lvl, 1, D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, &lvl, 1,
                             D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    }
    if (targetAdapter) targetAdapter->Release();
    if (FAILED(hr) || !device_) {
      factory->Release();
      return;
    }

    // 在目标适配器上找第 outputIndex 个 output
    IDXGIOutput* targetOutput = nullptr;
    globalIdx = 0;
    for (UINT ai = 0; !targetOutput; ai++) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      if (!adapter) break;
      for (UINT oi = 0;; oi++) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (!output) break;
        DXGI_OUTPUT_DESC od{};
        bool attached = SUCCEEDED(output->GetDesc(&od)) && od.AttachedToDesktop;
        if (attached) {
          if (globalIdx == outputIndex) {
            targetOutput = output;
            break;
          }
          globalIdx++;
        }
        output->Release();
      }
      adapter->Release();
    }
    factory->Release();

    if (!targetOutput) return;

    IDXGIOutput1* output1 = nullptr;
    HRESULT hr2 = targetOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    targetOutput->Release();
    if (FAILED(hr2) || !output1) return;

    hr2 = output1->DuplicateOutput(device_, &dup_);
    output1->Release();
    if (FAILED(hr2) || !dup_) {
      dup_ = nullptr;
      return;
    }

    DXGI_OUTDUPL_DESC ddesc{};
    dup_->GetDesc(&ddesc);
    width_ = static_cast<int>(ddesc.ModeDesc.Width);
    height_ = static_cast<int>(ddesc.ModeDesc.Height);
  }

  void releaseDxgi() {
    if (staging_) { staging_->Release(); staging_ = nullptr; }
    if (dup_) { dup_->Release(); dup_ = nullptr; }
    if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    stagingW_ = stagingH_ = 0;
    lastJpeg_.clear();
  }

  bool ensureStaging(int w, int h) {
    if (staging_ && stagingW_ == w && stagingH_ == h) return true;
    if (staging_) { staging_->Release(); staging_ = nullptr; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
      staging_ = nullptr;
      return false;
    }
    stagingW_ = w;
    stagingH_ = h;
    return true;
  }

  bool grabViaDxgi(int tw, int th, std::vector<uint8_t>& out) {
    if (!dup_ || !ctx_ || !device_) return false;

    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* res = nullptr;
    HRESULT hr = dup_->AcquireNextFrame(0, &info, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      if (!lastJpeg_.empty()) { out = lastJpeg_; return true; }
      return false;
    }
    if (FAILED(hr) || !res) return false;

    ID3D11Texture2D* tex = nullptr;
    hr = res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    res->Release();
    if (FAILED(hr) || !tex) {
      dup_->ReleaseFrame();
      return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (!ensureStaging(static_cast<int>(desc.Width), static_cast<int>(desc.Height))) {
      tex->Release();
      dup_->ReleaseFrame();
      return false;
    }

    ctx_->CopyResource(staging_, tex);
    tex->Release();
    dup_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) return false;

    bool ok = encodeFromBgra(static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch),
                             width_, height_, tw, th, out);

    ctx_->Unmap(staging_, 0);
    if (ok) lastJpeg_ = out;
    return ok;
  }

  bool encodeFromBgra(const uint8_t* src, int srcStride, int srcW, int srcH, int tw, int th,
                      std::vector<uint8_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = srcW;
    bi.bmiHeader.biHeight = -srcH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    void* bits = nullptr;
    HBITMAP srcBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!srcBmp || !bits) {
      if (srcBmp) DeleteObject(srcBmp);
      ReleaseDC(nullptr, screen);
      return false;
    }

    const size_t rowBytes = static_cast<size_t>(srcW) * 4;
    for (int y = 0; y < srcH; y++) {
      memcpy(static_cast<uint8_t*>(bits) + static_cast<size_t>(y) * rowBytes,
             src + static_cast<size_t>(y) * srcStride, rowBytes);
    }

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ oldSrc = SelectObject(mem, srcBmp);

    void* smallBits = nullptr;
    BITMAPINFO bi2{};
    bi2.bmiHeader = bi.bmiHeader;
    bi2.bmiHeader.biWidth = tw;
    bi2.bmiHeader.biHeight = -th;
    HBITMAP smallBmp = CreateDIBSection(screen, &bi2, DIB_RGB_COLORS, &smallBits, nullptr, 0);

    HDC dst = CreateCompatibleDC(screen);
    HGDIOBJ oldDst = SelectObject(dst, smallBmp);
    SetStretchBltMode(dst, HALFTONE);
    StretchBlt(dst, 0, 0, tw, th, mem, 0, 0, srcW, srcH, SRCCOPY);

    bool ok = gdi_.encodeHBitmap(smallBmp, out);

    SelectObject(dst, oldDst);
    DeleteObject(smallBmp);
    DeleteDC(dst);
    SelectObject(mem, oldSrc);
    DeleteObject(srcBmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
  }

  bool inited_ = false;
  int currentOutput_ = -1;

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  IDXGIOutputDuplication* dup_ = nullptr;
  ID3D11Texture2D* staging_ = nullptr;
  int stagingW_ = 0;
  int stagingH_ = 0;
  int width_ = 0;
  int height_ = 0;
  std::vector<uint8_t> lastJpeg_;

  GdiScreenCapture gdi_;
};

}  // namespace coolleo
