bool ReadBootCommands(HKEY key, const wchar_t* name, std::vector<std::wstring>& commands, std::wstring& error) {
    DWORD bytes = 0, type = 0;
    LONG status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    commands.clear();
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes > 65536 || bytes % sizeof(wchar_t)) {
        error = L"Cannot read " + std::wstring(name) + L" safely.";
        return false;
    }
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 2, 0);
    status = RegQueryValueExW(key, name, nullptr, &type, (BYTE*)data.data(), &bytes);
    if (status != ERROR_SUCCESS || type != REG_MULTI_SZ) { error = L"Cannot read " + std::wstring(name) + L"."; return false; }
    for (const wchar_t* item = data.data(); *item; item += wcslen(item) + 1) commands.emplace_back(item);
    return true;
}

bool WriteBootCommands(HKEY key, const wchar_t* name, const std::vector<std::wstring>& commands, std::wstring& error) {
    std::wstring blob;
    for (const auto& command : commands) { blob += command; blob.push_back(L'\0'); }
    if (blob.empty()) blob.push_back(L'\0');
    blob.push_back(L'\0');
    LONG status = RegSetValueExW(key, name, 0, REG_MULTI_SZ, (const BYTE*)blob.data(), (DWORD)(blob.size() * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    if (status == ERROR_SUCCESS) return true;
    error = L"Cannot save " + std::wstring(name) + L": " + LastErrorText(status);
    return false;
}

bool SetBootApplierCommands(HKEY key, bool ready, const std::wstring& entry, std::wstring& error) {
    std::vector<std::wstring> early, late;
    if (!ReadBootCommands(key, L"BootExecute", early, error) || !ReadBootCommands(key, L"UpdateExecute", late, error)) return false;
    auto originalEarly = early, originalLate = late;
    auto own = [&](const auto& command) { return _wcsicmp(command.c_str(), entry.c_str()) == 0; };
    early.erase(std::remove_if(early.begin(), early.end(), own), early.end());
    late.erase(std::remove_if(late.begin(), late.end(), own), late.end());
    (ready ? late : early).push_back(entry);
    // Plans are armed only after both command lists are durable.
    return ready ? (late == originalLate || WriteBootCommands(key, L"UpdateExecute", late, error)) &&
                   (early == originalEarly || WriteBootCommands(key, L"BootExecute", early, error))
                 : (early == originalEarly || WriteBootCommands(key, L"BootExecute", early, error)) &&
                   (late == originalLate || WriteBootCommands(key, L"UpdateExecute", late, error));
}

bool EnsureBootApplier(std::wstring& error) {
    wchar_t exe[MAX_PATH], sys[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring source = JoinPath(std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\')), L"luupboot.exe");
    std::wstring target = JoinPath(sys, L"luupboot.exe");
    if (!FileExists(source)) { error = source + Tr(L" chybí.", L" is missing."); return false; }
    if (Sha256File(source) != Sha256File(target) && !CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        error = Tr(L"Nelze nainstalovat ", L"Cannot install ") + target + L": " + LastErrorText(GetLastError());
        return false;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSessionManager, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        error = Tr(L"Nelze otevřít Session Manager v registru.", L"Cannot open Session Manager in the registry.");
        return false;
    }
    bool ok = SetBootApplierCommands(key, RegistryUpdatesSupported(), L"luupboot " + NtPath(DataDir()), error);
    RegCloseKey(key);
    return ok;
}

