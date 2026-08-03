#pragma once

// Banana Player Pro license key verification.
//
// Format:  V4-XXXX-XXXX-XXXX
//   V4     : format version prefix
//   XXXX   : base32 (Crockford, upper) chunks; 12 chars = 60 bits
//
// A key is computed as:
//   digest = HMAC-SHA256(kLicenseSecret,
//                        "banana-player-v1:" + normalized_email)
//   key    = base32(first 8 bytes of digest), split into 4-char groups
//
// Verification is offline - the client re-derives the expected key
// from the user's email and compares. The secret lives baked into the
// exe. Anyone determined can extract it, but at 9,900 KRW / one-time
// the crack incentive is low (WinRAR / Bandizip level protection).
//
// Storage: %APPDATA%\BananaPlayer\banana.license
//   line 1: email
//   line 2: key
// Lines trimmed; anything past line 2 ignored.
//
// UI: About panel exposes "Enter license" input + "Verify" button.
// On successful verify, License::isPro() returns true and
// Promo::setSuppressed(true) is called to hide the free-tier banner.

#include <string>

namespace License {

enum class Status {
    NotEntered,   // no banana.license on disk (fresh install / free user)
    Valid,        // saved license verified this session
    Invalid,      // saved license failed verification (tampered / renamed email)
};

// Load persisted license (if any) and verify it. Called from
// UIController ctor. Silent on failure.
void init(const std::string& data_dir);

// Try to accept a user-entered email + key pair. Returns true on
// success; on success the pair is persisted to banana.license, isPro()
// starts returning true, and Promo::setSuppressed(true) is called.
bool activate(const std::string& email, const std::string& key);

// Wipe the persisted license. isPro() returns false after this;
// Promo::setSuppressed(false) is called. Used by a "Remove license"
// action in About; also useful for testing.
void deactivate();

Status status();
bool   isPro();
std::string activatedEmail();   // empty when not Pro

// Deterministic key generator - used by the server-side script that
// generates keys after a successful payment. Also exposed here so
// tests can round-trip verify without duplicating the math. Given an
// email, returns the formatted key that would verify. Empty string on
// input validation failure.
std::string keyForEmail(const std::string& email);

}   // namespace License
