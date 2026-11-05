static bool ChangeLoaderFixture(std::vector<uint8_t>& data) {
    if (data.size() < 0x80 || data[0] != 'M' || data[1] != 'Z') return false;
    DWORD pe = 0;
    memcpy(&pe, data.data() + 0x3c, sizeof pe);
    size_t checksum = (size_t)pe + 4 + sizeof(IMAGE_FILE_HEADER) + offsetof(IMAGE_OPTIONAL_HEADER64, CheckSum);
    if (pe <= 0x40 || checksum + sizeof(DWORD) > data.size() || memcmp(data.data() + pe, "PE\0\0", 4)) return false;
    data[0x40] ^= 1; // The DOS stub is not executed by UEFI; keep the PE checksum consistent with the test bytes.
    memset(data.data() + checksum, 0, sizeof(DWORD));
    DWORD sum = 0;
    for (size_t i = 0; i < data.size(); i += 2) {
        sum += data[i] | (i + 1 < data.size() ? (DWORD)data[i + 1] << 8 : 0);
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    sum += (DWORD)data.size();
    memcpy(data.data() + checksum, &sum, sizeof sum);
    return true;
}

static int UefiBootProbe(const std::wstring& mode) {
    g_dataOverride.clear();
    g_migrationData = L"C:\\ProgramData\\LiberNTUpdateUefiTest";
    Config cfg{L"file:///C:/ProgramData/LiberNTUpdateUefiTest/feed/", L"C:\\"};
    std::wstring error;
    if (!SecureUpdateData(error)) { std::cout << Narrow(error) << std::endl; return 1; }
    const wchar_t* manager = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    std::wstring metadata = JoinPath(DataDir(), L"probe.json");
    std::wstring dummy = L"C:\\Windows\\System32\\lntu-uefi-test.bin";
    json saved = LoadJson(metadata, json::object());
    auto saveObject = [&](const std::vector<uint8_t>& data) {
        std::string hash = Sha256Hex(data.data(), data.size());
        auto path = JoinPath(DataDir(), L"feed\\objects\\" + Widen(hash.substr(0, 2)) + L"\\" + Widen(hash));
        return WriteFileAtomic(path, data.data(), data.size()) ? hash : std::string();
    };
    if (mode == L"uefi-prepare") {
        if (!saved.empty() || !PendingTransaction().empty() || FileExists(dummy) || FileExists(FreeldrIni(cfg))) return 1;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, manager, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return 1;
        std::vector<std::wstring> early, late;
        DWORD bytes = 0;
        bool lateAbsent = RegQueryValueExW(key, L"UpdateExecute", nullptr, nullptr, nullptr, &bytes) == ERROR_FILE_NOT_FOUND;
        bool commandsOk = ReadBootCommands(key, L"BootExecute", early, error) && ReadBootCommands(key, L"UpdateExecute", late, error);
        RegCloseKey(key);
        if (!commandsOk) return 1;
        // A helper registered for another data folder stays installed after cleanup.
        saved = {{"early", early}, {"late", late}, {"lateAbsent", lateAbsent}, {"loaders", json::array()},
                 {"helper", Sha256File(L"C:\\luupboot.exe")}, {"helperKept", FileExists(L"C:\\Windows\\System32\\luupboot.exe")}};
        if (saved["helper"].get<std::string>().empty() || !WriteTextAtomic(dummy, "before") ||
            !SaveState(json{{"installed", "UefiBefore"}, {"pending", ""}})) return 1;
        CheckResult update;
        update.installed = "UefiBefore";
        update.latest.id = "UefiAfter";
        for (const char* relative : {"BootPartition/EFI/Boot/bootx64.efi", "BootPartition/EFI/LiberNT/Boot/bootmgfw.efi"}) {
            auto target = TargetPath(cfg, relative);
            std::vector<uint8_t> data;
            if (target.empty() || !ReadFileBytes(target, data)) return 1;
            std::string before = Sha256Hex(data.data(), data.size());
            if (!ChangeLoaderFixture(data)) return 1;
            std::string after = saveObject(data);
            if (after.empty() || after == before) return 1;
            update.changes.push_back({relative, after, data.size(), before});
            saved["loaders"].push_back({{"path", relative}, {"before", before}, {"after", after}});
        }
        std::vector<uint8_t> probe{'a', 'f', 't', 'e', 'r'};
        std::string hash = saveObject(probe);
        update.changes.push_back({"Windows/system32/lntu-uefi-test.bin", hash, probe.size(), Sha256File(dummy)});
        if (hash.empty() || !SaveJson(metadata, saved)) return 1;
        HANDLE lock = CreateFileW(dummy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (lock == INVALID_HANDLE_VALUE) return 1;
        auto outcome = InstallUpdate(cfg, update, error, nullptr);
        CloseHandle(lock);
        std::cout << Narrow(error) << "\n" << saved.dump() << std::endl;
        return outcome == InstallOutcome::PendingRestart ? 0 : 1;
    }
    if (saved.empty()) return 1;
    if (mode == L"uefi-rollback") {
        HANDLE lock = CreateFileW(dummy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (lock == INVALID_HANDLE_VALUE) return 1;
        auto outcome = RollbackLast(cfg, error, nullptr);
        CloseHandle(lock);
        std::cout << Narrow(error) << std::endl;
        return outcome == InstallOutcome::PendingRestart ? 0 : 1;
    }
    bool rollback = mode.find(L"rollback") != std::wstring::npos || mode == L"uefi-cleanup";
    bool first = mode == L"uefi-first" || mode == L"uefi-rollback-first";
    bool ok = PendingTransaction().empty() ? !first : FinalizePending(cfg, error);
    for (const auto& loader : saved["loaders"]) {
        std::string hash = Sha256File(TargetPath(cfg, loader["path"].get<std::string>()));
        bool same = hash == loader[rollback ? "before" : "after"].get<std::string>();
        std::cout << (same ? "PASS " : "FAIL ") << loader["path"].get<std::string>() << " " << hash << "\n";
        ok = ok && same;
    }
    std::string expectedDummy = rollback ? Sha256Hex("before", 6) : Sha256Hex("after", 5);
    bool dummyMatches = Sha256File(dummy) == expectedDummy;
    std::cout << (dummyMatches ? "PASS " : "FAIL ") << "OS partition probe " << Sha256File(dummy) << "\n";
    ok = ok && dummyMatches;
    ok = ok && (first ? !PendingTransaction().empty() && FileExists(BootFile(L"trial"))
                     : PendingTransaction().empty() && InstalledRelease() == (rollback ? "UefiBefore" : "UefiAfter"));
    std::cout << "installed=" << InstalledRelease() << " pending=" << PendingTransaction() << " boot=" << CurrentBootId()
              << " " << Narrow(error) << "\n";
    if (mode == L"uefi-cleanup" && ok) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, manager, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return 1;
        std::vector<std::wstring> early, late;
        ok = ReadBootCommands(key, L"BootExecute", early, error) && ReadBootCommands(key, L"UpdateExecute", late, error);
        std::wstring own = L"luupboot " + NtPath(DataDir());
        auto remove = [&](auto& commands) {
            commands.erase(std::remove_if(commands.begin(), commands.end(), [&](const auto& s) { return _wcsicmp(s.c_str(), own.c_str()) == 0; }), commands.end());
        };
        remove(early);
        remove(late);
        ok = ok && early == saved["early"].get<std::vector<std::wstring>>() && late == saved["late"].get<std::vector<std::wstring>>();
        if (ok) ok = WriteBootCommands(key, L"BootExecute", early, error) && WriteBootCommands(key, L"UpdateExecute", late, error);
        if (ok && saved.value("lateAbsent", false)) ok = RegDeleteValueW(key, L"UpdateExecute") == ERROR_SUCCESS && RegFlushKey(key) == ERROR_SUCCESS;
        RegCloseKey(key);
        ok = ok && Sha256File(L"C:\\Windows\\System32\\luupboot.exe") == saved["helper"].get<std::string>();
        if (ok && !saved.value("helperKept", false)) ok = DeleteFileW(L"C:\\Windows\\System32\\luupboot.exe");
        if (ok) ok = DeleteFileW(dummy.c_str());
    }
    std::cout << (ok ? "PASS " : "FAIL ") << Narrow(mode) << std::endl;
    return ok ? 0 : 1;
}
