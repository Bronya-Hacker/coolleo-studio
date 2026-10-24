/**
 * Coolleo Studio 后台服务（C++ / Win32）
 *
 * 设计：
 *   - 无自带 UI：界面在浏览器里（http://127.0.0.1:8765）
 *   - 托盘图标：常驻任务栏右侧，双击打开浏览器
 *   - 本进程只做重活：HID 通信、屏幕捕获（副屏）、系统指标采集
 *   - 与网页通过 WebSocket + HTTP 通信（mongoose 单文件库）
 *
 * 这样分工的好处：界面随便改不用重编译；浏览器崩了不影响设备推送。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <atomic>
#include <cwctype>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mongoose.h"
#include "hid.h"
#include "capture.h"
#include "window_capture.h"
#include "dashboard.h"
#include "theme_store.h"
#include "theme_renderer.h"
#include "gdiplus_scope.h"
#include "metrics.h"
#include "protocol.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT ID_TRAY = 1;
constexpr int PORT = 8765;

HWND g_hwnd = nullptr;
HWND g_appWindow = nullptr;   // 界面窗口句柄（用于激活已有窗口）
HINSTANCE g_hinst = nullptr;

std::atomic<bool> g_running{true};
std::atomic<bool> g_connected{false};
std::atomic<int> g_fps{20};
std::atomic<bool> g_captureEnabled{false};
std::atomic<bool> g_usingDxgi{false};
std::atomic<int> g_captureMonitor{0};
/** 0 = 捕获显示器, 1 = 捕获窗口 */
std::atomic<int> g_captureMode{0};
/** 窗口模式下要抓的窗口句柄 */
std::atomic<unsigned long long> g_captureWindow{0};
/** 最近一次网页推帧的时间戳（ms）；2 秒内有推帧就认为网页在接管 */
std::atomic<uint64_t> g_lastWebFrame{0};
/** 内置仪表盘总开关 */
std::atomic<bool> g_builtinEnabled{true};
/** 副屏暂停（停止抓屏但保留 enable 与配置），恢复后立即继续 */
std::atomic<bool> g_capturePaused{false};

/** 网页推来的最后一帧（关掉浏览器后继续重发，保持画面） */
std::vector<uint8_t> g_lastFrame;
std::mutex g_lastFrameMutex;

/** 当前活动主题 JSON —— 后端据此自己渲染（浏览器关闭后仍然实时更新数据） */
std::string g_themeJson;
std::mutex g_themeMutex;
std::atomic<bool> g_themeDirty{false};
std::atomic<bool> g_themeDiskDirty{false};   // 主题 JSON 需要落盘
/** 强制后端渲染（忽略网页推送），调试/验证用 */
std::atomic<bool> g_forceBackend{false};
uint64_t g_lastFrameDiskSave = 0;

/** 最后一帧的磁盘缓存路径（exe 同级） */
std::string lastFramePath() {
  char buf[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string exe(buf);
  auto pos = exe.find_last_of("\\/");
  std::string base = (pos == std::string::npos) ? "." : exe.substr(0, pos);
  return base + "\\last-frame.jpg";
}

void saveFrameToDisk(const std::vector<uint8_t>& data) {
  std::string p = lastFramePath();
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) return;
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
}

std::string lastThemePath() {
  char buf[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string exe(buf);
  auto pos = exe.find_last_of("\\/");
  std::string base = (pos == std::string::npos) ? "." : exe.substr(0, pos);
  return base + "\\last-theme.json";
}

// 启动时恢复上次使用的主题（否则重启会掉回内置仪表盘）
void loadThemeFromDisk() {
  std::string p = lastThemePath();
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len > 32 && len < 4 * 1024 * 1024) {
    std::string buf(static_cast<size_t>(len), '\0');
    size_t rd = fread(&buf[0], 1, static_cast<size_t>(len), f);
    buf.resize(rd);
    if (rd > 32) {
      std::lock_guard<std::mutex> lk(g_themeMutex);
      g_themeJson = buf;
      g_themeDirty = true;
      g_themeDiskDirty = false;
      printf("[theme] restored last-theme.json (%d bytes)\n",
             static_cast<int>(rd));
    }
  }
  fclose(f);
}

void saveThemeToDisk(const std::string& json) {
  std::string p = lastThemePath();
  if (json.empty()) {
    remove(p.c_str());
    return;
  }
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) return;
  fwrite(json.data(), 1, json.size(), f);
  fclose(f);
}

void loadFrameFromDisk() {
  std::string p = lastFramePath();
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len > 512 && len < 4 * 1024 * 1024) {
    std::lock_guard<std::mutex> lk(g_lastFrameMutex);
    g_lastFrame.resize(static_cast<size_t>(len));
    size_t rd = fread(g_lastFrame.data(), 1, static_cast<size_t>(len), f);
    g_lastFrame.resize(rd);
  }
  fclose(f);
}

// 统计
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_packets{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_errors{0};
// 连续写失败计数：设备被强行终止的进程留下坏状态时，自动重开句柄
std::atomic<int> g_hidFailStreak{0};

coolleo::HidDevice g_hid;
std::mutex g_hidMutex;

// ---------------------------------------------------------------- 工具
std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
  return out;
}

std::string wideToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
  return out;
}

/**
 * 标准 JSON 字符串转义。
 * 必须转义 反斜杠 / 引号 / 控制字符，否则 "\\.\DISPLAY1" 这类
 * Windows 设备路径会让 JSON.parse 报 Bad escaped character。
 */

// 用 WinHTTP 同步下载（绕过浏览器 CORS）
static bool winHttpGet(const std::wstring& url, std::vector<char>& out,
                       std::string& ctype) {
  out.clear();
  ctype.clear();

  URL_COMPONENTS uc;
  memset(&uc, 0, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0}, path[2048] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2047;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

  bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  HINTERNET hs = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                             WINHTTP_ACCESS_TYPE_NO_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) return false;
  WinHttpSetTimeouts(hs, 15000, 15000, 20000, 30000);

  HINTERNET hc = WinHttpConnect(hs, host, uc.nPort, 0);
  if (!hc) { WinHttpCloseHandle(hs); return false; }

  DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path, nullptr,
                                    WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); return false; }

  bool ok = false;
  BOOL sent = WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (!sent) printf("[fetch] SendRequest failed %lu\n", GetLastError());
  BOOL recvd = sent ? WinHttpReceiveResponse(hr, nullptr) : FALSE;
  if (sent && !recvd) printf("[fetch] ReceiveResponse failed %lu\n", GetLastError());
  if (sent && recvd) {
    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (code < 200 || code >= 300) printf("[fetch] HTTP status %lu\n", code);
    if (code >= 200 && code < 300) {
      wchar_t ctbuf[256] = {0};
      DWORD ctl = sizeof(ctbuf);
      if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_TYPE,
                              WINHTTP_HEADER_NAME_BY_INDEX, ctbuf, &ctl,
                              WINHTTP_NO_HEADER_INDEX)) {
        for (int i = 0; ctbuf[i]; i++) ctype += static_cast<char>(ctbuf[i]);
      }
      DWORD avail = 0;
      do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hr, &avail)) break;
        if (avail == 0) { ok = true; break; }
        size_t old = out.size();
        if (out.size() + avail > 64u * 1024 * 1024) break;   // 64MB 上限
        out.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hr, out.data() + old, avail, &got)) { out.resize(old); break; }
        out.resize(old + got);
      } while (avail > 0);
      if (!out.empty()) ok = true;
    }
  }
  WinHttpCloseHandle(hr);
  WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  return ok;
}

// 极简 URL 解码
static std::string urlDecode(const std::string& s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hx = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return 0;
      };
      r += static_cast<char>(hx(s[i + 1]) * 16 + hx(s[i + 2]));
      i += 2;
    } else if (s[i] == '+') {
      r += ' ';
    } else {
      r += s[i];
    }
  }
  return r;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b";  break;
      case '\f': o += "\\f";  break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o;
}

uint64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------- HID 操作
bool sendFrameToDevice(const uint8_t* jpeg, size_t len) {
  std::lock_guard<std::mutex> lk(g_hidMutex);
  if (!g_hid.isOpen()) return false;

  auto packets = coolleo::splitFrame(jpeg, len, nowMs());
  for (const auto& p : packets) {
    int n = g_hid.write(p.data(), static_cast<int>(p.size()));
    if (n != static_cast<int>(p.size())) {
      g_errors++;
      // 连续失败到阈值：关掉重开一次。设备被强杀进程后会卡在不可写状态，
      // 重开句柄即可恢复，不需要用户拔插。
      if (++g_hidFailStreak >= 40) {
        g_hidFailStreak = 0;
        g_hid.close();
        g_connected = g_hid.open(coolleo::VID, coolleo::PID);
      }
      return false;
    }
    g_hidFailStreak = 0;
    g_packets++;
    g_bytes += p.size();
  }
  g_frames++;
  return true;
}

bool sendCmd(const uint8_t* cmd, int len) {
  std::lock_guard<std::mutex> lk(g_hidMutex);
  if (!g_hid.isOpen()) return false;
  auto pkt = coolleo::buildCmd(cmd, len);
  int n = g_hid.write(pkt.data(), static_cast<int>(pkt.size()));
  if (n != static_cast<int>(pkt.size())) {
    g_errors++;
    return false;
  }
  g_packets++;
  g_bytes += pkt.size();
  return true;
}

// ---------------------------------------------------------------- 托盘
void addTrayIcon() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_hwnd;
  nid.uID = ID_TRAY;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(nid.szTip, L"酷里欧屏幕服务 (双击打开界面)");

  Shell_NotifyIconW(NIM_ADD, &nid);
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void removeTrayIcon() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_hwnd;
  nid.uID = ID_TRAY;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void openBrowser() {
  // 用系统默认浏览器打开（普通标签页）
  std::wstring url = L"http://127.0.0.1:" + std::to_wstring(PORT) + L"/";
  ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/**
 * 注册/取消开机自启（HKCU，无需管理员）。
 * 命令：<exe路径> --startup   （--startup 表示静默，不弹浏览器）
 */
void enableAutoStart(bool enable) {
  const wchar_t* kRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  const wchar_t* kName = L"CoolleoStudio";

  HKEY hKey = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRun, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
    return;
  }

  if (enable) {
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
      RegCloseKey(hKey);
      return;
    }
    std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --startup";
    RegSetValueExW(hKey, kName, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(cmd.c_str()),
                   static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(hKey, kName);
  }
  RegCloseKey(hKey);
}

// ---------------------------------------------------------------- HTTP / WebSocket
struct WsConn {
  struct mg_connection* c = nullptr;
};

std::vector<WsConn> g_wsClients;
std::mutex g_wsMutex;

void wsBroadcast(const std::string& msg) {
  std::lock_guard<std::mutex> lk(g_wsMutex);
  for (auto& w : g_wsClients) {
    if (w.c) mg_ws_send(w.c, msg.c_str(), msg.size(), WEBSOCKET_OP_TEXT);
  }
}

void wsBroadcastBinary(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lk(g_wsMutex);
  for (auto& w : g_wsClients) {
    if (w.c) mg_ws_send(w.c, reinterpret_cast<const char*>(data), len, WEBSOCKET_OP_BINARY);
  }
}

/** 静态文件：自己读文件后回复，避开 mongoose 文件服务 API 差异 */
void serveStatic(struct mg_connection* c, struct mg_http_message* hm, const std::string& root) {
  std::string uri(hm->uri.buf, hm->uri.len);
  if (uri == "/" || uri.empty()) uri = "/index.html";
  if (uri.find("..") != std::string::npos) {
    mg_http_reply(c, 403, "", "forbidden");
    return;
  }

  std::string path = root + uri;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    char msg[512];
    snprintf(msg, sizeof(msg), "404 %s not found (root=%s)", uri.c_str(), root.c_str());
    mg_http_reply(c, 404, "Content-Type: text/plain; charset=utf-8\r\n", "%s", msg);
    return;
  }

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len < 0) len = 0;

  std::string content;
  content.resize(static_cast<size_t>(len));
  if (len > 0) {
    size_t rd = fread(&content[0], 1, static_cast<size_t>(len), f);
    content.resize(rd);
  }
  fclose(f);

  const char* type = "application/octet-stream";
  if (uri.size() >= 5 && uri.compare(uri.size() - 5, 5, ".html") == 0) type = "text/html; charset=utf-8";
  else if (uri.size() >= 3 && uri.compare(uri.size() - 3, 3, ".js") == 0) type = "text/javascript; charset=utf-8";
  else if (uri.size() >= 4 && uri.compare(uri.size() - 4, 4, ".css") == 0) type = "text/css; charset=utf-8";
  else if (uri.size() >= 4 && uri.compare(uri.size() - 4, 4, ".png") == 0) type = "image/png";
  else if (uri.size() >= 4 && uri.compare(uri.size() - 4, 4, ".jpg") == 0) type = "image/jpeg";

  char headers[256];
  snprintf(headers, sizeof(headers), "Content-Type: %s\r\nCache-Control: no-cache\r\n", type);
  mg_http_reply(c, 200, headers, "%.*s", static_cast<int>(content.size()), content.data());
}

void handleApi(struct mg_connection* c, struct mg_http_message* hm) {
  std::string body(hm->body.buf, hm->body.len);
  std::string uri(hm->uri.buf, hm->uri.len);

  if (uri == "/api/status") {
    char buf[640];
    bool captureOn = g_captureEnabled.load();
    // 网页推送中：2 秒内收到过帧，且副屏没抢占（副屏优先级最高）
    bool pushingNow = !captureOn && (nowMs() - g_lastWebFrame.load()) < 2000;
    int n = snprintf(buf, sizeof(buf),
                     R"({"open":%s,"pushing":%s,"capture":%s,"captureMode":%d,"dxgi":%s,)"
                     R"("paused":%s,"fps":%d,"frames":%llu,"packets":%llu,"bytes":%llu,"errors":%llu})",
                     g_connected.load() ? "true" : "false",
                     pushingNow ? "true" : "false",
                     captureOn ? "true" : "false",
                     g_captureMode.load(),
                     g_usingDxgi.load() ? "true" : "false",
                     g_capturePaused.load() ? "true" : "false",
                     g_fps.load(),
                     static_cast<unsigned long long>(g_frames.load()),
                     static_cast<unsigned long long>(g_packets.load()),
                     static_cast<unsigned long long>(g_bytes.load()),
                     static_cast<unsigned long long>(g_errors.load()));
    (void)n;
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", buf);
    return;
  }

  if (uri == "/api/connect") {
    bool ok = false;
    std::string err;
    {
      std::lock_guard<std::mutex> lk(g_hidMutex);
      ok = g_hid.open(coolleo::VID, coolleo::PID);
      if (!ok) err = wideToUtf8(g_hid.lastError());
    }
    g_connected = ok;
    if (ok) {
      // 新会话：清掉上一轮遗留的故障计数，界面上的「错误」从 0 开始
      g_hidFailStreak = 0;
      g_errors = 0;
      // 注意：sendCmd 内部会加锁，不能在上面的 lock_guard 作用域内调用
      sendCmd(coolleo::cmd::HAN, 3);
      sendCmd(coolleo::cmd::VER, 8);
      sendCmd(coolleo::cmd::DIS, 8);
    }
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", R"({"ok":%s,"error":"%s"})",
                  ok ? "true" : "false", err.c_str());
    return;
  }

  if (uri == "/api/disconnect") {
    g_captureEnabled = false;  // 断开时自动关掉副屏，避免空转
    g_capturePaused = false;
    g_lastWebFrame = 0;
    {
      std::lock_guard<std::mutex> lk(g_hidMutex);
      g_hid.close();
    }
    g_connected = false;
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", R"({"ok":true})");
    return;
  }

  if (uri == "/api/frame") {
    // 网页渲染好的 JPEG 帧（二进制 body）-> 分帧发 HID
    if (!g_connected.load()) {
      mg_http_reply(c, 400, "Content-Type: application/json\r\n", R"({"ok":false,"error":"未连接"})");
      return;
    }
    // 副屏优先级最高：抓屏期间拒绝主题推送，避免两路数据打架
    if (g_captureEnabled.load()) {
      g_lastWebFrame = 0;  // 清掉推送标记，让内置仪表盘也能接管
      mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"capture_active","reason":"副屏抓屏中，主题推送已暂停"})");
      return;
    }
    size_t blen = hm->body.len;
    g_lastWebFrame = nowMs();  // 标记网页正在推帧
    bool ok = sendFrameToDevice(reinterpret_cast<const uint8_t*>(hm->body.buf), blen);
    if (ok && blen > 512) {
      // 缓存这一帧：关浏览器后继续重发，重启后从磁盘恢复
      {
        std::lock_guard<std::mutex> lk(g_lastFrameMutex);
        g_lastFrame.assign(reinterpret_cast<const uint8_t*>(hm->body.buf),
                           reinterpret_cast<const uint8_t*>(hm->body.buf) + blen);
      }
      uint64_t now2 = nowMs();
      if (now2 - g_lastFrameDiskSave > 5000) {   // 最多 5 秒写一次盘
        g_lastFrameDiskSave = now2;
        std::lock_guard<std::mutex> lk(g_lastFrameMutex);
        saveFrameToDisk(g_lastFrame);
      }
    }
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  R"({"ok":%s,"bodyLen":%d,"err":"%s"})",
                  ok ? "true" : "false", static_cast<int>(blen),
                  wideToUtf8(g_hid.lastError()).c_str());
    return;
  }

  if (uri == "/api/metrics") {
    const auto& m = coolleo::sampleMetrics();
    std::string json = "{";
    bool first = true;
    for (auto& kv : m) {
      if (!first) json += ",";
      first = false;
      json += "\"" + kv.first + "\":" + std::to_string(kv.second);
    }
    json += "}";
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.c_str());
    return;
  }

  if (uri == "/api/displays") {
    auto list = coolleo::ScreenCapture::enumerateDisplays();
    std::string json = "[";
    bool first = true;
    for (const auto& d : list) {
      if (!first) json += ",";
      first = false;
      char item[256];
      snprintf(item, sizeof(item),
               R"({"index":%d,"width":%d,"height":%d,"left":%d,"top":%d,"primary":%s,"name":"%s"})",
               d.index, d.width, d.height, d.left, d.top,
               d.primary ? "true" : "false", jsonEscape(d.name).c_str());
      json += item;
    }
    json += "]";
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.c_str());
    return;
  }


  // ---------------- 主题同步（后端接管渲染） ----------------
  if (uri == "/api/theme/sync") {
    // body: 主题 JSON 原文
    if (body.size() < 32) {
      mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"empty"})");
      return;
    }
    {
      std::lock_guard<std::mutex> lk(g_themeMutex);
      g_themeJson = body;
    }
    g_themeDirty = true;
    g_themeDiskDirty = true;
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  R"({"ok":true,"bytes":%d})", static_cast<int>(body.size()));
    return;
  }

  // 代理下载外部图片/视频（绕过浏览器 CORS）
  if (uri == "/api/fetchurl") {
    char urlbuf[2048] = {0};
    std::string url;
    if (mg_http_get_var(&hm->query, "url", urlbuf, sizeof(urlbuf)) > 0)
      url = urlbuf;   // mg_http_get_var 已做 URL 解码

    if (url.rfind("http", 0) != 0) {
      mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"url must start with http"})");
      return;
    }

    std::wstring wurl(url.begin(), url.end());
    std::vector<char> data;
    std::string ctype;
    if (!winHttpGet(wurl, data, ctype)) {
      mg_http_reply(c, 502, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"download failed"})");
      return;
    }
    if (ctype.empty()) ctype = "application/octet-stream";
    // 二进制必须用 mg_send：mg_http_reply 的格式化输出会在 \0 处截断
    mg_printf(c,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %d\r\n"
              "Cache-Control: no-store\r\n"
              "Access-Control-Allow-Origin: *\r\n\r\n",
              ctype.c_str(), static_cast<int>(data.size()));
    mg_send(c, data.data(), data.size());
    return;
  }

  if (uri == "/api/openui") {
    // 让主线程去激活/打开界面窗口
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 2, 0, 0);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", R"({"ok":true})");
    return;
  }

  if (uri == "/api/mode/backend") {
    bool on = body.find("true") != std::string::npos;
    g_forceBackend = on;
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  R"({"ok":true,"forceBackend":%s})", on ? "true" : "false");
    return;
  }

  if (uri == "/api/theme/get") {
    std::string j;
    {
      std::lock_guard<std::mutex> lk(g_themeMutex);
      j = g_themeJson;
    }
    if (j.empty()) {
      mg_http_reply(c, 404, "Content-Type: application/json\r\n", R"({"ok":false})");
    } else {
      mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", j.c_str());
    }
    return;
  }

  if (uri == "/api/theme/preview") {
    std::string tj;
    {
      std::lock_guard<std::mutex> lk(g_themeMutex);
      tj = g_themeJson;
    }
    if (tj.empty()) {
      mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"no_theme"})");
      return;
    }
    static std::mutex previewMutex;
    std::vector<uint8_t> jpg;
    bool okRender = false;
    {
      std::lock_guard<std::mutex> lk(previewMutex);
      coolleo::ThemeRenderer r;
      if (r.loadFromJson(tj)) okRender = r.render(coolleo::sampleMetrics(), jpg);
    }
    if (!okRender || jpg.empty()) {
      mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"render_failed"})");
      return;
    }
    mg_printf(c,
              "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n"
              "Cache-Control: no-store\r\n\r\n",
              static_cast<int>(jpg.size()));
    mg_send(c, jpg.data(), jpg.size());
    return;
  }

  if (uri == "/api/theme/clear") {
    {
      std::lock_guard<std::mutex> lk(g_themeMutex);
      g_themeJson.clear();
    }
    g_themeDiskDirty = true;   // 顺手把 last-theme.json 删掉
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", R"({"ok":true})");
    return;
  }

  // ---------------- 主题库 ----------------
  if (uri == "/api/themes") {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s",
                  coolleo::ThemeStore::listJson().c_str());
    return;
  }

  if (uri == "/api/themes/save") {
    // body: {"id":"my-theme","name":"我的主题","theme":{...}}
    auto findKey = [&](const char* k) { return body.find(std::string("\"") + k + "\""); };

    // 提取 id（简单扫描，不引 JSON 库）
    std::string id = "theme";
    auto pid = findKey("id");
    if (pid != std::string::npos) {
      auto q1 = body.find('"', pid + 4);
      if (q1 != std::string::npos) {
        auto q2 = body.find('"', q1 + 1);
        if (q2 != std::string::npos) id = body.substr(q1 + 1, q2 - q1 - 1);
      }
    }

    // theme 字段原样存（找到 "theme": 后到最后一个 } 之间）
    std::string payload;
    auto pt = findKey("theme");
    if (pt != std::string::npos) {
      auto brace = body.find('{', pt);
      if (brace != std::string::npos) {
        // 从 brace 开始做括号配对
        int depth = 0;
        bool inStr = false, esc = false;
        size_t end = std::string::npos;
        for (size_t i = brace; i < body.size(); i++) {
          char ch = body[i];
          if (esc) { esc = false; continue; }
          if (ch == '\\') { esc = true; continue; }
          if (ch == '"') { inStr = !inStr; continue; }
          if (inStr) continue;
          if (ch == '{') depth++;
          else if (ch == '}') { depth--; if (depth == 0) { end = i; break; } }
        }
        if (end != std::string::npos) payload = body.substr(brace, end - brace + 1);
      }
    }

    if (payload.empty()) {
      mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"empty_theme"})");
      return;
    }

    bool ok = coolleo::ThemeStore::writeFile(coolleo::ThemeStore::pathFor(id), payload);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  R"({"ok":%s,"id":"%s","bytes":%d})",
                  ok ? "true" : "false", id.c_str(), static_cast<int>(payload.size()));
    return;
  }

  // 主题缩略图：从 JSON 里提取 thumb 字段（data:image/jpeg;base64,...）
  if (uri == "/api/themes/thumb") {
    char idbuf[256] = {0};
    std::string id;
    if (mg_http_get_var(&hm->query, "id", idbuf, sizeof(idbuf)) > 0) id = idbuf;

    std::string content;
    if (id.empty() ||
        !coolleo::ThemeStore::readFile(coolleo::ThemeStore::pathFor(id), content)) {
      mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"not_found"})");
      return;
    }

    // 找 "thumb":"data:image/jpeg;base64,xxxx"
    std::string b64;
    auto k = content.find("\"thumb\"");
    if (k != std::string::npos) {
      auto comma = content.find("base64,", k);
      if (comma != std::string::npos) {
        auto q = content.find('"', comma);
        if (q != std::string::npos) {
          b64 = content.substr(comma + 7, q - comma - 7);
        }
      }
    }
    if (b64.empty()) {
      mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"no_thumb"})");
      return;
    }

    // base64 解码
    static int T[256];
    static bool inited = false;
    if (!inited) {
      for (int i = 0; i < 256; i++) T[i] = -1;
      const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      for (int i = 0; i < 64; i++) T[(unsigned char)a[i]] = i;
      inited = true;
    }
    std::string bin;
    bin.reserve(b64.size() * 3 / 4 + 4);
    int val = 0, bits = 0;
    for (unsigned char ch : b64) {
      if (ch == '=') break;
      int dv = T[ch];
      if (dv < 0) continue;
      val = (val << 6) | dv;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        bin.push_back(static_cast<char>((val >> bits) & 0xFF));
      }
    }

    mg_printf(c,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: image/jpeg\r\n"
              "Content-Length: %d\r\n"
              "Cache-Control: max-age=3600\r\n\r\n",
              static_cast<int>(bin.size()));
    mg_send(c, bin.data(), bin.size());
    return;
  }

  if (uri == "/api/themes/load") {
    auto pid = body.find("\"id\"");
    std::string id;
    if (pid != std::string::npos) {
      auto q1 = body.find('"', pid + 4);
      if (q1 != std::string::npos) {
        auto q2 = body.find('"', q1 + 1);
        if (q2 != std::string::npos) id = body.substr(q1 + 1, q2 - q1 - 1);
      }
    }
    std::string content;
    if (id.empty() || !coolleo::ThemeStore::readFile(coolleo::ThemeStore::pathFor(id), content)) {
      mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                    R"({"ok":false,"error":"not_found"})");
      return;
    }
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", content.c_str());
    return;
  }

  if (uri == "/api/themes/delete") {
    auto pid = body.find("\"id\"");
    std::string id;
    if (pid != std::string::npos) {
      auto q1 = body.find('"', pid + 4);
      if (q1 != std::string::npos) {
        auto q2 = body.find('"', q1 + 1);
        if (q2 != std::string::npos) id = body.substr(q1 + 1, q2 - q1 - 1);
      }
    }
    bool ok = !id.empty() && coolleo::ThemeStore::remove(id);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  R"({"ok":%s,"id":"%s"})", ok ? "true" : "false", id.c_str());
    return;
  }

  if (uri == "/api/windows") {
    auto list = coolleo::WindowCapture::enumerate();
    std::string json = "[";
    bool first = true;
    for (const auto& w : list) {
      if (!first) json += ",";
      first = false;
      std::string t = jsonEscape(w.title);
      char item[1400];
      snprintf(item, sizeof(item),
               R"({"hwnd":%llu,"title":"%s","class":"%s","width":%d,"height":%d})",
               static_cast<unsigned long long>(w.hwnd), t.c_str(), jsonEscape(w.className).c_str(), w.width, w.height);
      json += item;
    }
    json += "]";
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.c_str());
    return;
  }

  if (uri == "/api/capture") {
    // {"enable":true,"mode":0|1,"monitor":0,"hwnd":12345}
    // 区分「没传 enable」和「传了 false」：只改 paused 时不能把副屏关掉
    bool hasEnable = body.find("\"enable\"") != std::string::npos;
    bool enable = body.find("\"enable\":true") != std::string::npos ||
                  body.find("\"enable\": true") != std::string::npos;

    auto readInt = [&](const char* key, long long defVal) -> long long {
      auto p = body.find(std::string("\"") + key + "\"");
      if (p == std::string::npos) return defVal;
      auto q = body.find_first_of("-0123456789", p);
      if (q == std::string::npos) return defVal;
      return atoll(body.c_str() + q);
    };

    long long mode = readInt("mode", g_captureMode.load());
    long long monitor = readInt("monitor", g_captureMonitor.load());
    long long hwnd = readInt("hwnd", static_cast<long long>(g_captureWindow.load()));

    if (mode >= 0 && mode <= 1) g_captureMode = static_cast<int>(mode);
    if (monitor >= 0) g_captureMonitor = static_cast<int>(monitor);
    if (hwnd > 0) g_captureWindow = static_cast<unsigned long long>(hwnd);

    // 开启副屏：抢占推送权，立即让网页主题停推
    if (enable) {
      if (!g_connected.load()) {
        mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                      R"({"ok":false,"error":"not_connected","reason":"请先连接设备"})");
        return;
      }
      g_lastWebFrame = 0;  // 清标记 -> 网页推送立即失效
    }

    // 暂停/恢复：{"paused":true|false}
    if (body.find("\"paused\"") != std::string::npos) {
      bool paused = body.find("\"paused\":true") != std::string::npos ||
                    body.find("\"paused\": true") != std::string::npos;
      g_capturePaused = paused;
    }
    if (hasEnable && enable) g_capturePaused = false;  // 开启即清除暂停

    if (hasEnable) g_captureEnabled = enable;  // 只有显式传 enable 才改开关

    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  R"({"ok":true,"enable":%s,"mode":%d,"monitor":%d,"hwnd":%llu})",
                  enable ? "true" : "false", g_captureMode.load(), g_captureMonitor.load(),
                  static_cast<unsigned long long>(g_captureWindow.load()));
    return;
  }

  if (uri == "/api/fps") {
    int v = 20;
    auto p = body.find("\"fps\"");
    if (p != std::string::npos) {
      auto c2 = body.find_first_of("0123456789", p);
      if (c2 != std::string::npos) {
        v = atoi(body.c_str() + c2);
        if (v < 1) v = 1;
        if (v > 30) v = 30;
      }
    }
    g_fps = v;
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", R"({"ok":true,"fps":%d})", v);
    return;
  }

  mg_http_reply(c, 404, "Content-Type: application/json\r\n", R"({"error":"unknown"})");
}

void evHandler(struct mg_connection* c, int ev, void* evData) {
  static const std::string uiRoot = [] {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe(buf);
    auto pos = exe.find_last_of("\\/");
    std::string dir = pos == std::string::npos ? "." : exe.substr(0, pos);
    return dir + "\\ui";
  }();

  if (ev == MG_EV_HTTP_MSG) {
    auto* hm = static_cast<struct mg_http_message*>(evData);
    std::string uri(hm->uri.buf, hm->uri.len);
    if (uri.rfind("/api/", 0) == 0) {
      handleApi(c, hm);
    } else if (uri == "/ws") {
      mg_ws_upgrade(c, hm, nullptr);
    } else {
      serveStatic(c, hm, uiRoot);
    }
  } else if (ev == MG_EV_WS_OPEN) {
    std::lock_guard<std::mutex> lk(g_wsMutex);
    g_wsClients.push_back({c});
  } else if (ev == MG_EV_WS_MSG) {
    auto* wm = static_cast<struct mg_ws_message*>(evData);
    std::string msg(wm->data.buf, wm->data.len);
    // 二进制帧：直接推设备
    if (wm->flags & 0x2) {  // binary
      if (g_connected.load()) {
        sendFrameToDevice(reinterpret_cast<const uint8_t*>(wm->data.buf), wm->data.len);
      }
      return;
    }
    // 文本：简单命令
    if (msg == "connect") {
      bool ok = false;
      {
        std::lock_guard<std::mutex> lk(g_hidMutex);
        ok = g_hid.open(coolleo::VID, coolleo::PID);
      }
      g_connected = ok;
      if (ok) {
        sendCmd(coolleo::cmd::HAN, 3);
        sendCmd(coolleo::cmd::VER, 8);
        sendCmd(coolleo::cmd::DIS, 8);
      }
    } else if (msg == "disconnect") {
      std::lock_guard<std::mutex> lk(g_hidMutex);
      g_hid.close();
      g_connected = false;
    }
  } else if (ev == MG_EV_CLOSE) {
    std::lock_guard<std::mutex> lk(g_wsMutex);
    for (auto it = g_wsClients.begin(); it != g_wsClients.end();) {
      if (it->c == c) it = g_wsClients.erase(it);
      else ++it;
    }
  }
}

// ---------------------------------------------------------------- 内置仪表盘推送线程
// 网页没打开时（比如开机自启），仍然让屏幕显示监控仪表盘
void builtinLoop() {
  coolleo::DashboardRenderer dash;
  if (!dash.init()) return;

  std::vector<uint8_t> jpeg;
  auto nextTick = std::chrono::steady_clock::now();

  while (g_running.load()) {
    // 网页接管了推送，或副屏在抓屏，就暂停内置仪表盘
    bool webActive = !g_forceBackend.load() && (nowMs() - g_lastWebFrame.load()) < 2000;
    bool capActive = g_captureEnabled.load();
    if (!g_builtinEnabled.load() || !g_connected.load() || webActive || capActive) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      nextTick = std::chrono::steady_clock::now();
      continue;
    }

    // ---- 有活动主题 -> 后端自己渲染（数据实时更新）----
    std::string themeJson;
    {
      std::lock_guard<std::mutex> lk(g_themeMutex);
      themeJson = g_themeJson;
    }

    static coolleo::ThemeRenderer renderer;
    static std::string loadedJson;
    if (!themeJson.empty() && themeJson != loadedJson) {
      if (renderer.loadFromJson(themeJson)) {
        loadedJson = themeJson;
        printf("[theme] theme loaded, backend rendering\n");
      } else {
        loadedJson = themeJson;   // 解析失败也不重试
      }
    }

    if (renderer.loaded() && !themeJson.empty()) {
      jpeg.clear();
      if (renderer.render(coolleo::sampleMetrics(), jpeg) && !jpeg.empty()) {
        sendFrameToDevice(jpeg.data(), jpeg.size());
      }
      int tfps = renderer.fps();
      if (tfps < 1) tfps = 1;
      if (tfps > 30) tfps = 30;
      nextTick += std::chrono::microseconds(1000000 / tfps);
      auto now2 = std::chrono::steady_clock::now();
      if (nextTick > now2) std::this_thread::sleep_for(nextTick - now2);
      else nextTick = now2;
      continue;
    }

    // ---- 没有主题但有缓存帧 -> 重发（静态保持）----
    std::vector<uint8_t> cached;
    {
      std::lock_guard<std::mutex> lk(g_lastFrameMutex);
      cached = g_lastFrame;
    }
    if (!cached.empty()) {
      sendFrameToDevice(cached.data(), cached.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));  // 静止画面 1fps 足够
      nextTick = std::chrono::steady_clock::now();
      continue;
    }

    const auto& m = coolleo::sampleMetrics();
    auto getv = [&](const char* k) -> double {
      auto it = m.find(k);
      return it == m.end() ? 0.0 : it->second;
    };

    jpeg.clear();
    if (dash.render(getv("cpu.load"), getv("mem.usedPercent"), getv("disk.usedPercent"),
                    L"COOLLEO STUDIO", L"auto", jpeg) &&
        !jpeg.empty()) {
      sendFrameToDevice(jpeg.data(), jpeg.size());
    }

    int fps = g_fps.load();
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    nextTick += std::chrono::microseconds(1000000 / fps);
    auto now = std::chrono::steady_clock::now();
    if (nextTick > now) {
      std::this_thread::sleep_for(nextTick - now);
    } else {
      nextTick = now;
    }
  }
}

// ---------------------------------------------------------------- 副屏：屏幕捕获线程
void captureLoop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

  coolleo::ScreenCapture cap;
  coolleo::WindowCapture winCap;

  // 懒创建：不开副屏就不建 D3D 设备（省 20~30MB 常驻内存）
  bool capReady = false;
  int curMonitor = -1;
  std::vector<uint8_t> jpeg;
  auto nextTick = std::chrono::steady_clock::now();

  while (g_running.load()) {
    if (!g_captureEnabled.load() || g_capturePaused.load() || !g_connected.load()) {
      // 停用/暂停时释放 D3D 会话，把显存和驱动内存还给系统
      if (capReady) {
        cap.releaseSession();
        capReady = false;
        curMonitor = -1;
        g_usingDxgi = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      nextTick = std::chrono::steady_clock::now();
      continue;
    }

    jpeg.clear();
    bool got = false;
    int mode = g_captureMode.load();

    if (mode == 1) {
      // 窗口捕获（不需要 D3D）
      if (capReady) {
        cap.releaseSession();
        capReady = false;
        curMonitor = -1;
      }
      unsigned long long hwnd = g_captureWindow.load();
      if (hwnd && coolleo::WindowCapture::isValid(hwnd)) {
        got = winCap.grabJpeg(hwnd, coolleo::SCREEN_WIDTH, coolleo::SCREEN_HEIGHT, jpeg);
      }
    } else {
      // 显示器捕获（DXGI）：首次使用或切换显示器时才建会话
      int mon = g_captureMonitor.load();
      if (!capReady || mon != curMonitor) {
        if (capReady) cap.releaseSession();
        cap.init(mon);
        capReady = true;
        curMonitor = mon;
      }
      g_usingDxgi = cap.usingDxgi();
      got = cap.grabJpeg(coolleo::SCREEN_WIDTH, coolleo::SCREEN_HEIGHT, jpeg);
    }

    if (got && !jpeg.empty()) {
      sendFrameToDevice(jpeg.data(), jpeg.size());
    }

    int fps = g_fps.load();
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    nextTick += std::chrono::microseconds(1000000 / fps);
    auto now = std::chrono::steady_clock::now();
    if (nextTick > now) {
      std::this_thread::sleep_for(nextTick - now);
    } else {
      nextTick = now;
    }
  }
}

// ---------------------------------------------------------------- 指标广播线程
void metricsLoop() {
  while (g_running.load()) {
    const auto& m = coolleo::sampleMetrics();
    std::string json = "{\"type\":\"metrics\",\"data\":{";
    bool first = true;
    for (auto& kv : m) {
      if (!first) json += ",";
      first = false;
      json += "\"" + kv.first + "\":" + std::to_string(kv.second);
    }
    json += "}}";
    wsBroadcast(json);

    // 主题变了就落盘（1 秒最多一次，避免编辑时疯狂写文件）
    if (g_themeDiskDirty.exchange(false)) {
      std::string tj;
      {
        std::lock_guard<std::mutex> lk(g_themeMutex);
        tj = g_themeJson;
      }
      saveThemeToDisk(tj);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}

// ---------------------------------------------------------------- 窗口过程
LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_APP_TRAY:
      // 左右键都只弹菜单：单击直接开浏览器太容易误触，一开就是一串标签页
      if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP ||
          LOWORD(lp) == WM_LBUTTONDBLCLK) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"打开界面");
        AppendMenuW(menu, MF_STRING, 2, L"副屏：开/关");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"退出");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        int sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (sel == 1) openBrowser();
        else if (sel == 2) g_captureEnabled = !g_captureEnabled.load();
        else if (sel == 3) PostQuitMessage(0);
      }
      return 0;

    case WM_APP + 2:   // 来自 /api/openui 的「打开或激活界面」请求
      openBrowser();
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_COMMAND:
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  g_hinst = hInst;

  // 开机自启时带 --startup：静默运行，不弹浏览器
  bool silentStart = false;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
      for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--startup") == 0) silentStart = true;
      }
      LocalFree(argv);
    }
  }

  // 单实例
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"CoolleoStudio_Singleton");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // 已有实例：让它去激活界面窗口（不要自己开新标签）
    if (!silentStart) {
      std::wstring req = L"http://127.0.0.1:" + std::to_wstring(PORT) + L"/api/openui";
      // 用 PowerShell 发一个极简 POST（避免引入 HTTP 客户端）
      std::wstring cmd = L"powershell -NoProfile -WindowStyle Hidden -Command "
                         L"\"try{Invoke-WebRequest -Uri '" + req +
                         L"' -Method POST -UseBasicParsing -TimeoutSec 3 | Out-Null}catch{}\"";
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi{};
      std::wstring mutableCmd = cmd;
      if (CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
      }
    }
    CloseHandle(mutex);
    return 0;
  }

  // 注册开机自启（每次启动都校正一遍路径，防止移动后失效）
  if (!silentStart) {
    enableAutoStart(true);
  }

  // 隐藏窗口承载托盘
  WNDCLASSW wc{};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"CoolleoStudioTray";
  RegisterClassW(&wc);
  g_hwnd = CreateWindowW(wc.lpszClassName, L"CoolleoStudio", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
  if (!g_hwnd) return 1;

  addTrayIcon();

  // 启动 HTTP 服务
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  char addr[64];
  snprintf(addr, sizeof(addr), "http://127.0.0.1:%d", PORT);
  auto* c = mg_http_listen(&mgr, addr, evHandler, nullptr);
  if (!c) {
    removeTrayIcon();
    mg_mgr_free(&mgr);
    return 1;
  }

  // ---- 启动即自动连接设备 ----
  {
    std::lock_guard<std::mutex> lk(g_hidMutex);
    g_connected = g_hid.open(coolleo::VID, coolleo::PID);
  }
  if (g_connected.load()) {
    sendCmd(coolleo::cmd::HAN, 3);
    sendCmd(coolleo::cmd::VER, 8);
    sendCmd(coolleo::cmd::DIS, 8);
  }

  loadThemeFromDisk();   // 恢复上次使用的主题（数据实时刷新）
  loadFrameFromDisk();   // 恢复上次的静态画面（主题文件缺失时兜底）

  std::thread capThread(captureLoop);
  std::thread metricThread(metricsLoop);
  std::thread builtinThread(builtinLoop);

  // 只有手动启动才打开浏览器；开机自启静默运行
  if (!silentStart) openBrowser();

  // 消息循环 + mongoose poll
  MSG msg;
  while (g_running.load()) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g_running = false;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    mg_mgr_poll(&mgr, 5);
  }

  capThread.join();
  metricThread.join();
  builtinThread.join();

  g_hid.close();
  coolleo::GdiPlusScope::shutdownAll();
  removeTrayIcon();
  mg_mgr_free(&mgr);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}
