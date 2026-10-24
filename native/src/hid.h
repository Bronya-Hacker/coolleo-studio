/**
 * HID 通信层 —— Windows 原生 API（SetupAPI + CreateFile/WriteFile），零第三方依赖
 */
#pragma once

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <string>
#include <vector>

#include "protocol.h"

namespace coolleo {

class HidDevice {
public:
  HidDevice() = default;
  ~HidDevice() { close(); }

  HidDevice(const HidDevice&) = delete;
  HidDevice& operator=(const HidDevice&) = delete;

  /** 枚举所有匹配 VID/PID 的 HID 设备路径 */
  static std::vector<std::wstring> enumerate(uint16_t vid, uint16_t pid) {
    std::vector<std::wstring> paths;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO info = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return paths;

    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &hidGuid, i, &iface); i++) {
      DWORD need = 0;
      SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &need, nullptr);
      if (need == 0) continue;
      auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(new BYTE[need]);
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, need, nullptr, nullptr)) {
        delete[] reinterpret_cast<BYTE*>(detail);
        continue;
      }

      // 打开读取属性
      HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == vid && attr.ProductID == pid) {
          paths.emplace_back(detail->DevicePath);
        }
        CloseHandle(h);
      }
      delete[] reinterpret_cast<BYTE*>(detail);
    }
    SetupDiDestroyDeviceInfoList(info);
    return paths;
  }

  bool open(uint16_t vid, uint16_t pid) {
    close();
    auto paths = enumerate(vid, pid);
    if (paths.empty()) {
      lastError_ = L"未找到设备";
      return false;
    }
    return openPath(paths[0]);
  }

  bool openPath(const std::wstring& path) {
    close();
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      handle_ = nullptr;
      lastError_ = L"CreateFile 失败: " + std::to_wstring(GetLastError());
      return false;
    }
    path_ = path;
    return true;
  }

  void close() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = nullptr;
  }

  bool isOpen() const { return handle_ != nullptr; }

  /** 写入一个 1025 字节报告；返回实际写入字节数，-1 表示失败 */
  int write(const uint8_t* data, int len) {
    if (!handle_) {
      lastError_ = L"设备未打开";
      return -1;
    }
    DWORD written = 0;
    if (!WriteFile(handle_, data, static_cast<DWORD>(len), &written, nullptr)) {
      lastError_ = L"WriteFile 失败: " + std::to_wstring(GetLastError());
      return -1;
    }
    return static_cast<int>(written);
  }

  bool writePacket(const std::vector<uint8_t>& pkt) { return write(pkt.data(), static_cast<int>(pkt.size())) == static_cast<int>(pkt.size()); }

  std::wstring lastError() const { return lastError_; }
  std::wstring path() const { return path_; }

private:
  HANDLE handle_ = nullptr;
  std::wstring path_;
  std::wstring lastError_;
};

}  // namespace coolleo
