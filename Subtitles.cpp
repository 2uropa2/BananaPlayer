#include "Subtitles.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace {

// ---------- shared helpers ----------

// std::fopen on Windows treats the path as ANSI (CP949 on Korean locale),
// but SDL3 drop events and our own drag-drop pipeline deliver UTF-8. A path
// with characters outside CP949 - and even a Korean-named file that Windows
// happens to store as UTF-16 in the file system - fails to open through
// plain fopen because the CP949 round-trip loses information. Wide-path the
// UTF-8 -> UTF-16 conversion and call _wfopen so the actual filesystem
// lookup runs against Unicode. Non-Windows paths keep the plain fopen path.
std::FILE* fopenUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wpath(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    wchar_t wmode[8] = {0};
    for (int i = 0; mode[i] && i < 7; ++i) wmode[i] = (wchar_t)mode[i];
    return _wfopen(wpath.c_str(), wmode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Strip leading BOM (EF BB BF).
std::string_view stripBOM(std::string_view s) {
    if (s.size() >= 3
        && (unsigned char)s[0] == 0xEF
        && (unsigned char)s[1] == 0xBB
        && (unsigned char)s[2] == 0xBF) {
        return s.substr(3);
    }
    return s;
}

// Split into lines, dropping the trailing \r on CRLF.
std::vector<std::string_view> splitLines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t nl = s.find('\n', i);
        if (nl == std::string_view::npos) {
            if (i < s.size()) out.emplace_back(s.substr(i));
            break;
        }
        size_t end = nl;
        if (end > i && s[end - 1] == '\r') --end;
        out.emplace_back(s.substr(i, end - i));
        i = nl + 1;
    }
    return out;
}

std::string_view trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Parse "HH:MM:SS,mmm" or "HH:MM:SS.mmm" into seconds.
bool parseSrtTimestamp(std::string_view s, double& out) {
    unsigned int h, m, sec, ms;
    // Try comma first, then dot.
    if (std::sscanf(std::string(s).c_str(), "%u:%u:%u,%u", &h, &m, &sec, &ms) == 4
        || std::sscanf(std::string(s).c_str(), "%u:%u:%u.%u", &h, &m, &sec, &ms) == 4) {
        out = h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
        return true;
    }
    return false;
}

// ---------- SRT ----------

// Line looks like: "00:00:20,000 --> 00:00:24,400"
bool parseSrtTimingLine(std::string_view line, double& start, double& end) {
    auto arrow = line.find("-->");
    if (arrow == std::string_view::npos) return false;
    auto left  = trim(line.substr(0, arrow));
    auto right = trim(line.substr(arrow + 3));
    // Trailing style hints ("X1:... Y1:...") may appear after the end stamp -
    // clip on the first space if any.
    auto rspace = right.find(' ');
    if (rspace != std::string_view::npos) right = right.substr(0, rspace);
    return parseSrtTimestamp(left, start) && parseSrtTimestamp(right, end);
}

// ---------- SMI ----------

std::string smiUnescape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '<') {
            // Handle a few common inline tags, drop the rest.
            size_t end = raw.find('>', i);
            if (end == std::string_view::npos) break;
            std::string tag = toLowerAscii(raw.substr(i + 1, end - i - 1));
            // Strip attributes to compare the tag name only.
            auto sp = tag.find(' ');
            std::string name = (sp == std::string::npos) ? tag : tag.substr(0, sp);
            // Also strip a leading '/' for closing tags.
            std::string bare = (!name.empty() && name[0] == '/') ? name.substr(1) : name;
            if (bare == "br") out.push_back('\n');
            // <b>, <i>, <font>, <p>, </p>, etc. are dropped.
            i = end + 1;
        } else if (raw[i] == '&') {
            // Handle &nbsp;, &amp;, &lt;, &gt;, &quot;. Anything else falls
            // through as literal '&'.
            auto sc = raw.find(';', i);
            if (sc != std::string_view::npos && sc - i <= 8) {
                std::string entity = toLowerAscii(raw.substr(i + 1, sc - i - 1));
                if      (entity == "nbsp")  { /* skip - handled by caller */ }
                else if (entity == "amp")   out.push_back('&');
                else if (entity == "lt")    out.push_back('<');
                else if (entity == "gt")    out.push_back('>');
                else if (entity == "quot")  out.push_back('"');
                else if (entity == "apos")  out.push_back('\'');
                else                        out.push_back('&');
                i = sc + 1;
                continue;
            }
            out.push_back('&');
            ++i;
        } else {
            out.push_back((char)raw[i]);
            ++i;
        }
    }
    // Strip trailing whitespace/newlines then leading, but keep interior.
    size_t a = 0, b = out.size();
    while (a < b && (unsigned char)out[a] <= ' ') ++a;
    while (b > a && (unsigned char)out[b - 1] <= ' ') --b;
    return out.substr(a, b - a);
}

// Body of `<P Class="XYZ">`. Returns the class value in lowercase or "".
std::string smiExtractClass(std::string_view tag) {
    // Find "class=" in a case-insensitive way.
    std::string lower = toLowerAscii(tag);
    auto pos = lower.find("class");
    if (pos == std::string::npos) return "";
    pos = lower.find('=', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '"' || lower[pos] == '\'')) ++pos;
    size_t end = pos;
    while (end < lower.size() && lower[end] != ' ' && lower[end] != '"'
           && lower[end] != '\'' && lower[end] != '>') ++end;
    return lower.substr(pos, end - pos);
}

} // namespace

// ---------- public utilities ----------

bool isValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t need = 0;
        if      (c < 0x80)                  need = 0;
        else if ((c & 0xE0) == 0xC0)        need = 1;
        else if ((c & 0xF0) == 0xE0)        need = 2;
        else if ((c & 0xF8) == 0xF0)        need = 3;
        else                                return false;
        if (i + need >= s.size()) return false;
        for (size_t k = 1; k <= need; ++k) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

// ---------- public parsers ----------

std::vector<SubtitleCue> parseSRT(std::string_view utf8) {
    std::vector<SubtitleCue> cues;
    auto text = stripBOM(utf8);
    auto lines = splitLines(text);

    size_t i = 0;
    while (i < lines.size()) {
        // Skip blanks and (optional) index lines.
        while (i < lines.size() && trim(lines[i]).empty()) ++i;
        if (i >= lines.size()) break;
        // A numeric line here is an index; skip. Otherwise this line might be
        // the timing line directly.
        {
            auto tl = trim(lines[i]);
            bool numeric = !tl.empty();
            for (char c : tl) if (!(c >= '0' && c <= '9')) { numeric = false; break; }
            if (numeric && (i + 1 < lines.size())
                && lines[i + 1].find("-->") != std::string_view::npos) {
                ++i;
            }
        }
        if (i >= lines.size()) break;

        double start = 0.0, end = 0.0;
        if (!parseSrtTimingLine(trim(lines[i]), start, end)) {
            ++i;  // not a timing line; skip and try again
            continue;
        }
        ++i;

        // Read text lines until blank or EOF.
        std::string text_block;
        while (i < lines.size() && !trim(lines[i]).empty()) {
            if (!text_block.empty()) text_block.push_back('\n');
            text_block.append(std::string(trim(lines[i])));
            ++i;
        }
        if (end > start && !text_block.empty()) {
            SubtitleCue cue;
            cue.start = start;
            cue.end   = end;
            cue.text  = text_block;
            cues.push_back(std::move(cue));
        }
    }

    std::sort(cues.begin(), cues.end(),
              [](const SubtitleCue& a, const SubtitleCue& b){ return a.start < b.start; });
    return cues;
}

std::vector<SubtitleCue> parseSMI(std::string_view utf8, std::string_view preferred_class) {
    std::vector<SubtitleCue> cues;
    auto text = stripBOM(utf8);

    struct Raw { double start; std::string text_class; std::string body; };
    std::vector<Raw> raws;

    // Walk the document tag-by-tag, latching onto <SYNC> then reading the
    // body up to the next <SYNC> or end of file.
    std::string lower = toLowerAscii(text);
    size_t i = 0;
    while (i < text.size()) {
        auto tag_start = lower.find("<sync", i);
        if (tag_start == std::string::npos) break;
        auto tag_end = text.find('>', tag_start);
        if (tag_end == std::string::npos) break;

        // Parse Start= attribute.
        std::string syncTag = toLowerAscii(text.substr(tag_start, tag_end - tag_start));
        auto sp = syncTag.find("start");
        double start_sec = -1.0;
        if (sp != std::string::npos) {
            sp = syncTag.find('=', sp);
            if (sp != std::string::npos) {
                ++sp;
                while (sp < syncTag.size() && (syncTag[sp] == ' ' || syncTag[sp] == '"' || syncTag[sp] == '\'')) ++sp;
                long long ms = 0;
                bool any = false;
                while (sp < syncTag.size() && syncTag[sp] >= '0' && syncTag[sp] <= '9') {
                    ms = ms * 10 + (syncTag[sp] - '0'); ++sp; any = true;
                }
                if (any) start_sec = ms / 1000.0;
            }
        }

        // Body runs from after '>' to the next <sync>.
        size_t body_start = tag_end + 1;
        auto next_sync = lower.find("<sync", body_start);
        size_t body_end = (next_sync == std::string::npos) ? text.size() : next_sync;
        std::string_view body = text.substr(body_start, body_end - body_start);

        // Extract class from the first <P ...> tag in body if any.
        std::string cls;
        {
            std::string blower(toLowerAscii(body));
            auto pt = blower.find("<p");
            if (pt != std::string::npos) {
                auto pe = blower.find('>', pt);
                if (pe != std::string::npos) {
                    cls = smiExtractClass(body.substr(pt, pe - pt));
                }
            }
        }

        if (start_sec >= 0.0) {
            Raw r;
            r.start = start_sec;
            r.text_class = cls;
            r.body = smiUnescape(body);
            raws.push_back(std::move(r));
        }

        i = body_end;
    }

    // If a preferred class is given and any cue matches, filter down to that
    // class - many SMI files ship multiple language tracks in parallel.
    if (!preferred_class.empty()) {
        bool has_preferred = false;
        std::string pref = toLowerAscii(preferred_class);
        for (auto& r : raws) if (r.text_class == pref) { has_preferred = true; break; }
        if (has_preferred) {
            raws.erase(std::remove_if(raws.begin(), raws.end(),
                        [&](const Raw& r){ return r.text_class != pref; }),
                       raws.end());
        }
    }

    std::sort(raws.begin(), raws.end(),
              [](const Raw& a, const Raw& b){ return a.start < b.start; });

    // Convert to cues: each cue's end is the next entry's start, clipped by
    // an empty body which signals "clear the previous cue."
    for (size_t k = 0; k < raws.size(); ++k) {
        const auto& r = raws[k];
        if (r.body.empty()) continue;  // clear marker
        double end;
        if (k + 1 < raws.size()) end = raws[k + 1].start;
        else                     end = r.start + 5.0;  // last cue - assume 5s
        if (end > r.start) {
            SubtitleCue cue;
            cue.start = r.start;
            cue.end   = end;
            cue.text  = r.body;
            cues.push_back(std::move(cue));
        }
    }
    return cues;
}

// ---------- decode + load ----------

std::string decodeSubtitleFileToUtf8(const std::string& path) {
    std::FILE* f = fopenUtf8(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return {}; }
    std::string buf(n, '\0');
    size_t got = std::fread(buf.data(), 1, (size_t)n, f);
    std::fclose(f);
    buf.resize(got);

    if (isValidUtf8(buf)) return buf;

#ifdef _WIN32
    // Assume CP949 (Korean) - the common case for Korean .smi/.srt files that
    // aren't UTF-8. MultiByteToWideChar -> WideCharToMultiByte to UTF-8.
    int wlen = MultiByteToWideChar(949, 0, buf.data(), (int)buf.size(), nullptr, 0);
    if (wlen <= 0) return buf;  // give up, return raw bytes
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(949, 0, buf.data(), (int)buf.size(), wide.data(), wlen);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return buf;
    std::string out(u8len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, out.data(), u8len, nullptr, nullptr);
    return out;
#else
    return buf;
#endif
}

std::vector<SubtitleTrack> enumerateCompanionSubtitles(const std::string& video_path) {
    std::vector<SubtitleTrack> out;
#ifdef _WIN32
    // Split path -> (dir, basename-without-ext, ext). Chapter picker uses
    // both the directory (for FindFirstFile scan) and the basename (for
    // the wildcard).
    auto slash = video_path.find_last_of("\\/");
    std::string dir  = (slash == std::string::npos) ? std::string(".")
                                                    : video_path.substr(0, slash);
    std::string fname = (slash == std::string::npos) ? video_path
                                                     : video_path.substr(slash + 1);
    auto dot = fname.find_last_of('.');
    std::string base = (dot == std::string::npos) ? fname : fname.substr(0, dot);

    // FindFirstFileA operates on ANSI paths - the same UTF-8 vs CP949
    // trap that broke subtitle open before. Wide-path it via FindFirstFileW.
    std::string pattern_utf8 = dir + "\\" + base + "*";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, pattern_utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return out;
    std::wstring wpattern(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, pattern_utf8.c_str(), -1, wpattern.data(), wlen);

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(wpattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    auto endsWithI = [](std::wstring_view s, std::wstring_view suf) {
        if (s.size() < suf.size()) return false;
        for (size_t i = 0; i < suf.size(); ++i) {
            wchar_t a = (wchar_t)std::towlower(s[s.size() - suf.size() + i]);
            wchar_t b = (wchar_t)std::towlower(suf[i]);
            if (a != b) return false;
        }
        return true;
    };

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring wname = fd.cFileName;
        bool is_srt = endsWithI(wname, L".srt");
        bool is_smi = endsWithI(wname, L".smi");
        if (!is_srt && !is_smi) continue;

        // Widen -> UTF-8 for our string_view world.
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1,
                                        nullptr, 0, nullptr, nullptr);
        if (u8len <= 1) continue;
        std::string u8(u8len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, u8.data(),
                            u8len, nullptr, nullptr);

        SubtitleTrack t;
        t.path = dir + "\\" + u8;
        // Label: strip base name from the front and the .srt/.smi from the
        // back. `movie.ko.srt` -> ".ko" -> "ko". `movie.srt` -> "" -> "default".
        std::string mid;
        if (u8.size() > base.size()) {
            mid = u8.substr(base.size());
            // strip extension
            size_t d2 = mid.find_last_of('.');
            if (d2 != std::string::npos) mid = mid.substr(0, d2);
            // strip a leading dot
            if (!mid.empty() && mid.front() == '.') mid = mid.substr(1);
        }
        if (mid.empty()) mid = is_smi ? "default (smi)" : "default";
        t.label = std::move(mid);
        out.push_back(std::move(t));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Sort: non-default entries alphabetically first (language tags), then
    // the plain default, then any .smi. The current path already tends to
    // interleave; sort by label makes the picker menu predictable.
    std::sort(out.begin(), out.end(),
              [](const SubtitleTrack& a, const SubtitleTrack& b) {
                  auto rank = [](const std::string& l) {
                      if (l == "default") return 1;
                      if (l == "default (smi)") return 2;
                      return 0;
                  };
                  int ra = rank(a.label), rb = rank(b.label);
                  if (ra != rb) return ra < rb;
                  return a.label < b.label;
              });
#endif
    return out;
}

std::vector<SubtitleCue> loadCompanionSubtitles(const std::string& video_path) {
    // Strip extension and try common subtitle extensions in a preference order.
    auto dot = video_path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string base = video_path.substr(0, dot);

    struct Candidate { const char* ext; bool is_smi; };
    const Candidate cands[] = {
        { ".srt",  false },
        { ".SRT",  false },
        { ".smi",  true  },
        { ".SMI",  true  },
    };
    for (const auto& c : cands) {
        std::string path = base + c.ext;
        std::FILE* f = fopenUtf8(path, "rb");
        if (!f) continue;
        std::fclose(f);
        std::string content = decodeSubtitleFileToUtf8(path);
        if (content.empty()) continue;
        auto cues = c.is_smi ? parseSMI(content) : parseSRT(content);
        if (!cues.empty()) return cues;
    }
    return {};
}
