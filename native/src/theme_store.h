/**
 * Theme storage - save / list / load / delete themes on disk.
 *
 * Themes live next to the exe in ./themes/*.json so the whole app stays
 * portable (copy the folder, everything comes with it).
 */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace coolleo {

struct ThemeEntry {
  std::string id;       ///< 文件名（不含扩展名）
  std::string name;     ///< 显示名（存在 JSON 的 name 字段）
  long long mtime = 0;  ///< 修改时间（秒）
  long long size = 0;   ///< 字节数
};

class ThemeStore {
public:
  /** 主题目录：exe 同级 /themes */
  static std::string dir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe(buf);
    auto pos = exe.find_last_of("\\/");
    std::string base = (pos == std::string::npos) ? "." : exe.substr(0, pos);
    std::string d = base + "\\themes";
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
  }

  static std::string pathFor(const std::string& id) {
    return dir() + "\\" + sanitize(id) + ".json";
  }

  /** 只允许字母数字下划线横线，防止路径穿越 */
  static std::string sanitize(const std::string& id) {
    std::string o;
    o.reserve(id.size());
    for (char c : id) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-') {
        o += c;
      } else {
        o += '_';
      }
    }
    if (o.empty()) o = "theme";
    if (o.size() > 64) o = o.substr(0, 64);
    return o;
  }

  static bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return n == data.size();
  }

  static bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return false; }
    out.resize(static_cast<size_t>(len));
    size_t rd = len > 0 ? fread(&out[0], 1, static_cast<size_t>(len), f) : 0;
    out.resize(rd);
    fclose(f);
    return true;
  }

  static bool remove(const std::string& id) {
    return DeleteFileA(pathFor(id).c_str()) != 0;
  }

  /** 从主题 JSON 里粗提 name 字段（不引 JSON 库，够用） */
  static std::string extractName(const std::string& json) {
    auto p = json.find("\"name\"");
    if (p == std::string::npos) return "";
    p = json.find('"', p + 6);
    if (p == std::string::npos) return "";
    auto e = json.find('"', p + 1);
    if (e == std::string::npos) return "";
    return json.substr(p + 1, e - p - 1);
  }

  static std::vector<ThemeEntry> list() {
    std::vector<ThemeEntry> out;
    const std::string d = dir();
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((d + "\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      std::string fn = fd.cFileName;
      auto dot = fn.find_last_of('.');
      std::string id = (dot == std::string::npos) ? fn : fn.substr(0, dot);

      ThemeEntry e;
      e.id = id;
      e.size = (static_cast<long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;

      ULARGE_INTEGER t{};
      t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
      t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
      // FILETIME(1601) -> unix 秒
      e.mtime = static_cast<long long>(t.QuadPart / 10000000ULL - 11644473600ULL);

      std::string content;
      if (readFile(d + "\\" + fn, content)) {
        e.name = extractName(content);
        if (e.name.empty()) e.name = id;
      } else {
        e.name = id;
      }
      out.push_back(e);
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return out;
  }

  /** 列出所有主题的完整 JSON（一次性给前端做主题库） */
  static std::string listJson() {
    auto items = list();
    std::string json = "[";
    bool first = true;
    for (const auto& e : items) {
      if (!first) json += ",";
      first = false;
      char buf[1024];
      // 从主题文件头部提取 category（不存在则为空）
      std::string cat;
      {
        FILE* cf = fopen(pathFor(e.id).c_str(), "rb");
        if (cf) {
          fseek(cf, 0, SEEK_END);
          long flen = ftell(cf);
          fseek(cf, 0, SEEK_SET);
          if (flen < 0) flen = 0;
          if (flen > 2 * 1024 * 1024) flen = 2 * 1024 * 1024;   // 最多读 2MB
          std::string head;
          head.resize(static_cast<size_t>(flen));
          size_t n = flen > 0 ? fread(&head[0], 1, static_cast<size_t>(flen), cf) : 0;
          fclose(cf);
          head.resize(n);
          auto k = head.find("\"category\"");
          if (k != std::string::npos) {
            auto c1 = head.find('"', k + 10);
            auto c2 = (c1 == std::string::npos) ? std::string::npos : head.find('"', c1 + 1);
            if (c1 != std::string::npos && c2 != std::string::npos)
              cat = head.substr(c1 + 1, c2 - c1 - 1);
          }
        }
      }
      snprintf(buf, sizeof(buf),
               R"({"id":"%s","name":"%s","mtime":%lld,"size":%lld,"category":"%s"})",
               e.id.c_str(), jsonEscapeMini(e.name).c_str(), e.mtime, e.size,
               jsonEscapeMini(cat).c_str());
      json += buf;
    }
    json += "]";
    return json;
  }

  static std::string jsonEscapeMini(const std::string& s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\') o += '\\';
      if (static_cast<unsigned char>(c) < 0x20) { o += ' '; continue; }
      o += c;
    }
    return o;
  }
};

}  // namespace coolleo
