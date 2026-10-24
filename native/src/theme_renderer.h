/**
 * Theme renderer (C++ side) - renders a theme JSON into a JPEG frame.
 *
 * Why: the browser is only an editor. Once a theme is saved, the backend must
 * keep rendering it (live metrics) even when the browser is closed. This class
 * parses the same theme JSON the web editor produces and draws it with GDI+.
 *
 * Supported widgets: panel / text / bar / arcGauge / clock / sparkline /
 *                    icon / divider / image(data-url) / bgImage(data-url)
 * Video is editor-only (not rendered headlessly).
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "gdi_capture.h"
#include "gdiplus_scope.h"
#include "protocol.h"

#pragma comment(lib, "gdiplus.lib")

namespace coolleo {

class ThemeRenderer {
 public:
  using MetricMap = std::map<std::string, double>;

  bool loadFromJson(const std::string& text) {
    try {
      auto j = nlohmann::json::parse(text);
      if (!j.contains("canvas")) return false;

      canvasW_ = j["canvas"].value("width", SCREEN_WIDTH);
      canvasH_ = j["canvas"].value("height", SCREEN_HEIGHT);
      background_ = j["canvas"].value("background", std::string("#000000"));
      fps_ = j["canvas"].value("fps", 20);

      widgets_.clear();
      if (j.contains("widgets") && j["widgets"].is_array()) {
        for (const auto& w : j["widgets"]) {
          Widget wd;
          wd.type = w.value("type", std::string(""));
          wd.z = w.value("z", 0);
          if (w.contains("rect")) {
            wd.x = numOrPct(w["rect"], "x", canvasW_);
            wd.y = numOrPct(w["rect"], "y", canvasH_);
            wd.w = numOrPct(w["rect"], "w", canvasW_);
            wd.h = numOrPct(w["rect"], "h", canvasH_);
          }
          if (w.contains("props")) wd.props = w["props"];
          if (w.contains("bindings")) wd.bindings = w["bindings"];
          wd.hidden = w.value("hidden", false);
          wd.visibleWhen = w.value("visibleWhen", std::string(""));
          widgets_.push_back(std::move(wd));
        }
      }
      std::stable_sort(widgets_.begin(), widgets_.end(),
                       [](const Widget& a, const Widget& b) { return a.z < b.z; });
      loaded_ = true;
      return true;
    } catch (...) {
      return false;
    }
  }

  bool loaded() const { return loaded_; }
  int fps() const { return fps_; }
  int width() const { return canvasW_; }
  int height() const { return canvasH_; }

  /** 用给定指标渲染一帧（含旋转），输出 JPEG */
  bool render(const MetricMap& metrics, std::vector<uint8_t>& out) {
    if (!loaded_) return false;
    if (!GdiPlusScope::ensure()) return false;

    Gdiplus::Bitmap bmp(canvasW_, canvasH_, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    Gdiplus::SolidBrush bg(colorOf(background_));
    g.FillRectangle(&bg, 0, 0, canvasW_, canvasH_);

    for (const auto& w : widgets_) {
      if (w.hidden) continue;
      if (!w.visibleWhen.empty() && !condOk(w.visibleWhen, metrics)) continue;
      drawWidget(g, w, metrics);
    }

    HBITMAP hbmp = nullptr;
    if (bmp.GetHBITMAP(Gdiplus::Color(255, 0, 0, 0), &hbmp) != Gdiplus::Ok || !hbmp) return false;
    bool ok = encoder_.encodeHBitmap(hbmp, out);
    DeleteObject(hbmp);
    GdiPlusScope::release();
    return ok;
  }

 private:
  struct Widget {
    std::string type;
    int z = 0;
    double x = 0, y = 0, w = 0, h = 0;
    bool hidden = false;
    std::string visibleWhen;
    nlohmann::json props;
    nlohmann::json bindings;
  };

  // ---------------------------------------------------------------- helpers
  static double numOrPct(const nlohmann::json& r, const char* key, int base) {
    if (!r.contains(key)) return 0;
    const auto& v = r[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
      std::string s = v.get<std::string>();
      if (!s.empty() && s.back() == '%') {
        try {
          return std::stod(s.substr(0, s.size() - 1)) / 100.0 * base;
        } catch (...) {
          return 0;
        }
      }
      try {
        return std::stod(s);
      } catch (...) {
        return 0;
      }
    }
    return 0;
  }

  static BYTE hex2(const std::string& s, size_t i) {
    auto f = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return 0;
    };
    if (i + 1 >= s.size()) return 0;
    return static_cast<BYTE>(f(s[i]) * 16 + f(s[i + 1]));
  }

  static Gdiplus::Color colorOf(const std::string& hex, BYTE alpha = 255) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() >= 6) {
      return Gdiplus::Color(alpha, hex2(s, 0), hex2(s, 2), hex2(s, 4));
    }
    return Gdiplus::Color(alpha, 128, 128, 128);
  }

  /** 取绑定值：bindings.value = "cpu.load" */
  static double bindValue(const Widget& w, const MetricMap& m, const char* alias) {
    if (w.bindings.is_object() && w.bindings.contains(alias)) {
      std::string key = w.bindings[alias].get<std::string>();
      auto it = m.find(key);
      if (it != m.end()) return it->second;
    }
    return std::nan("");
  }

  static double propNum(const Widget& w, const char* k, double def) {
    if (w.props.is_object() && w.props.contains(k) && w.props[k].is_number())
      return w.props[k].get<double>();
    return def;
  }
  static std::string propStr(const Widget& w, const char* k, const std::string& def) {
    if (w.props.is_object() && w.props.contains(k) && w.props[k].is_string())
      return w.props[k].get<std::string>();
    return def;
  }

  /** UTF-8 -> UTF-16（以前用 byte-wise 拷贝，中文会变乱码） */
  static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
    return out;
  }

  static std::string trimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
  }

  /** 管道函数：key | round(1) | clamp(0,100) */
  static double applyPipes(double v, const std::vector<std::string>& pipes) {
    for (const auto& raw : pipes) {
      std::string s = trimStr(raw);
      if (s.empty()) continue;
      std::string fn = s;
      std::vector<double> args;
      size_t lp = s.find('(');
      if (lp != std::string::npos) {
        fn = trimStr(s.substr(0, lp));
        size_t rp = s.find(')', lp);
        std::string inside = (rp == std::string::npos) ? s.substr(lp + 1)
                                                       : s.substr(lp + 1, rp - lp - 1);
        size_t q = 0;
        while (q <= inside.size()) {
          size_t c2 = inside.find(',', q);
          std::string tok = (c2 == std::string::npos) ? inside.substr(q)
                                                      : inside.substr(q, c2 - q);
          tok = trimStr(tok);
          if (!tok.empty()) {
            try { args.push_back(std::stod(tok)); } catch (...) {}
          }
          if (c2 == std::string::npos) break;
          q = c2 + 1;
        }
      }
      std::string lf = fn;
      for (auto& c : lf) c = static_cast<char>(::tolower(c));
      double a0 = args.empty() ? 0.0 : args[0];
      if (lf == "round") {
        double p = std::pow(10.0, args.empty() ? 0.0 : args[0]);
        v = std::round(v * p) / p;
      } else if (lf == "floor") {
        v = std::floor(v);
      } else if (lf == "ceil") {
        v = std::ceil(v);
      } else if (lf == "abs") {
        v = std::fabs(v);
      } else if (lf == "clamp") {
        double lo = args.size() > 0 ? args[0] : 0.0;
        double hi = args.size() > 1 ? args[1] : 100.0;
        v = (std::max)(lo, (std::min)(hi, v));
      } else if (lf == "min") {
        v = (std::min)(v, a0);
      } else if (lf == "max") {
        v = (std::max)(v, a0);
      } else if (lf == "mul") {
        v = v * (args.empty() ? 1.0 : args[0]);
      } else if (lf == "div") {
        double d2 = args.empty() ? 1.0 : args[0];
        if (d2 != 0.0) v = v / d2;
      }
    }
    return v;
  }

  /** 显示条件：cpu.temp>80 && gpu.load<50（留空=始终显示；写错当无条件，别把控件弄没） */
  static bool condOk(const std::string& expr, const MetricMap& m) {
    std::string s = trimStr(expr);
    if (s.empty()) return true;
    size_t pos = 0;
    while (pos <= s.size()) {
      size_t andp = s.find("&&", pos);
      std::string part = (andp == std::string::npos) ? s.substr(pos)
                                                     : s.substr(pos, andp - pos);
      pos = (andp == std::string::npos) ? s.size() + 1 : andp + 2;

      std::string t = trimStr(part);
      if (t.empty()) continue;
      // 找比较符
      static const char* kOps[] = {">=", "<=", "==", "!=", ">", "<", nullptr};
      size_t opos = std::string::npos;
      std::string op;
      for (int i = 0; kOps[i]; ++i) {
        size_t f = t.find(kOps[i]);
        if (f != std::string::npos && (opos == std::string::npos || f < opos)) {
          opos = f;
          op = kOps[i];
        }
      }
      if (opos == std::string::npos) continue;   // 没比较符 -> 忽略
      std::string key = trimStr(t.substr(0, opos));
      std::string rhs = trimStr(t.substr(opos + op.size()));
      double target = 0;
      try { target = std::stod(rhs); } catch (...) { continue; }
      auto it = m.find(key);
      if (it == m.end()) continue;               // 指标不存在 -> 忽略
      double v = it->second;
      bool ok = true;
      if (op == ">") ok = v > target;
      else if (op == ">=") ok = v >= target;
      else if (op == "<") ok = v < target;
      else if (op == "<=") ok = v <= target;
      else if (op == "==") ok = v == target;
      else if (op == "!=") ok = v != target;
      if (!ok) return false;
    }
    return true;
  }

  /** 小数位：props.decimals 可能是数字或字符串；缺省 -1 表示用默认值 */
  static int propDecimals(const Widget& w) {
    if (!w.props.is_object() || !w.props.contains("decimals")) return -1;
    const auto& d = w.props["decimals"];
    if (d.is_number()) return static_cast<int>(d.get<double>());
    if (d.is_string()) {
      std::string ds = d.get<std::string>();
      if (!ds.empty()) { try { return std::stoi(ds); } catch (...) {} }
    }
    return -1;
  }

  /** 阈值配色：返回空串表示不覆盖 */
  static std::string thColorOf(const Widget& w, double v) {
    if (std::isnan(v)) return std::string();
    double dv = propNum(w, "thDanger", -1);
    double wv = propNum(w, "thWarn", -1);
    std::string dc = propStr(w, "thDangerColor", "");
    std::string wc = propStr(w, "thWarnColor", "");
    if (!dc.empty() && dv > 0 && v >= dv) return dc;
    if (!wc.empty() && wv > 0 && v >= wv) return wc;
    return std::string();
  }

  /** 按指标键取值：先查绑定别名，再直接查指标表 */
  static bool metricOf(const Widget& w, const MetricMap& m,
                       const std::string& key, double& out) {
    if (w.bindings.is_object() && w.bindings.contains(key) && w.bindings[key].is_string()) {
      std::string mk = w.bindings[key].get<std::string>();
      auto it = m.find(mk);
      if (it != m.end()) { out = it->second; return true; }
    }
    auto it = m.find(key);
    if (it != m.end()) { out = it->second; return true; }
    return false;
  }

  /** CSS 通用字族名 -> Windows 实际字体，并校验字体存在（GDI+ 找不到时字体构造会失败，文字就画不出来） */
  static std::wstring resolveFamily(const std::string& fam) {
    std::string s = fam;
    // 只取第一个字族："Segoe UI", sans-serif -> Segoe UI
    size_t comma = s.find(',');
    if (comma != std::string::npos) s = s.substr(0, comma);
    size_t b = s.find_first_not_of(" \t\"'");
    if (b == std::string::npos) {
      s.clear();
    } else {
      size_t e = s.find_last_not_of(" \t\"'");
      s = s.substr(b, e - b + 1);
    }

    std::string l = s;
    for (auto& c : l) c = static_cast<char>(::tolower(c));

    std::string mapped;
    if (l.empty() || l == "sans-serif" || l == "system-ui" || l == "-apple-system" ||
        l == "ui-sans-serif")
      mapped = "Microsoft YaHei";
    else if (l == "monospace" || l == "ui-monospace" || l == "courier new")
      mapped = "Consolas";
    else if (l == "serif")
      mapped = "Times New Roman";
    else
      mapped = s;

    std::wstring w = widen(mapped);
    Gdiplus::FontFamily probe(w.c_str());
    if (probe.GetLastStatus() == Gdiplus::Ok) return w;

    Gdiplus::FontFamily fb(L"Microsoft YaHei");
    if (fb.GetLastStatus() == Gdiplus::Ok) return std::wstring(L"Microsoft YaHei");
    return std::wstring(L"Arial");
  }

  static Gdiplus::FontStyle parseStyle(const std::string& fontSpec, float& sizeOut,
                                       std::string& familyOut) {
    // "bold 18px Microsoft YaHei" -> bold, 18, sans-serif
    Gdiplus::FontStyle style = Gdiplus::FontStyleRegular;
    std::string s = fontSpec;
    sizeOut = 16.f;
    familyOut = "Microsoft YaHei";

    auto lower = [](std::string x) {
      for (auto& c : x) c = static_cast<char>(::tolower(c));
      return x;
    };
    std::string ls = lower(s);
    if (ls.find("bold") != std::string::npos)
      style = static_cast<Gdiplus::FontStyle>(style | Gdiplus::FontStyleBold);
    if (ls.find("italic") != std::string::npos)
      style = static_cast<Gdiplus::FontStyle>(style | Gdiplus::FontStyleItalic);

    auto px = s.find("px");
    if (px != std::string::npos) {
      auto sp = s.rfind(' ', px);
      std::string numStr = (sp == std::string::npos) ? s.substr(0, px) : s.substr(sp + 1, px - sp - 1);
      try { sizeOut = static_cast<float>(std::stod(numStr)); } catch (...) {}
      // px 之后是字族名，可能带引号/逗号/中日韩字符
      std::string famPart = s.substr(px + 2);
      size_t b = famPart.find_first_not_of(" \t,\"'");
      if (b != std::string::npos) {
        famPart = famPart.substr(b);
        size_t e = famPart.find_last_not_of(" \t,\"'");
        if (e != std::string::npos) famPart = famPart.substr(0, e + 1);
        if (!famPart.empty()) familyOut = famPart;
      }
    }
    return style;
  }

  // ---------------------------------------------------------------- 各控件
  void drawWidget(Gdiplus::Graphics& g, const Widget& w, const MetricMap& m) {
    const int X = static_cast<int>(std::lround(w.x));
    const int Y = static_cast<int>(std::lround(w.y));
    const int W = static_cast<int>(std::lround(w.w));
    const int H = static_cast<int>(std::lround(w.h));

    if (w.type == "panel") {
      int radius = static_cast<int>(propNum(w, "radius", 8));
      fillRound(g, X, Y, W, H, radius, colorOf(propStr(w, "color", "#18181b")));
    } else if (w.type == "text") {
      drawText(g, w, m, X, Y, W, H);
    } else if (w.type == "bar") {
      drawBar(g, w, m, X, Y, W, H);
    } else if (w.type == "arcGauge") {
      drawArc(g, w, m, X, Y, W, H);
    } else if (w.type == "clock") {
      drawClock(g, w, X, Y, W, H);
    } else if (w.type == "sparkline") {
      drawSpark(g, w, m, X, Y, W, H);
    } else if (w.type == "icon") {
      drawIcon(g, w, X, Y, W, H);
    } else if (w.type == "divider") {
      drawDivider(g, w, X, Y, W, H);
    } else if (w.type == "image" || w.type == "bgImage") {
      drawImage(g, w, X, Y, W, H);
    }
  }

  static void fillRound(Gdiplus::Graphics& g, int x, int y, int w, int h, int r, Gdiplus::Color c) {
    if (r <= 0) {
      Gdiplus::SolidBrush b(c);
      g.FillRectangle(&b, x, y, w, h);
      return;
    }
    int rr = (std::min)(r, (std::min)(w, h) / 2);
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, rr * 2, rr * 2, 180, 90);
    path.AddArc(x + w - rr * 2, y, rr * 2, rr * 2, 270, 90);
    path.AddArc(x + w - rr * 2, y + h - rr * 2, rr * 2, rr * 2, 0, 90);
    path.AddArc(x, y + h - rr * 2, rr * 2, rr * 2, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &path);
  }

  void drawText(Gdiplus::Graphics& g, const Widget& w, const MetricMap& m, int x, int y, int W, int H) {
    std::string tpl = propStr(w, "template", "");
    if (tpl.empty()) return;

    int decimals = propDecimals(w);
    if (decimals > 4) decimals = 4;
    const std::string suffix = propStr(w, "suffix", "");
    const std::string noValue = propStr(w, "noValue", "-");

    // 模板替换：${value}（走绑定）或 ${cpu.load}（直接写指标键）
    std::string out;
    size_t pos = 0;
    while (pos < tpl.size()) {
      auto p = tpl.find("${", pos);
      if (p == std::string::npos) { out += tpl.substr(pos); break; }
      out += tpl.substr(pos, p - pos);
      auto e = tpl.find('}', p);
      if (e == std::string::npos) { out += tpl.substr(p); break; }
      std::string expr = tpl.substr(p + 2, e - p - 2);
      std::string key = expr;
      std::vector<std::string> pipes;
      {
        size_t bp = expr.find('|');
        if (bp != std::string::npos) {
          key = expr.substr(0, bp);
          std::string rest = expr.substr(bp + 1);
          size_t q = 0;
          while (q <= rest.size()) {
            size_t nx = rest.find('|', q);
            if (nx == std::string::npos) { pipes.push_back(rest.substr(q)); break; }
            pipes.push_back(rest.substr(q, nx - q));
            q = nx + 1;
          }
        }
        key = trimStr(key);
      }
      double v = 0;
      if (metricOf(w, m, key, v)) {
        v = applyPipes(v, pipes);
        char buf[64];
        int d = (decimals < 0) ? 1 : decimals;
        snprintf(buf, sizeof(buf), "%.*f", d, v);
        out += buf;
        out += suffix;
      } else {
        out += noValue;
      }
      pos = e + 1;
    }

    float fs; std::string fam;
    auto style = parseStyle(propStr(w, "font", "16px Microsoft YaHei"), fs, fam);
    Gdiplus::FontFamily ff(resolveFamily(fam).c_str());
    Gdiplus::Font font(&ff, fs, style, Gdiplus::UnitPixel);

    std::string align = propStr(w, "align", "left");
    Gdiplus::StringFormat sf;
    // 关键：禁止自动换行，否则宽度差几像素就把文字折成两行
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetTrimming(Gdiplus::StringTrimmingNone);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    int tx = x;
    if (align == "center") { sf.SetAlignment(Gdiplus::StringAlignmentCenter); tx = x + W / 2; }
    else if (align == "right") { sf.SetAlignment(Gdiplus::StringAlignmentFar); tx = x + W; }
    else sf.SetAlignment(Gdiplus::StringAlignmentNear);

    // 阈值配色：取绑定值判断
    std::string textColor = propStr(w, "color", "#ffffff");
    {
      double tv = bindValue(w, m, "value");
      std::string tc = thColorOf(w, tv);
      if (!tc.empty()) textColor = tc;
    }
    Gdiplus::SolidBrush brush(colorOf(textColor));
    std::wstring ws = widen(out);
    Gdiplus::RectF rc(static_cast<float>(x), static_cast<float>(y), static_cast<float>(W), static_cast<float>(H));
    g.DrawString(ws.c_str(), -1, &font, rc, &sf, &brush);
  }

  void drawBar(Gdiplus::Graphics& g, const Widget& w, const MetricMap& m, int x, int y, int W, int H) {
    double raw = bindValue(w, m, "value");
    if (std::isnan(raw)) raw = propNum(w, "value", 0);
    const double v = (std::max)(0.0, (std::min)(100.0, raw)) / 100.0;
    int radius = static_cast<int>(propNum(w, "radius", 4));
    std::string fill = propStr(w, "fillColor", "#22d3ee");
    {
      std::string tc = thColorOf(w, raw);
      if (!tc.empty()) fill = tc;
    }

    fillRound(g, x, y, W, H, radius, colorOf(propStr(w, "bgColor", "#27272a")));
    std::string dir = propStr(w, "direction", "h");
    if (dir == "v") {
      int hh = static_cast<int>(H * v);
      fillRound(g, x, y + H - hh, W, hh, radius, colorOf(fill));
    } else {
      int ww = static_cast<int>(W * v);
      fillRound(g, x, y, ww, H, radius, colorOf(fill));
    }
  }

  void drawArc(Gdiplus::Graphics& g, const Widget& w, const MetricMap& m, int x, int y, int W, int H) {
    double raw = bindValue(w, m, "value");
    if (std::isnan(raw)) raw = propNum(w, "value", 0);
    double v = (std::max)(0.0, (std::min)(100.0, raw));
    std::string arcColor = propStr(w, "arcColor", "#22d3ee");
    std::string warnColor = thColorOf(w, raw);
    if (!warnColor.empty()) arcColor = warnColor;

    float thick = static_cast<float>(propNum(w, "thickness", 10));
    float r = (std::min)(W, H) / 2.0f - thick / 2.0f - 2;
    if (r <= 0) return;
    float cx = x + W / 2.0f, cy = y + H / 2.0f;

    Gdiplus::Pen track(colorOf(propStr(w, "trackColor", "#27272a")), thick);
    track.SetStartCap(Gdiplus::LineCapRound);
    track.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&track, cx - r, cy - r, r * 2, r * 2, 135.f, 270.f);

    Gdiplus::Pen arc(colorOf(arcColor), thick);
    arc.SetStartCap(Gdiplus::LineCapRound);
    arc.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&arc, cx - r, cy - r, r * 2, r * 2, 135.f, static_cast<float>(270.0 * v / 100.0));

    float fs; std::string fam;
    auto style = parseStyle(propStr(w, "font", "bold 18px Microsoft YaHei"), fs, fam);
    Gdiplus::FontFamily ff(resolveFamily(fam).c_str());
    Gdiplus::Font font(&ff, fs, style, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush white(warnColor.empty() ? Gdiplus::Color(255, 255, 255, 255)
                                               : colorOf(warnColor));

    int dec = propDecimals(w);
    if (dec < 0) dec = 0;
    if (dec > 4) dec = 4;
    wchar_t num[48];
    swprintf_s(num, L"%.*f%%", dec, v);
    Gdiplus::RectF rc(cx - r, cy - r * 0.5f, r * 2, r);
    g.DrawString(num, -1, &font, rc, &sf, &white);

    std::string label = propStr(w, "label", "");
    if (!label.empty()) {
      Gdiplus::Font lf(&ff, r * 0.30f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
      Gdiplus::SolidBrush lb(warnColor.empty() ? Gdiplus::Color(255, 154, 167, 181)
                                               : colorOf(warnColor));
      Gdiplus::RectF lr(cx - r, cy + r * 0.42f, r * 2, r * 0.6f);
      std::wstring wl = widen(label);
      g.DrawString(wl.c_str(), -1, &lf, lr, &sf, &lb);
    }
  }

  void drawClock(Gdiplus::Graphics& g, const Widget& w, int x, int y, int W, int H) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string fmt = propStr(w, "fmt", "HH:mm");
    std::string s;
    for (size_t i = 0; i < fmt.size();) {
      if (fmt.compare(i, 2, "HH") == 0) { char b[4]; snprintf(b, sizeof(b), "%02d", st.wHour); s += b; i += 2; }
      else if (fmt.compare(i, 2, "mm") == 0) { char b[4]; snprintf(b, sizeof(b), "%02d", st.wMinute); s += b; i += 2; }
      else if (fmt.compare(i, 2, "ss") == 0) { char b[4]; snprintf(b, sizeof(b), "%02d", st.wSecond); s += b; i += 2; }
      else { s += fmt[i]; i++; }
    }

    float fs; std::string fam;
    auto style = parseStyle(propStr(w, "font", "bold 34px Microsoft YaHei"), fs, fam);
    Gdiplus::FontFamily ff(resolveFamily(fam).c_str());
    Gdiplus::Font font(&ff, fs, style, Gdiplus::UnitPixel);

    std::string align = propStr(w, "align", "center");
    Gdiplus::StringFormat sf;
    // 禁止换行，保证 "14:32" 始终一行
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetTrimming(Gdiplus::StringTrimmingNone);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    if (align == "center") sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    else if (align == "right") sf.SetAlignment(Gdiplus::StringAlignmentFar);
    else sf.SetAlignment(Gdiplus::StringAlignmentNear);

    bool showDate = propStr(w, "showDate", "") == "1";
    Gdiplus::SolidBrush brush(colorOf(propStr(w, "color", "#ffffff")));
    std::wstring ws(s.begin(), s.end());
    Gdiplus::RectF rc(static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(W), static_cast<float>(showDate ? H * 0.7f : H));
    g.DrawString(ws.c_str(), -1, &font, rc, &sf, &brush);

    if (showDate) {
      float dfs; std::string dfam;
      auto dstyle = parseStyle(propStr(w, "dateFont", "12px Microsoft YaHei"), dfs, dfam);
      Gdiplus::FontFamily dff(resolveFamily(dfam).c_str());
      Gdiplus::Font dfont(&dff, dfs, dstyle, Gdiplus::UnitPixel);
      const wchar_t* wk = L"日一二三四五六";
      wchar_t ds[64];
      swprintf_s(ds, L"%d月%d日 周%c", st.wMonth, st.wDay, wk[st.wDayOfWeek]);
      Gdiplus::SolidBrush db(colorOf(propStr(w, "dateColor", "#94a3b8")));
      Gdiplus::RectF dr(static_cast<float>(x), static_cast<float>(y + H * 0.62f),
                        static_cast<float>(W), static_cast<float>(H * 0.38f));
      g.DrawString(ds, -1, &dfont, dr, &sf, &db);
    }
  }

  void drawSpark(Gdiplus::Graphics& g, const Widget& w, const MetricMap& m, int x, int y, int W, int H) {
    std::string id = w.type + "_" + std::to_string(static_cast<int>(w.x)) + "_" +
                     std::to_string(static_cast<int>(w.y));
    auto& hist = sparkHist_[id];
    double v = bindValue(w, m, "value");
    if (std::isnan(v)) v = propNum(w, "value", 0);
    hist.push_back((std::max)(0.0, (std::min)(100.0, v)));
    size_t maxN = static_cast<size_t>((std::max)(4.0, (std::min)(200.0, propNum(w, "samples", 40))));
    while (hist.size() > maxN) hist.pop_front();
    if (hist.size() < 2) return;

    size_t n = hist.size();
    std::vector<Gdiplus::PointF> pts;
    pts.reserve(n);
    for (size_t i = 0; i < n; i++) {
      float px = x + (n == 1 ? 0 : (static_cast<float>(i) / (n - 1)) * W);
      float py = y + H - static_cast<float>(hist[i] / 100.0) * H;
      pts.emplace_back(px, py);
    }

    std::string fillHex = propStr(w, "fillColor", "");
    if (!fillHex.empty() && fillHex.size() >= 7) {
      Gdiplus::GraphicsPath path;
      path.AddLines(pts.data(), static_cast<INT>(pts.size()));
      path.AddLine(pts.back().X, pts.back().Y, static_cast<float>(x + W), static_cast<float>(y + H));
      path.AddLine(static_cast<float>(x + W), static_cast<float>(y + H), static_cast<float>(x), static_cast<float>(y + H));
      path.CloseFigure();
      Gdiplus::SolidBrush fb(colorOf(fillHex, 60));
      g.FillPath(&fb, &path);
    }

    Gdiplus::Pen pen(colorOf(propStr(w, "lineColor", "#22d3ee")),
                     static_cast<float>(propNum(w, "lineWidth", 2)));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawLines(&pen, pts.data(), static_cast<INT>(pts.size()));
  }

  void drawIcon(Gdiplus::Graphics& g, const Widget& w, int x, int y, int W, int H) {
    std::string bgHex = propStr(w, "bgColor", "");
    if (!bgHex.empty()) {
      Gdiplus::SolidBrush b(colorOf(bgHex));
      g.FillRectangle(&b, x, y, W, H);
    }
    double sz = propNum(w, "size", 0);
    if (sz <= 0) sz = (std::min)(W, H);
    float cx = x + W / 2.0f, cy = y + H / 2.0f, h = static_cast<float>(sz / 2.0);
    Gdiplus::SolidBrush brush(colorOf(propStr(w, "color", "#22d3ee")));
    std::string shape = propStr(w, "shape", "circle");

    if (shape == "circle") {
      g.FillEllipse(&brush, cx - h, cy - h, h * 2, h * 2);
    } else if (shape == "square") {
      g.FillRectangle(&brush, cx - h, cy - h, h * 2, h * 2);
    } else if (shape == "triangle") {
      Gdiplus::PointF p[3] = {{cx, cy - h}, {cx + h, cy + h}, {cx - h, cy + h}};
      g.FillPolygon(&brush, p, 3);
    } else if (shape == "cross") {
      float t = static_cast<float>(sz * 0.28);
      g.FillRectangle(&brush, cx - h, cy - t / 2, h * 2, t);
      g.FillRectangle(&brush, cx - t / 2, cy - h, t, h * 2);
    } else if (shape == "bolt") {
      Gdiplus::PointF p[6] = {
          {cx + static_cast<float>(sz * 0.08), cy - h},
          {cx - h * 0.55f, cy + static_cast<float>(sz * 0.06)},
          {cx - static_cast<float>(sz * 0.02), cy + static_cast<float>(sz * 0.06)},
          {cx - static_cast<float>(sz * 0.08), cy + h},
          {cx + h * 0.55f, cy - static_cast<float>(sz * 0.06)},
          {cx + static_cast<float>(sz * 0.02), cy - static_cast<float>(sz * 0.06)}};
      g.FillPolygon(&brush, p, 6);
    }
  }

  void drawDivider(Gdiplus::Graphics& g, const Widget& w, int x, int y, int W, int H) {
    float t = static_cast<float>(propNum(w, "thickness", 1));
    Gdiplus::Pen pen(colorOf(propStr(w, "color", "#27272a")), t);
    std::string style = propStr(w, "style", "solid");
    if (style == "dash") pen.SetDashPattern(std::vector<float>{3.f, 2.f}.data(), 2);
    else if (style == "dot") pen.SetDashPattern(std::vector<float>{1.f, 2.f}.data(), 2);

    if (W >= H) {
      float yy = y + H / 2.0f;
      g.DrawLine(&pen, static_cast<float>(x), yy, static_cast<float>(x + W), yy);
    } else {
      float xx = x + W / 2.0f;
      g.DrawLine(&pen, xx, static_cast<float>(y), xx, static_cast<float>(y + H));
    }
  }

  /** 支持 data:image/...;base64 内嵌图 */
  void drawImage(Gdiplus::Graphics& g, const Widget& w, int x, int y, int W, int H) {
    std::string src = propStr(w, "src", "");
    if (src.rfind("data:image", 0) != 0) return;

    auto comma = src.find(',');
    if (comma == std::string::npos) return;
    std::string b64 = src.substr(comma + 1);
    std::vector<BYTE> raw = base64Decode(b64);
    if (raw.empty()) return;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, raw.size());
    if (!hMem) return;
    void* p = GlobalLock(hMem);
    memcpy(p, raw.data(), raw.size());
    GlobalUnlock(hMem);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) { GlobalFree(hMem); return; }

    Gdiplus::Bitmap* img = Gdiplus::Bitmap::FromStream(stream);
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
      std::string fit = propStr(w, "fit", "cover");
      int iw = static_cast<int>(img->GetWidth()), ih = static_cast<int>(img->GetHeight());
      if (iw > 0 && ih > 0) {
        if (fit == "fill") {
          g.DrawImage(img, x, y, W, H);
        } else {
          double ir = static_cast<double>(iw) / ih, br = static_cast<double>(W) / H;
          int dw, dh;
          if ((fit == "cover") == (ir > br)) { dh = H; dw = static_cast<int>(dh * ir); }
          else { dw = W; dh = static_cast<int>(dw / ir); }
          g.DrawImage(img, x + (W - dw) / 2, y + (H - dh) / 2, dw, dh);
        }
      }
    }
    if (img) delete img;
    stream->Release();
    GlobalFree(hMem);
  }

  static std::vector<BYTE> base64Decode(const std::string& in) {
    static int T[256];
    static bool init = false;
    if (!init) {
      for (int i = 0; i < 256; i++) T[i] = -1;
      const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(a[i])] = i;
      init = true;
    }
    std::vector<BYTE> out;
    out.reserve(in.size() * 3 / 4 + 4);
    int val = 0, bits = 0;
    for (unsigned char c : in) {
      if (c == '=') break;
      int d = T[c];
      if (d < 0) continue;
      val = (val << 6) | d;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back(static_cast<BYTE>((val >> bits) & 0xFF));
      }
    }
    return out;
  }

  bool loaded_ = false;
  int canvasW_ = SCREEN_WIDTH;
  int canvasH_ = SCREEN_HEIGHT;
  int fps_ = 20;
  std::string background_ = "#000000";
  std::vector<Widget> widgets_;
  std::map<std::string, std::deque<double>> sparkHist_;
  GdiScreenCapture encoder_;
};

}  // namespace coolleo
