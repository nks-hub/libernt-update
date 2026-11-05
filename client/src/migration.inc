namespace {
bool RenameMigrationDirectory(HANDLE directory, const std::wstring& destination, std::wstring& error) {
    std::vector<uint8_t> buffer(sizeof(FILE_RENAME_INFO) + destination.size() * sizeof(wchar_t));
    auto info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = (DWORD)(destination.size() * sizeof(wchar_t));
    memcpy(info->FileName, destination.c_str(), info->FileNameLength + sizeof(wchar_t));
    if (SetFileInformationByHandle(directory, FileRenameInfo, info, (DWORD)buffer.size())) return true;
    error = L"Cannot move the pinned update directory: " + LastErrorText(GetLastError());
    return false;
}

HANDLE OpenMigrationDirectory(const std::wstring& path, std::wstring& error) {
    HANDLE directory = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (directory != INVALID_HANDLE_VALUE && GetFileInformationByHandle(directory, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return directory;
    DWORD status = GetLastError();
    if (directory != INVALID_HANDLE_VALUE) { CloseHandle(directory); status = ERROR_INVALID_DATA; }
    error = L"Cannot pin a plain legacy data directory: " + LastErrorText(status);
    return INVALID_HANDLE_VALUE;
}

bool SwitchMigrationDirectories(HANDLE old, HANDLE replacement, const std::wstring& original,
                                const std::wstring& destination, std::wstring& archive, std::wstring& error) {
    if (!RenameMigrationDirectory(old, destination, error)) return false;
    archive = destination;
    if (RenameMigrationDirectory(replacement, original, error)) return true;
    std::wstring restoreError;
    if (RenameMigrationDirectory(old, original, restoreError)) archive.clear();
    else error += L". Legacy data remains at " + destination + L"; " + restoreError;
    return false;
}

bool StopMigrationService(const wchar_t* name, std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = scm ? OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG) : nullptr;
    DWORD status = GetLastError();
    if (!service) {
        if (scm) CloseServiceHandle(scm);
        if (status == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = L"Cannot open the updater service: " + LastErrorText(status);
        return false;
    }
    SERVICE_STATUS state{};
    bool ok = QueryServiceStatus(service, &state) != 0;
    if (ok && state.dwCurrentState != SERVICE_STOPPED && state.dwCurrentState != SERVICE_STOP_PENDING)
        ok = ControlService(service, SERVICE_CONTROL_STOP, &state) != 0;
    for (unsigned i = 0; ok && state.dwCurrentState != SERVICE_STOPPED && i < 120; ++i) {
        Sleep(250);
        ok = QueryServiceStatus(service, &state) != 0;
    }
    ok = ok && state.dwCurrentState == SERVICE_STOPPED;
    if (ok) ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != 0;
    if (!ok) error = L"Cannot stop and disable the legacy updater service: " + LastErrorText(GetLastError());
    CloseServiceHandle(service); CloseServiceHandle(scm);
    return ok;
}

bool LegacyBootStateCanBeArchived(const std::wstring& original, std::wstring& error) {
    auto boot = JoinPath(original, L"boot");
    HANDLE directory = CreateFileW(boot.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        DWORD status = GetLastError();
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
        error = L"Cannot inspect legacy boot recovery safely: " + LastErrorText(status);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(directory, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(directory);
        error = L"Cannot inspect legacy boot recovery safely: the boot directory is invalid or redirected.";
        return false;
    }
    for (const wchar_t* marker : {L"applying", L"trial", L"plan.done", L"rolledback", L"committed"}) {
        auto path = JoinPath(boot, marker);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CloseHandle(directory);
            error = L"An applied legacy update is unconfirmed. Finish or recover it before migration.";
            return false;
        }
        DWORD status = GetLastError();
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) {
            CloseHandle(directory);
            error = L"Cannot inspect legacy boot recovery safely: " + LastErrorText(status);
            return false;
        }
    }
    CloseHandle(directory);
    return true;
}
}

bool MigrateLegacyData(const Config& config, std::wstring& archive, std::wstring& error) {
    archive.clear(); error.clear();
    if (config.feedUrl.empty() || config.baseRelease.empty() || config.autoMode != "off") {
        error = L"Migration requires an explicit feed, original file baseline and automatic updates off.";
        return false;
    }
    if (!IsUserAnAdmin()) { error = L"Legacy migration requires an administrator token."; return false; }
    const auto original = DataDir();
    if (!LegacyBootStateCanBeArchived(original, error)) return false;
    const auto parent = original.substr(0, original.find_last_of(L'\\'));
    static LONG sequence = 0;
    auto container = JoinPath(parent, L"LiberNTUpdate-archive-" + Widen(NowId()) + L"-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(InterlockedIncrement(&sequence)));
    if (!CreateUpdateDataDirectory(container, error)) return false;
    auto stage = JoinPath(container, L"new-data");
    if (!CreateUpdateDataDirectory(stage, error)) return false;
    HANDLE old = OpenMigrationDirectory(original, error);
    if (old == INVALID_HANDLE_VALUE) return false;
    json settings{{"feedUrl", Narrow(config.feedUrl)}, {"root", Narrow(config.root)}, {"baseRelease", config.baseRelease},
                  {"auto", "off"}, {"restartHour", -1}};
    if (!config.registryBaseRelease.empty()) settings["registryBaseRelease"] = config.registryBaseRelease;
    if (!config.bootRoot.empty()) settings["bootRoot"] = Narrow(config.bootRoot);
    bool prepared = WriteTextAtomic(JoinPath(stage, L"config.json"), settings.dump(1));
    if (prepared) {
        g_migrationData = stage;
        CheckResult result;
        prepared = CheckForUpdate(config, result, error, nullptr);
        g_migrationData.clear();
    }
    if (prepared) prepared = TrustedUpdateTree(stage, error);
    if (!prepared) {
        if (error.empty()) error = L"Cannot prepare the new trusted update state.";
        CloseHandle(old);
        return false;
    }
    if (!IsTestData() && (!StopMigrationService(L"LiberNTUpdateGuard", error) || !StopMigrationService(L"LiberNTUpdate", error))) {
        CloseHandle(old);
        return false;
    }
    UpdateLock operation(error);
    if (!operation) { CloseHandle(old); return false; }
    if (!LegacyBootStateCanBeArchived(original, error)) { CloseHandle(old); return false; }
    HANDLE replacement = OpenMigrationDirectory(stage, error);
    if (replacement == INVALID_HANDLE_VALUE) { CloseHandle(old); return false; }
    auto destination = JoinPath(container, L"legacy-data");
    bool moved = SwitchMigrationDirectories(old, replacement, original, destination, archive, error);
    CloseHandle(replacement); CloseHandle(old);
    if (!moved) return false;
    if (!IsTestData()) RemoveRecoveryEntry(config);
    return TrustedUpdateTree(original, error);
}
