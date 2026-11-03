#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

/// ISO 8601 UTC timestamp, e.g. 2026-11-03T13:28:35Z.
std::string NowIso();
/// Sortable id from the current UTC time, e.g. 20261103-132835.
std::string NowId();

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out);
/// Writes through a temporary file and renames it into place, so a crash never leaves a torn file.
bool WriteFileAtomic(const std::wstring& path, const void* data, size_t len);
bool WriteTextAtomic(const std::wstring& path, const std::string& text);
/// SHA-256 of a file's contents, or empty when it cannot be read.
std::string Sha256File(const std::wstring& path);
bool FileExists(const std::wstring& path);
/// Creates every missing directory of path (not the last component when isFile).
bool EnsureDirs(const std::wstring& path, bool isFile);
std::wstring ParentDir(const std::wstring& path);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring LastErrorText(DWORD err);

/// Appends a line to %ProgramData%\LiberNTUpdate\update.log.
void LogLine(const std::string& line);
