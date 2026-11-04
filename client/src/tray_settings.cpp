#include "tray.h"
#include "util.h"
#include "engine.h"
#include <cwctype>
#include <cstdint>

namespace {
HKEY OwnTraySettings(REGSAM access, std::wstring& error) {
    wchar_t executable[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, executable, _countof(executable));
    if (!length || length >= _countof(executable)) return nullptr;
    // LiberNT Explorer's TrayGetIconSettingsKey hashes WCHARs, then the four UID bytes.
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (DWORD i = 0; i < length; i++) { hash ^= (wchar_t)towlower(executable[i]); hash *= 0x100000001b3ULL; }
    UINT uid = kTrayIconId;
    for (unsigned i = 0; i < sizeof(uid); i++) { hash ^= ((const uint8_t*)&uid)[i]; hash *= 0x100000001b3ULL; }
    wchar_t path[128];
    swprintf_s(path, L"Control Panel\\NotifyIconSettings\\%08lX%08lX", (DWORD)(hash >> 32), (DWORD)hash);
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, access | KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS) {
        wchar_t recorded[MAX_PATH]{};
        DWORD bytes = sizeof(recorded), recordedUid = 0, uidBytes = sizeof(recordedUid);
        if (RegGetValueW(key, nullptr, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, recorded, &bytes) == ERROR_SUCCESS &&
            RegGetValueW(key, nullptr, L"UID", RRF_RT_REG_DWORD, nullptr, &recordedUid, &uidBytes) == ERROR_SUCCESS &&
            recordedUid == uid && !_wcsicmp(recorded, executable)) return key;
        RegCloseKey(key);
        status = ERROR_INVALID_DATA;
    }
    error = Tr(L"Nelze přečíst nastavení této ikony: ", L"Cannot read this icon's settings: ") + LastErrorText(status);
    return nullptr;
}
}

bool ReadTrayPin(bool& pinned, std::wstring& error) {
    pinned = false;
    HKEY key = OwnTraySettings(KEY_QUERY_VALUE, error);
    if (!key) return false;
    DWORD value = 0, bytes = sizeof(value);
    LONG status = RegGetValueW(key, nullptr, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) { pinned = value != 0; return true; }
    error = Tr(L"Nelze přečíst volbu připnutí: ", L"Cannot read the pin preference: ") + LastErrorText(status);
    return false;
}

bool SetTrayPin(bool pinned, std::wstring& error) {
    HKEY key = OwnTraySettings(KEY_SET_VALUE, error);
    if (!key) return false;
    DWORD value = pinned ? 1 : 0;
    LONG status = RegSetValueExW(key, L"IsPromoted", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) return true;
    error = Tr(L"Nelze uložit volbu připnutí: ", L"Cannot save the pin preference: ") + LastErrorText(status);
    return false;
}
