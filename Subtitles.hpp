#pragma once

#include <string>
#include <string_view>
#include <vector>

// Pure, testable subtitle parsers. Callers hand in UTF-8 text and get back a
// time-sorted list of cues; the loader helper below wraps file I/O + a naive
// UTF-8-or-CP949 decode for the common Korean case.

// Exposed so tests can validate the encoding-detection path directly.
bool isValidUtf8(std::string_view s);

struct SubtitleCue {
    double      start = 0.0;   // seconds
    double      end   = 0.0;   // seconds; must satisfy end > start
    std::string text;          // UTF-8, may contain '\n' for multi-line
};

// Parse SRT (SubRip). Blank-line separated blocks of `index / timestamps /
// text*`. Robust to CRLF, extra blank lines, and missing index numbers.
std::vector<SubtitleCue> parseSRT(std::string_view utf8);

// Parse SAMI/SMI. Case-insensitive tags. `<SYNC Start=N>` marks a cue's start
// in milliseconds; the cue's text runs until the next `<SYNC>` (a `&nbsp;`
// body means "clear/end the previous cue"). Multiple language classes are
// merged - callers can prefer one via `preferred_class` (e.g. "KRCC").
std::vector<SubtitleCue> parseSMI(std::string_view utf8,
                                  std::string_view preferred_class = "");

// Read `path` from disk and best-effort convert its bytes to UTF-8. If the
// content already parses as UTF-8 it is returned as-is; otherwise it is
// treated as CP949 (Korean codepage) and converted. Returns "" if the file
// can't be read.
std::string decodeSubtitleFileToUtf8(const std::string& path);

// Given a video path (e.g. `C:\videos\clip.mp4`), look for `clip.srt` then
// `clip.smi` next to it and return parsed cues. Empty vector if none.
std::vector<SubtitleCue> loadCompanionSubtitles(const std::string& video_path);

// One sidecar subtitle track discovered next to the video: an absolute
// `path` plus a UI `label` derived from the tag between basename and
// extension (e.g. `movie.ko.srt` -> "ko", `movie.srt` -> "default").
struct SubtitleTrack {
    std::string path;
    std::string label;
};

// Enumerate every `<basename>*.srt` / `<basename>*.smi` next to the video
// path. Used to build a track picker when a folder has multiple language
// sidecars (movie.ko.srt, movie.en.srt, ...). Order: `basename.<tag>.srt`
// entries alphabetically, then the plain `basename.srt`, then the .smi
// variants (which are rarer for multi-language content).
std::vector<SubtitleTrack> enumerateCompanionSubtitles(const std::string& video_path);
