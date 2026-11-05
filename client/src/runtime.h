#pragma once
#include <windows.h>
#include <string>
#include "engine.h"

class UpdateLock {
    HANDLE handle_ = nullptr;
    bool owned_ = false;
public:
    explicit UpdateLock(std::wstring& error, DWORD timeout = 30000);
    ~UpdateLock();
    explicit operator bool() const { return owned_; }
    UpdateLock(const UpdateLock&) = delete;
    UpdateLock& operator=(const UpdateLock&) = delete;
};

std::wstring TargetPath(const Config& cfg, const std::string& relative);
std::string CurrentBootId();
bool IsTestData();
bool SecureUpdateData(std::wstring& error);
bool CreateUpdateDataDirectory(const std::wstring& path, std::wstring& error);
bool TrustedUpdateTree(const std::wstring& path, std::wstring& error);

struct VolumeIdentity {
    std::wstring root;
    DWORD disk = 0, partition = 0;
};
bool ReadVolumeIdentity(const std::wstring& path, VolumeIdentity& identity);
bool RecoveryBootVolume(const std::wstring& path, std::wstring& root);
