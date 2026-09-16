#pragma once

// All application paths are UTF-8. Convert only at the Windows boundary.
#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace FileIO {
#ifdef _WIN32
inline std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), result.data(), size);
    return result;
}
inline std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), result.data(), size, nullptr, nullptr);
    return result;
}
#endif
inline FILE* open(const char* path, const char* mode) {
#ifdef _WIN32
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return std::fopen(path, mode);
#endif
}
inline int open(FILE** result, const char* path, const char* mode) {
    *result = open(path, mode);
    return *result ? 0 : (errno ? errno : EIO);
}
inline int remove(const char* path) {
#ifdef _WIN32
    return _wremove(wide(path).c_str());
#else
    return std::remove(path);
#endif
}
inline bool replace(const std::string& temporary, const std::string& destination) {
#ifdef _WIN32
    return MoveFileExW(wide(temporary).c_str(), wide(destination).c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
}
inline std::string environment(const char* name) {
#ifdef _WIN32
    const wchar_t* value = _wgetenv(wide(name).c_str());
    return value ? utf8(value) : std::string();
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}
#ifdef _WIN32
inline std::string executableDirectory() {
    std::wstring path(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    if (!size || size >= path.size()) return ".";
    path.resize(size);
    return utf8(std::filesystem::path(path).parent_path().wstring());
}
inline DWORD attributes(const char* path) { return GetFileAttributesW(wide(path).c_str()); }
inline BOOL attributesEx(const char* path, GET_FILEEX_INFO_LEVELS level, void* out) {
    return GetFileAttributesExW(wide(path).c_str(), level, out);
}
inline BOOL copy(const char* from, const char* to, BOOL failIfExists) {
    return CopyFileW(wide(from).c_str(), wide(to).c_str(), failIfExists);
}
inline HINSTANCE shell(HWND owner, const char* verb, const char* file,
                       const char* args, const char* directory, int show) {
    const auto wverb = wide(verb ? verb : "");
    const auto wfile = wide(file ? file : "");
    const auto wargs = wide(args ? args : "");
    const auto wdir = wide(directory ? directory : "");
    return ShellExecuteW(owner, verb ? wverb.c_str() : nullptr, wfile.c_str(),
                         args ? wargs.c_str() : nullptr, directory ? wdir.c_str() : nullptr, show);
}
inline std::string chooseFile(const wchar_t* title, const wchar_t* filter, bool save = false,
                              const std::string& suggested = {}, const std::string& directory = {},
                              const wchar_t* extension = nullptr) {
    std::vector<wchar_t> path(32768, 0);
    const auto initial = wide(suggested);
    if (initial.size() >= path.size()) return {};
    std::copy(initial.begin(), initial.end(), path.begin());
    const auto dir = wide(directory);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = (DWORD)path.size();
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.nFilterIndex = 1;
    dialog.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    dialog.lpstrDefExt = extension;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER
                 | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return {};
    return utf8(path.data());
}
inline std::string chooseFolder(const wchar_t* title) {
    IFileOpenDialog* dialog = nullptr;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = utf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
#endif
} // namespace FileIO
