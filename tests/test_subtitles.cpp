#include "doctest/doctest.h"
#include "Subtitles.hpp"

// -------- UTF-8 validation --------

TEST_CASE("isValidUtf8 accepts ASCII") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("hello world"));
    CHECK(isValidUtf8("plain\nascii\r\ntext"));
}

TEST_CASE("isValidUtf8 accepts multi-byte sequences") {
    // Korean two-syllable word: EC 95 88 EB 85 95 (an-nyeong).
    CHECK(isValidUtf8("\xEC\x95\x88\xEB\x85\x95"));
    // Emoji clapper board U+1F3AC = F0 9F 8E AC.
    CHECK(isValidUtf8("\xF0\x9F\x8E\xAC"));
    // Latin-1 accented "cafe" = c a f e-with-acute (C3 A9).
    CHECK(isValidUtf8("caf\xC3\xA9"));
}

TEST_CASE("isValidUtf8 rejects obviously invalid bytes") {
    // Bare 0xFF is never valid in UTF-8.
    CHECK_FALSE(isValidUtf8("\xFF"));
    // Continuation byte with no lead.
    CHECK_FALSE(isValidUtf8("\x80"));
    // Truncated multi-byte sequence.
    CHECK_FALSE(isValidUtf8("\xC3"));
    // Lead byte followed by non-continuation.
    CHECK_FALSE(isValidUtf8("\xC3Z"));
}

TEST_CASE("isValidUtf8 rejects CP949 bytes typical of Korean text") {
    // Byte pattern B0 A1 B3 AA B4 D9 is CP949 for the three-syllable
    // "GaNaDa" test string. Not valid UTF-8 - two of the leading bytes
    // (0xB0, 0xB3, 0xB4) start a 4-byte UTF-8 sequence but the follow
    // bytes are not 10xxxxxx, so a strict validator must reject.
    CHECK_FALSE(isValidUtf8("\xB0\xA1\xB3\xAA\xB4\xD9"));
}

// -------- SRT parser --------

TEST_CASE("parseSRT handles a canonical block") {
    const char* srt =
        "1\n"
        "00:00:20,000 --> 00:00:24,400\n"
        "Hello world\n";
    auto cues = parseSRT(srt);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].start == doctest::Approx(20.0));
    CHECK(cues[0].end   == doctest::Approx(24.4));
    CHECK(cues[0].text  == "Hello world");
}

TEST_CASE("parseSRT handles multiple cues separated by blank lines") {
    const char* srt =
        "1\n00:00:01,000 --> 00:00:02,000\nfirst\n\n"
        "2\n00:00:03,500 --> 00:00:04,000\nsecond\n\n"
        "3\n00:00:05,000 --> 00:00:06,000\nthird\n";
    auto cues = parseSRT(srt);
    REQUIRE(cues.size() == 3);
    CHECK(cues[0].text == "first");
    CHECK(cues[1].text == "second");
    CHECK(cues[2].text == "third");
    CHECK(cues[1].start == doctest::Approx(3.5));
}

TEST_CASE("parseSRT preserves multi-line text") {
    const char* srt =
        "1\n00:00:01,000 --> 00:00:02,000\n"
        "line one\n"
        "line two\n"
        "line three\n";
    auto cues = parseSRT(srt);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "line one\nline two\nline three");
}

TEST_CASE("parseSRT tolerates CRLF and BOM") {
    const char* srt =
        "\xEF\xBB\xBF"  // UTF-8 BOM
        "1\r\n00:00:10,000 --> 00:00:12,000\r\nhi\r\n";
    auto cues = parseSRT(srt);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "hi");
    CHECK(cues[0].start == doctest::Approx(10.0));
}

TEST_CASE("parseSRT accepts dot-separated milliseconds") {
    // Some tools emit "HH:MM:SS.mmm" instead of "HH:MM:SS,mmm".
    const char* srt =
        "1\n00:00:07.500 --> 00:00:09.250\ndot format\n";
    auto cues = parseSRT(srt);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].start == doctest::Approx(7.5));
    CHECK(cues[0].end   == doctest::Approx(9.25));
}

TEST_CASE("parseSRT drops cues with end <= start") {
    const char* srt =
        "1\n00:00:05,000 --> 00:00:05,000\nempty\n\n"
        "2\n00:00:10,000 --> 00:00:12,000\nkeep me\n";
    auto cues = parseSRT(srt);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "keep me");
}

TEST_CASE("parseSRT returns cues sorted by start time even if input is out of order") {
    const char* srt =
        "1\n00:00:10,000 --> 00:00:12,000\nlate\n\n"
        "2\n00:00:02,000 --> 00:00:03,000\nearly\n";
    auto cues = parseSRT(srt);
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].text == "early");
    CHECK(cues[1].text == "late");
}

TEST_CASE("parseSRT handles empty and garbage input") {
    CHECK(parseSRT("").empty());
    CHECK(parseSRT("this is not srt at all\nno timing lines").empty());
}

// -------- SMI parser --------

TEST_CASE("parseSMI extracts basic cues") {
    const char* smi =
        "<SAMI><BODY>"
        "<SYNC Start=1000><P Class=KRCC>hello</P>"
        "<SYNC Start=3000><P Class=KRCC>world</P>"
        "<SYNC Start=5000><P Class=KRCC>&nbsp;</P>"
        "</BODY></SAMI>";
    auto cues = parseSMI(smi);
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].text == "hello");
    CHECK(cues[0].start == doctest::Approx(1.0));
    CHECK(cues[0].end   == doctest::Approx(3.0));
    CHECK(cues[1].text == "world");
    CHECK(cues[1].start == doctest::Approx(3.0));
    CHECK(cues[1].end   == doctest::Approx(5.0));
}

TEST_CASE("parseSMI converts <br> to newline") {
    const char* smi =
        "<SYNC Start=0><P>line one<br>line two</P>"
        "<SYNC Start=2000><P>&nbsp;</P>";
    auto cues = parseSMI(smi);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "line one\nline two");
}

TEST_CASE("parseSMI is case-insensitive on tag names") {
    const char* smi =
        "<sync start=500><p>lower</p>"
        "<Sync Start=1500><P>mixed</P>"
        "<SYNC Start=2500><P>&nbsp;</P>";
    auto cues = parseSMI(smi);
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].text == "lower");
    CHECK(cues[1].text == "mixed");
}

TEST_CASE("parseSMI resolves HTML entities") {
    const char* smi =
        "<SYNC Start=0><P>a &amp; b &lt;tag&gt; &quot;q&quot;</P>"
        "<SYNC Start=1000><P>&nbsp;</P>";
    auto cues = parseSMI(smi);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "a & b <tag> \"q\"");
}

TEST_CASE("parseSMI with preferred_class filters when the class exists") {
    const char* smi =
        "<SYNC Start=0><P Class=KRCC>ko</P>"
        "<SYNC Start=0><P Class=ENCC>en</P>"
        "<SYNC Start=1000><P Class=KRCC>&nbsp;</P>"
        "<SYNC Start=1000><P Class=ENCC>&nbsp;</P>";
    auto cues_ko = parseSMI(smi, "KRCC");
    REQUIRE(cues_ko.size() == 1);
    CHECK(cues_ko[0].text == "ko");
    auto cues_en = parseSMI(smi, "ENCC");
    REQUIRE(cues_en.size() == 1);
    CHECK(cues_en[0].text == "en");
}

TEST_CASE("parseSMI ignores preferred_class if no cue has it") {
    const char* smi =
        "<SYNC Start=0><P Class=KRCC>only ko</P>"
        "<SYNC Start=1000><P Class=KRCC>&nbsp;</P>";
    auto cues = parseSMI(smi, "ENCC");
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "only ko");
}

TEST_CASE("parseSMI skips <SYNC> without a Start attribute") {
    const char* smi =
        "<SYNC><P>orphan</P>"
        "<SYNC Start=1000><P>real</P>"
        "<SYNC Start=2000><P>&nbsp;</P>";
    auto cues = parseSMI(smi);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "real");
}

TEST_CASE("parseSMI gives the last cue a default 5s duration") {
    const char* smi = "<SYNC Start=1000><P>tail</P>";
    auto cues = parseSMI(smi);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].end - cues[0].start == doctest::Approx(5.0));
}

TEST_CASE("parseSMI handles empty and garbage input") {
    CHECK(parseSMI("").empty());
    CHECK(parseSMI("<html><body>no sync tags</body></html>").empty());
}
