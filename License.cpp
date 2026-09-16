#include "License.hpp"
#include "Promo.hpp"
#include "FileIO.hpp"
#include <cstdint>
#include <mutex>
#include <vector>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#ifndef BP_LICENSE_PUBLIC_KEY_HEADER
#define BP_LICENSE_PUBLIC_KEY_HEADER "resources/license_public_key.hpp"
#endif
#include BP_LICENSE_PUBLIC_KEY_HEADER
namespace License {
namespace {
std::mutex g_mutex;
Status g_status = Status::NotEntered;
std::string g_email, g_key, g_license_path;
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::string normalizeEmail(const std::string& raw) {
    auto email = trim(raw);
    if (email.size() > 254 || email.find('@') == std::string::npos) return {};
    for (char& ch : email) {
        if ((unsigned char)ch < 33 || (unsigned char)ch > 126) return {};
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    return email;
}
std::vector<unsigned char> signatureBytes(const std::string& raw) {
    const auto key = trim(raw);
    if (key.size() != 89 || key.compare(0, 3, "V5-") != 0) return {};
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::vector<unsigned char> bytes;
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 3; i < key.size(); ++i) {
        const auto value = alphabet.find(key[i]);
        if (value == std::string::npos) return {};
        buffer = (buffer << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back((unsigned char)(buffer >> bits)); }
    }
    if (bits && (buffer & ((1u << bits) - 1u))) return {};
    return bytes.size() == 64 ? bytes : std::vector<unsigned char>();
}
bool verifyLocked(const std::string& email, const std::string& license) {
    const auto normalized = normalizeEmail(email);
    auto signature = signatureBytes(license);
    if (normalized.empty() || signature.empty()) return false;
    const auto payload = "banana-player-license-v5:" + normalized;
    BCRYPT_ALG_HANDLE sha = nullptr, ec = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    unsigned char digest[32]{};
    bool ok = BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0
        && BCryptCreateHash(sha, &hash, nullptr, 0, nullptr, 0, 0) >= 0
        && BCryptHashData(hash, (PUCHAR)payload.data(), (ULONG)payload.size(), 0) >= 0
        && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0
        && BCryptOpenAlgorithmProvider(&ec, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) >= 0
        && BCryptImportKeyPair(ec, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                              (PUCHAR)kLicensePublicKey, sizeof(kLicensePublicKey), 0) >= 0
        && BCryptVerifySignature(key, nullptr, digest, sizeof(digest), signature.data(), (ULONG)signature.size(), 0) >= 0;
    if (key) BCryptDestroyKey(key);
    if (hash) BCryptDestroyHash(hash);
    if (ec) BCryptCloseAlgorithmProvider(ec, 0);
    if (sha) BCryptCloseAlgorithmProvider(sha, 0);
    return ok;
}
bool writeLicenseFile() {
    if (g_license_path.empty()) return false;
    const auto temporary = g_license_path + ".tmp";
    FILE* file = FileIO::open(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fprintf(file, "%s\n%s\n", g_email.c_str(), g_key.c_str()) > 0;
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (ok) ok = FileIO::replace(temporary, g_license_path);
    if (!ok) FileIO::remove(temporary.c_str());
    return ok;
}
bool readLicenseFile(std::string& email, std::string& key) {
    FILE* file = FileIO::open(g_license_path.c_str(), "rb");
    if (!file) return false;
    char first[512]{}, second[512]{};
    bool ok = std::fgets(first, sizeof(first), file) && std::fgets(second, sizeof(second), file);
    std::fclose(file);
    if (ok) { email = trim(first); key = trim(second); }
    return ok && !email.empty() && !key.empty();
}
} // namespace

void init(const std::string& data_dir) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string dir = data_dir;
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    g_license_path = dir + "\\banana.license";
    g_status = Status::NotEntered;
    g_email.clear();
    g_key.clear();
    Promo::setSuppressed(false);
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
        Promo::setSuppressed(false);
        return false;
    }
    g_email  = normalizeEmail(email);
    g_key    = key;   // preserve the user's formatting for later display
    g_status = Status::Valid;
    if (!writeLicenseFile()) {
        g_status = Status::Invalid;
        g_email.clear();
        g_key.clear();
        Promo::setSuppressed(false);
        return false;
    }
    Promo::setSuppressed(true);
    return true;
}

void deactivate() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_email.clear();
    g_key.clear();
    g_status = Status::NotEntered;
    if (!g_license_path.empty()) {
        FileIO::remove(g_license_path.c_str());
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
