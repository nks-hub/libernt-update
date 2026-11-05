#include "runtime.h"
#include <winternl.h>
#include <sddl.h>
#include <aclapi.h>
#include <algorithm>
#include "sha256.h"
#include "util.h"

UpdateLock::UpdateLock(std::wstring& error, DWORD timeout) {
    std::string name = Narrow(DataDir());
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)tolower(c); });
    name = (IsTestData() ? "Local\\LiberNTUpdate-" : "Global\\LiberNTUpdate-") + Sha256Hex(name.data(), name.size()).substr(0, 24);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
        error = L"Update lock: " + LastErrorText(GetLastError());
        return;
    }
    SECURITY_ATTRIBUTES attrs{sizeof attrs, sd, FALSE};
    handle_ = CreateMutexW(IsTestData() ? nullptr : &attrs, FALSE, Widen(name).c_str());
    LocalFree(sd);
    DWORD wait = handle_ ? WaitForSingleObject(handle_, timeout) : WAIT_FAILED;
    owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (!owned_) error = Tr(L"Jiná aktualizační operace právě běží nebo k ní nemáte oprávnění.",
                           L"Another update operation is running, or access was denied.");
}

UpdateLock::~UpdateLock() {
    if (owned_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
}

std::string CurrentBootId() {
    struct TimeOfDay { LARGE_INTEGER boot, current, bias; ULONG zone, reserved; ULONGLONG bootBias, sleepBias; } info{};
    if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)3, &info, sizeof info, nullptr) < 0) return {};
    return std::to_string(info.boot.QuadPart);
}

std::wstring TargetPath(const Config& cfg, const std::string& relative) {
    std::wstring path = Widen(relative);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    const std::wstring prefix = L"BootPartition\\";
    if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return JoinPath(cfg.root, path);
    if (!cfg.bootRoot.empty()) return JoinPath(cfg.bootRoot, path.substr(prefix.size()));
    if (IsTestData()) return JoinPath(cfg.root, path.substr(prefix.size()));

    wchar_t partition[512]{};
    DWORD bytes = sizeof partition;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"SystemPartition", RRF_RT_REG_SZ, nullptr, partition, &bytes);
    wchar_t volume[MAX_PATH];
    HANDLE find = FindFirstVolumeW(volume, MAX_PATH);
    bool haveEfi = false;
    std::wstring chosen;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!FileExists(std::wstring(volume) + L"EFI\\LiberNT\\Boot\\bootmgfw.efi")) continue;
            haveEfi = true;
            std::wstring deviceName = volume + 4;
            if (!deviceName.empty() && deviceName.back() == L'\\') deviceName.pop_back();
            wchar_t device[512]{};
            if (*partition && QueryDosDeviceW(deviceName.c_str(), device, 512) && _wcsicmp(device, partition) == 0)
                chosen = volume;
        } while (FindNextVolumeW(find, volume, MAX_PATH));
        FindVolumeClose(find);
    }
    if (!chosen.empty()) return JoinPath(chosen, path.substr(prefix.size()));
    // A removable clone can contain the same loader. Require an explicit partition when Setup cannot identify it.
    using FirmwareQuery = BOOL(WINAPI*)(DWORD*);
    auto queryFirmware = (FirmwareQuery)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareType");
    DWORD firmware = 0;
    if (haveEfi || (queryFirmware && queryFirmware(&firmware) && firmware == 2)) return {};
    return JoinPath(cfg.root, path.substr(prefix.size()));
}

bool TrustedUpdateTree(const std::wstring& path, std::wstring& reason) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) { reason = L"Unreadable or redirected update path: " + path; return false; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    PSID owner = nullptr;
    PACL acl = nullptr;
    DWORD status = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                         OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &sd);
    bool trusted = status == ERROR_SUCCESS && acl &&
                   (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
    if (!trusted) reason = L"Untrusted owner or missing access rules: " + path;
    for (DWORD i = 0; trusted && i < acl->AceCount; i++) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) { trusted = false; break; }
        auto header = (ACE_HEADER*)entry;
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto ace = (ACCESS_ALLOWED_ACE*)entry;
        PSID sid = &ace->SidStart;
        DWORD writes = GENERIC_ALL | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA |
                       FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD;
        if ((ace->Mask & writes) && !IsWellKnownSid(sid, WinLocalSystemSid) && !IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
            trusted = false;
            reason = L"Untrusted write permission: " + path;
        }
    }
    if (sd) LocalFree(sd);
    if (!trusted || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return trusted;
    WIN32_FIND_DATAW item;
    HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &item);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L"..")) continue;
        if (!TrustedUpdateTree(JoinPath(path, item.cFileName), reason)) { trusted = false; break; }
    } while (FindNextFileW(find, &item));
    FindClose(find);
    return trusted;
}

static const wchar_t kUpdateDataSecurity[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)";

bool CreateUpdateDataDirectory(const std::wstring& path, std::wstring& error) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kUpdateDataSecurity, SDDL_REVISION_1, &sd, nullptr)) {
        error = L"Update data security: " + LastErrorText(GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof attributes, sd, FALSE};
    bool ok = CreateDirectoryW(path.c_str(), &attributes) != 0;
    if (!ok) error = L"Cannot create a protected update directory: " + LastErrorText(GetLastError());
    LocalFree(sd);
    return ok;
}

bool SecureUpdateData(std::wstring& error) {
    if (IsTestData()) return true;
    UpdateLock operation(error);
    if (!operation) return false;
    std::wstring dir = DataDir();
    static std::wstring securedDirectory;
    if (securedDirectory == dir) return true;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES && !CreateUpdateDataDirectory(dir, error)) return false;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kUpdateDataSecurity, SDDL_REVISION_1, &sd, nullptr)) return false;
    if (!TrustedUpdateTree(dir, error)) {
        error += L". Recovery files were not used.";
        LocalFree(sd);
        return false;
    }
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted);
    DWORD status = SetNamedSecurityInfoW(&dir[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, acl, nullptr);
    LocalFree(sd);
    if (status != ERROR_SUCCESS) error = L"Update data permissions: " + LastErrorText(status);
    else securedDirectory = dir;
    return status == ERROR_SUCCESS;
}
