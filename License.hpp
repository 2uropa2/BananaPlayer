#pragma once
#include <string>
// V5 licenses use offline ECDSA P-256 verification with a public key only.
// Publisher issuance: tools/license_admin.ps1. V4 development keys need reissue;
// existing license files are preserved when verification fails.
namespace License {
enum class Status { NotEntered, Valid, Invalid };
void init(const std::string& data_dir);
bool activate(const std::string& email, const std::string& key);
void deactivate();
Status status();
bool isPro();
std::string activatedEmail();
}
