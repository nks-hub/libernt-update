#include "util.h"

#include <algorithm>
#include <cstdio>

#include "engine.h"
#include "sha256.h"

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string NowIso() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

std::string NowId() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1LL << 31);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD want = (DWORD)std::min<size_t>(out.size() - done, 1 << 20);
            ok = ReadFile(h, out.data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(h);
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const void* data, size_t len) {
    if (!EnsureDirs(path, true)) return false;
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = len == 0 || (WriteFile(h, data, (DWORD)len, &wrote, nullptr) && wrote == len);
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool WriteTextAtomic(const std::wstring& path, const std::string& text) {
    return WriteFileAtomic(path, text.data(), text.size());
}

std::string Sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    Sha256 s;
    // A full check hashes thousands of files. Reuse one buffer per worker instead of allocating 1 MiB per file.
    thread_local std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) != 0) && got) s.update(buf.data(), got);
    CloseHandle(h);
    return ok ? s.hex() : std::string();
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ParentDir(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

bool EnsureDirs(const std::wstring& path, bool isFile) {
    std::wstring dir = isFile ? ParentDir(path) : path;
    if (dir.empty() || dir.size() <= 3) return true;  // drive root
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!EnsureDirs(dir, true)) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}

std::wstring LastErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                   (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    wchar_t code[32];
    swprintf(code, 32, L" (%lu)", err);
    return s + code;
}

void LogLine(const std::string& line) {
    std::wstring path = JoinPath(DataDir(), L"update.log");
    EnsureDirs(path, true);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text = NowIso() + " " + line + "\r\n";
    DWORD wrote;
    WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
}
