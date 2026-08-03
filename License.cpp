#include "License.hpp"
#include "Promo.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
#endif

namespace License {

namespace {

// Baked-in HMAC secret. Determined + rotated per major version. Anyone
// who reverses the exe can extract it - accepted tradeoff at 9,900 KRW
// pricing. If crack tooling starts publishing keys against this
// secret, bump kFormatPrefix, rotate this constant, and ship a new
// major version; existing licenses stay valid because the on-disk
// license carries the format prefix.
//
// NOTE: rotate this to a fresh 32-byte random value before public
// release. The placeholder here is fine for dev builds.
static const uint8_t kLicenseSecret[32] = {
    0x42, 0x41, 0x4E, 0x41, 0x4E, 0x41, 0x50, 0x4C,
    0x41, 0x59, 0x45, 0x52, 0x5F, 0x44, 0x45, 0x56,
    0xB0, 0x0B, 0xFA, 0xCE, 0xC0, 0xFF, 0xEE, 0xBA,
    0xDF, 0x00, 0xD1, 0x5E, 0xA5, 0xE0, 0xF0, 0x0D,
};

// Prefix that keys the HMAC input. Anything the key derivation
// depends on goes here so a future format change (say V5) can't be
// confused with V4 output.
constexpr const char* kHmacContext = "banana-player-v1:";
constexpr const char* kFormatPrefix = "V4-";

// Crockford Base32 alphabet, uppercase. Ambiguous chars (I / L / O / U)
// are omitted so users typing keys off a receipt don't confuse them.
constexpr const char* kBase32 =
    "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// -------- Small helpers --------

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string toLower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

std::string toUpper(std::string s) {
    for (auto& c : s) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    }
    return s;
}

// Normalize email so trivial variations map to the same license:
// trim whitespace + lowercase. NOT full RFC 5321 normalization; the
// server-side generator does the same trim+lower.
std::string normalizeEmail(const std::string& raw) {
    return toLower(trim(raw));
}

// Encode `n` bytes as base32 (Crockford, upper), no padding. Output
// character count = ceil(n * 8 / 5).
std::string base32Encode(const uint8_t* data, size_t n) {
    std::string out;
    out.reserve((n * 8 + 4) / 5);
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kBase32[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0) {
        out.push_back(kBase32[(buffer << (5 - bits)) & 0x1F]);
    }
    return out;
}

// Split a base32 string into hyphen-separated 4-char groups so users
// see "XXXX-XXXX-XXXX" instead of a wall of characters.
std::string groupsOf4(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (i % 4) == 0) out.push_back('-');
        out.push_back(s[i]);
    }
    return out;
}

// Strip hyphens + whitespace from a user-typed key so we can compare
// the raw base32 content regardless of formatting. Users may type
// spaces, uppercase / lowercase, extra hyphens; we tolerate all.
std::string stripFormatting(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        out.push_back(c);
    }
    return out;
}

#ifdef _WIN32
// HMAC-SHA256 via BCrypt (CNG). Any error returns an empty vector -
// callers treat that as verification failure.
std::vector<uint8_t> hmacSha256(const uint8_t* key, size_t key_len,
                                 const uint8_t* data, size_t data_len) {
    std::vector<uint8_t> out(32, 0);
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                             nullptr,
                                             BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!NT_SUCCESS(s)) return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    s = BCryptCreateHash(alg, &hash, nullptr, 0,
                         (PUCHAR)key, (ULONG)key_len, 0);
    if (!NT_SUCCESS(s)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    s = BCryptHashData(hash, (PUCHAR)data, (ULONG)data_len, 0);
    if (NT_SUCCESS(s)) {
        s = BCryptFinishHash(hash, out.data(), (ULONG)out.size(), 0);
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!NT_SUCCESS(s)) return {};
    return out;
}
#else
std::vector<uint8_t> hmacSha256(const uint8_t*, size_t,
                                 const uint8_t*, size_t) { return {}; }
#endif

// -------- State --------

std::mutex     g_mutex;
Status         g_status = Status::NotEntered;
std::string    g_email;
std::string    g_key;
std::string    g_license_path;

bool verifyLocked(const std::string& email, const std::string& key) {
    std::string normed = normalizeEmail(email);
    if (normed.empty()) return false;
    // Strip and uppercase whatever the user typed so "v4-abcd-efgh-jkmn"
    // and "V4 ABCD EFGH JKMN" both parse the same.
    std::string user_key = toUpper(stripFormatting(key));
    std::string expected = stripFormatting(keyForEmail(email));
    if (user_key.empty() || expected.empty()) return false;
    // Constant-time compare so timing side-channel attacks on the
    // client-side check don't leak the expected string.
    if (user_key.size() != expected.size()) return false;
    uint32_t diff = 0;
    for (size_t i = 0; i < user_key.size(); ++i) {
        diff |= (uint32_t)(user_key[i] ^ expected[i]);
    }
    return diff == 0;
}

void writeLicenseFile() {
    if (g_license_path.empty()) return;
    std::string tmp = g_license_path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "%s\n%s\n", g_email.c_str(), g_key.c_str());
    std::fflush(f);
    std::fclose(f);
#ifdef _WIN32
    MoveFileExA(tmp.c_str(), g_license_path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    std::rename(tmp.c_str(), g_license_path.c_str());
#endif
}

bool readLicenseFile(std::string& email_out, std::string& key_out) {
    std::FILE* f = std::fopen(g_license_path.c_str(), "rb");
    if (!f) return false;
    char buf[512];
    std::string content;
    while (std::fgets(buf, sizeof(buf), f)) content += buf;
    std::fclose(f);
    // First two lines are email + key. Split on \n.
    size_t nl = content.find('\n');
    if (nl == std::string::npos) return false;
    email_out = trim(content.substr(0, nl));
    std::string rest = content.substr(nl + 1);
    nl = rest.find('\n');
    if (nl == std::string::npos) nl = rest.size();
    key_out = trim(rest.substr(0, nl));
    return !email_out.empty() && !key_out.empty();
}

}   // namespace

// -------- Public API --------

std::string keyForEmail(const std::string& email) {
    std::string normed = normalizeEmail(email);
    if (normed.empty()) return {};
    std::string ctx = std::string(kHmacContext) + normed;
    auto digest = hmacSha256(kLicenseSecret, sizeof(kLicenseSecret),
                             (const uint8_t*)ctx.data(), ctx.size());
    if (digest.size() < 8) return {};
    std::string b32 = base32Encode(digest.data(), 8);   // 8 bytes -> ~13 chars
    // Truncate to 12 chars so it fits three neat 4-char groups.
    if (b32.size() > 12) b32.resize(12);
    return std::string(kFormatPrefix) + groupsOf4(b32);
}

void init(const std::string& data_dir) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string dir = data_dir;
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    g_license_path = dir + "\\banana.license";
    std::string em, kk;
    if (readLicenseFile(em, kk)) {
        if (verifyLocked(em, kk)) {
            g_email = normalizeEmail(em);
            g_key = kk;
            g_status = Status::Valid;
            Promo::setSuppressed(true);
        } else {
            g_status = Status::Invalid;
        }
    } else {
        g_status = Status::NotEntered;
    }
}

bool activate(const std::string& email, const std::string& key) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!verifyLocked(email, key)) {
        g_status = Status::Invalid;
        return false;
    }
    g_email  = normalizeEmail(email);
    g_key    = key;   // preserve the user's formatting for later display
    g_status = Status::Valid;
    writeLicenseFile();
    Promo::setSuppressed(true);
    return true;
}

void deactivate() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_email.clear();
    g_key.clear();
    g_status = Status::NotEntered;
    if (!g_license_path.empty()) {
        std::remove(g_license_path.c_str());
    }
    Promo::setSuppressed(false);
}

Status status() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_status;
}

bool isPro() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_status == Status::Valid;
}

std::string activatedEmail() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return (g_status == Status::Valid) ? g_email : std::string();
}

}   // namespace License
