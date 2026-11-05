namespace {

bool RecoveryHash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool PlainRecoveryPath(const std::wstring& root, const std::string& relative, bool mustExist = false) {
    if (!SafeManifestPath(relative)) return false;
    std::wstring path = root;
    size_t at = 0;
    do {
        size_t slash = relative.find('/', at);
        path = JoinPath(path, Widen(relative.substr(at, slash == std::string::npos ? slash : slash - at)));
        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            return !mustExist && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
        }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) || (slash != std::string::npos && !(attrs & FILE_ATTRIBUTE_DIRECTORY))) return false;
        if (slash == std::string::npos) return true;
        at = slash + 1;
    } while (at < relative.size());
    return false;
}

std::wstring RecoveryDataPath(std::wstring recorded, const std::string& dataRelative, const std::wstring& data) {
    if (recorded.rfind(L"\\\\?\\", 0) == 0 || recorded.rfind(L"\\??\\", 0) == 0) recorded.erase(0, 4);
    if (recorded.size() < 4 || !((recorded[0] >= L'A' && recorded[0] <= L'Z') || (recorded[0] >= L'a' && recorded[0] <= L'z')) ||
        recorded[1] != L':' || recorded[2] != L'\\') return {};
    std::wstring relative = Widen(dataRelative);
    std::replace(relative.begin(), relative.end(), L'/', L'\\');
    relative += L"\\";
    if (_wcsnicmp(recorded.c_str() + 3, relative.c_str(), relative.size()) != 0) return {};
    std::wstring tail = recorded.substr(3 + relative.size());
    if (_wcsnicmp(tail.c_str(), L"tx\\", 3)) return {};
    std::string safe = Narrow(tail);
    std::replace(safe.begin(), safe.end(), '\\', '/');
    if (!SafeManifestPath(safe) || !PlainRecoveryPath(data, safe, true)) return {};
    return JoinPath(data, tail);
}

bool RecoveryTarget(Config& cfg, std::wstring& error) {
    VolumeIdentity target, running;
    wchar_t windows[MAX_PATH]{}, mount[MAX_PATH]{};
    if (cfg.root.empty() || !GetVolumePathNameW(cfg.root.c_str(), mount, MAX_PATH) || _wcsicmp(cfg.root.c_str(), mount) ||
        !ReadVolumeIdentity(cfg.root, target) || !GetWindowsDirectoryW(windows, MAX_PATH) || !ReadVolumeIdentity(windows, running)) {
        error = L"Recovery requires an explicit readable volume root.";
        return false;
    }
    if (target.disk == running.disk && target.partition == running.partition) {
        error = L"Offline recovery refuses the running system volume.";
        return false;
    }
    cfg.root = target.root;
    if (!cfg.bootRoot.empty()) {
        std::wstring boot;
        VolumeIdentity bootIdentity;
        if (!GetVolumePathNameW(cfg.bootRoot.c_str(), mount, MAX_PATH) || _wcsicmp(cfg.bootRoot.c_str(), mount) ||
            !ReadVolumeIdentity(cfg.bootRoot, bootIdentity)) { error = L"Pass an explicit boot volume root."; return false; }
        if (bootIdentity.disk != target.disk) { error = L"Offline recovery requires the boot partition on the selected OS disk."; return false; }
        if (bootIdentity.disk == target.disk && bootIdentity.partition == target.partition) boot = target.root;
        else if (!RecoveryBootVolume(cfg.bootRoot, boot)) {
            error = L"The recovery boot target must be an ESP outside the running recovery system.";
            return false;
        }
        cfg.bootRoot = boot;
    }
    return true;
}

class OfflineDataScope {
    std::wstring previous_ = g_migrationData;
    bool wasOffline_ = g_offlineRecovery;
public:
    explicit OfflineDataScope(const std::wstring& data) { g_migrationData = data; g_offlineRecovery = true; }
    ~OfflineDataScope() { g_migrationData = previous_; g_offlineRecovery = wasOffline_; }
};

}
