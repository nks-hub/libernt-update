namespace {

class RecoveryPrivileges {
    struct PrivilegeState { DWORD count; LUID_AND_ATTRIBUTES values[2]; } previous_{};
    HANDLE token_ = nullptr;
    bool enabled_ = false, changed_ = false;
public:
    explicit RecoveryPrivileges(std::wstring& error) {
        PrivilegeState privileges{2};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token_) ||
            !LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &privileges.values[0].Luid) ||
            !LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &privileges.values[1].Luid)) {
            error = L"Cannot enable registry recovery privileges: " + LastErrorText(GetLastError());
            return;
        }
        for (auto& value : privileges.values) value.Attributes = SE_PRIVILEGE_ENABLED;
        DWORD bytes = 0;
        SetLastError(ERROR_SUCCESS);
        changed_ = AdjustTokenPrivileges(token_, FALSE, (TOKEN_PRIVILEGES*)&privileges, sizeof previous_,
                                         (TOKEN_PRIVILEGES*)&previous_, &bytes) != 0;
        enabled_ = changed_ && GetLastError() == ERROR_SUCCESS;
        if (!enabled_) error = L"Registry recovery privileges are unavailable.";
    }
    ~RecoveryPrivileges() {
        if (changed_) AdjustTokenPrivileges(token_, FALSE, (TOKEN_PRIVILEGES*)&previous_, 0, nullptr, nullptr);
        if (token_) CloseHandle(token_);
    }
    explicit operator bool() const { return enabled_; }
};

class OfflineHives {
    std::wstring root_, prefix_;
    std::map<std::wstring, std::wstring> mounted_;
public:
    explicit OfflineHives(const std::wstring& root) : root_(root),
        prefix_(L"LntuOffline-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount()) + L"-") {}
    ~OfflineHives() { std::wstring ignored; Unload(ignored); }
    std::wstring Mount(const std::wstring& hive, std::wstring& error) {
        auto existing = mounted_.find(hive);
        if (existing != mounted_.end()) return existing->second;
        auto file = JoinPath(root_, L"Windows\\System32\\config\\" + hive);
        std::string relative = "Windows/System32/config/" + Narrow(hive);
        if (!PlainRecoveryPath(root_, relative, true)) { error = L"Unreadable or redirected offline hive: " + file; return {}; }
        for (const char* suffix : {".LOG", ".LOG1", ".LOG2", ".ALT"}) {
            if (!PlainRecoveryPath(root_, relative + suffix)) { error = L"Unreadable or redirected offline hive log: " + file + Widen(suffix); return {}; }
        }
        auto name = prefix_ + hive;
        HKEY key = nullptr;
        LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, name.c_str(), 0, KEY_QUERY_VALUE, &key);
        if (key) RegCloseKey(key);
        if (status != ERROR_FILE_NOT_FOUND) { error = L"The temporary hive name is unavailable."; return {}; }
        status = RegLoadKeyW(HKEY_LOCAL_MACHINE, name.c_str(), file.c_str());
        if (status != ERROR_SUCCESS) {
            error = L"Cannot load offline hive " + hive + L": " + LastErrorText(status);
            if (status == ERROR_SHARING_VIOLATION || status == ERROR_LOCK_VIOLATION || status == ERROR_BUSY || status == ERROR_ALREADY_EXISTS)
                error += L". The hive is already in use. Finish another recovery operation, or restart the recovery OS if one was interrupted.";
            return {};
        }
        mounted_[hive] = name;
        return name;
    }
    bool Map(RegistryItem& item, std::wstring& error) {
        for (const auto& pair : {std::pair<const wchar_t*, const wchar_t*>{L"\\Registry\\Machine\\SYSTEM\\", L"SYSTEM"},
                                {L"\\Registry\\Machine\\SOFTWARE\\", L"SOFTWARE"}, {L"\\Registry\\User\\.DEFAULT\\", L"DEFAULT"}}) {
            size_t size = wcslen(pair.first);
            if (_wcsnicmp(item.key.c_str(), pair.first, size)) continue;
            auto tail = item.key.substr(size);
            if (tail.empty()) break;
            auto name = Mount(pair.second, error);
            if (name.empty()) return false;
            std::wstring relative = name;
            size_t at = 0;
            do {
                size_t slash = tail.find(L'\\', at);
                relative += L"\\" + tail.substr(at, slash == std::wstring::npos ? slash : slash - at);
                HKEY key = nullptr;
                LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, relative.c_str(), REG_OPTION_OPEN_LINK, KEY_QUERY_VALUE, &key);
                if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) break;
                if (status != ERROR_SUCCESS) { error = L"Cannot inspect an offline registry path."; return false; }
                DWORD type = 0;
                status = RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr);
                RegCloseKey(key);
                if ((status == ERROR_SUCCESS && type == REG_LINK) || (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)) {
                    error = L"Offline registry links are refused.";
                    return false;
                }
                if (slash == std::wstring::npos) break;
                at = slash + 1;
            } while (at < tail.size());
            item.key = L"\\Registry\\Machine\\" + name + L"\\" + tail;
            return true;
        }
        error = L"The recovery record does not belong to a supported offline hive.";
        return false;
    }
    bool DisableGuard(const std::string& tx, std::wstring& error) {
        auto system = Mount(L"SYSTEM", error);
        if (system.empty()) return false;
        DWORD current = 0, bytes = sizeof current;
        LONG status = RegGetValueW(HKEY_LOCAL_MACHINE, (system + L"\\Select").c_str(), L"Current",
                                   RRF_RT_REG_DWORD, nullptr, &current, &bytes);
        if (status != ERROR_SUCCESS || !current || current > 999) { error = L"Cannot identify the offline control set."; return false; }
        wchar_t control[32];
        swprintf(control, 32, L"\\ControlSet%03lu", current);
        RegistryItem image{L"\\Registry\\Machine\\SYSTEM" + std::wstring(control) + L"\\Services\\LiberNTUpdateGuard", L"ImagePath"};
        if (!Map(image, error) || !ReadRegistryItem(image, error)) return false;
        if (!image.present) return true;
        if ((image.type != REG_SZ && image.type != REG_EXPAND_SZ) || image.data.size() % sizeof(wchar_t)) {
            error = L"The offline update guard service has an invalid ImagePath.";
            return false;
        }
        std::wstring command((const wchar_t*)image.data.data(), image.data.size() / sizeof(wchar_t));
        while (!command.empty() && command.back() == L'\0') command.pop_back();
        auto suffix = L" /guard-service " + Widen(tx);
        if (command.size() < suffix.size() || command.substr(command.size() - suffix.size()) != suffix) return true;
        std::wstring keyPath = image.key.substr(wcslen(L"\\Registry\\Machine\\"));
        HKEY key = nullptr;
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_SET_VALUE, &key);
        DWORD disabled = SERVICE_DISABLED;
        if (status == ERROR_SUCCESS) status = RegSetValueExW(key, L"Start", 0, REG_DWORD, (const BYTE*)&disabled, sizeof disabled);
        if (status == ERROR_SUCCESS) status = RegFlushKey(key);
        if (key) RegCloseKey(key);
        if (status != ERROR_SUCCESS) error = L"Cannot disable the recovered transaction's offline guard: " + LastErrorText(status);
        return status == ERROR_SUCCESS;
    }
    bool Unload(std::wstring& error) {
        bool ok = true;
        for (auto it = mounted_.begin(); it != mounted_.end();) {
            HKEY key = nullptr;
            LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, it->second.c_str(), 0, KEY_QUERY_VALUE, &key);
            if (status == ERROR_SUCCESS) status = RegFlushKey(key);
            if (key) RegCloseKey(key);
            if (status == ERROR_SUCCESS) status = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, it->second.c_str());
            if (status != ERROR_SUCCESS) {
                error = L"Cannot unload " + it->second + L": " + LastErrorText(status);
                ok = false;
                ++it;
            } else it = mounted_.erase(it);
        }
        return ok;
    }
};

}
