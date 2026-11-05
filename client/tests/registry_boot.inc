static int RegistryBootProbe(const std::wstring& mode, const Config& cfg) {
    const wchar_t* sessionManager = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    const wchar_t* testKey = L"SOFTWARE\\LiberNTUpdateBootTest";
    auto savedBoot = JoinPath(ProbeBase(), L"bootexecute-original.bin");
    auto expectedBoot = JoinPath(ProbeBase(), L"bootexecute-test.bin");
    const std::wstring helper = L"C:\\Windows\\System32\\lnturegtest.exe";
    auto backupBoot = [&](const wchar_t* name = L"BootExecute") {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sessionManager, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return std::vector<uint8_t>{};
        DWORD size = 0, type = 0;
        std::vector<uint8_t> raw;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ && size >= 4) {
            raw.resize(size);
            if (RegQueryValueExW(key, name, nullptr, &type, raw.data(), &size) != ERROR_SUCCESS) raw.clear();
        }
        RegCloseKey(key);
        return raw;
    };
    auto writeBoot = [&](const std::vector<uint8_t>& raw) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sessionManager, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
        bool ok = RegSetValueExW(key, L"BootExecute", 0, REG_MULTI_SZ, raw.data(), (DWORD)raw.size()) == ERROR_SUCCESS &&
                  RegFlushKey(key) == ERROR_SUCCESS;
        RegCloseKey(key);
        return ok;
    };
    RegistryItem absent{L"\\Registry\\Machine\\" + std::wstring(testKey), L"", true, false};
    RegistryItem present = absent;
    present.present = true;
    RegistryItem missing{absent.key, L"Value", false, false};
    RegistryItem value{absent.key, L"Value", false, true, REG_DWORD, {1, 0, 0, 0}};
    auto file = TargetPath(cfg, "Windows/system32/probe.dll");
    std::wstring error;
    if (mode == L"smss-prepare") {
        if (!PendingTransaction().empty()) return 1;
        Config system = cfg;
        system.root = L"C:\\";
        std::wstring target = TargetPath(system, "Windows/system32/smss.exe");
        std::vector<uint8_t> candidate;
        if (!ReadFileBytes(JoinPath(ProbeBase(), L"smss-next.exe"), candidate) || !FileExists(target)) return 1;
        std::string hash = Sha256Hex(candidate.data(), candidate.size());
        if (hash == Sha256File(target)) return 1;
        auto object = JoinPath(ProbeBase(), L"feed\\objects\\" + Widen(hash.substr(0, 2)) + L"\\" + Widen(hash));
        if (!WriteFileAtomic(object, candidate.data(), candidate.size()) || !SaveState(json{{"installed", "SmssBefore"}, {"pending", ""}})) return 1;
        CheckResult update;
        update.installed = "SmssBefore";
        update.latest.id = "SmssBootstrap";
        update.changes = {{"Windows/system32/smss.exe", hash, candidate.size(), Sha256File(target)}};
        auto outcome = InstallUpdate(system, update, error, nullptr);
        std::cout << Narrow(error) << std::endl;
        return outcome == InstallOutcome::PendingRestart ? 0 : 1;
    }
    if (mode == L"smss-first" || mode == L"smss-second") {
        bool finalized = FinalizePending(cfg, error);
        bool first = mode == L"smss-first";
        bool correct = Sha256File(L"C:\\Windows\\System32\\smss.exe") == Sha256File(JoinPath(ProbeBase(), L"smss-next.exe"));
        bool ok = finalized && correct && (first ? !PendingTransaction().empty() : PendingTransaction().empty() && RegistryUpdatesSupported());
        std::cout << (ok ? "PASS " : "FAIL ") << Narrow(mode) << " capability=" << RegistryUpdatesSupported() << " " << Narrow(error) << std::endl;
        return ok ? 0 : 1;
    }
    if (mode == L"registry-prepare" || mode == L"registry-retry") {
        bool retry = mode == L"registry-retry";
        RegistryItem current = absent;
        if (!ReadRegistryItem(current, error) || current.present || (!retry && FileExists(savedBoot)) || !FileExists(helper) ||
            !PendingTransaction().empty()) return 1;
        auto original = backupBoot();
        std::vector<uint8_t> active = original;
        if (retry && (!RegistryUpdatesSupported() || !ReadFileBytes(savedBoot, original))) return 1;
        if (original.empty() || !WriteFileAtomic(savedBoot, original.data(), original.size())) return 1;
        std::wstring entries((const wchar_t*)original.data(), original.size() / sizeof(wchar_t));
        while (!entries.empty() && !entries.back()) entries.pop_back();
        entries.push_back(L'\0');
        entries += L"lnturegtest \\??\\C:\\lntu-review\\data";
        entries.push_back(L'\0'); entries.push_back(L'\0');
        if (!WriteFileAtomic(expectedBoot, entries.data(), entries.size() * sizeof(wchar_t)) ||
            !WriteTextAtomic(JoinPath(ProbeBase(), L"helper-sha256.txt"), Sha256File(helper))) return 1;
        if (!SaveState(json{{"installed", "ProbeBefore"}, {"registryRelease", "ProbeBefore"}, {"pending", ""}}) ||
            !WriteTextAtomic(file, "before")) return 1;
        PutObject(cfg, "after");
        CheckResult update;
        update.installed = update.registryBefore = "ProbeBefore";
        update.latest.id = update.registryAfter = "ProbeAfter";
        update.changes = {{"Windows/system32/probe.dll", Sha256Hex("after", 5), 5, Sha256Hex("before", 6)}};
        update.registryChanges = {{absent, present}, {missing, value}};
        auto outcome = InstallUpdate(cfg, update, error, nullptr);
        std::vector<uint8_t> desired;
        if (outcome != InstallOutcome::PendingRestart || !ReadFileBytes(expectedBoot, desired) || backupBoot() != active) return 1;
        if (retry) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sessionManager, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return 1;
            std::vector<std::wstring> early, late;
            bool registered = ReadBootCommands(key, L"BootExecute", early, error) && ReadBootCommands(key, L"UpdateExecute", late, error);
            std::wstring entry = L"lnturegtest \\??\\C:\\lntu-review\\data";
            if (registered) {
                DWORD bytes = 0;
                bool absent = RegQueryValueExW(key, L"UpdateExecute", nullptr, nullptr, nullptr, &bytes) == ERROR_FILE_NOT_FOUND;
                std::wstring blob;
                for (const auto& command : late) { blob += command; blob.push_back(L'\0'); }
                if (blob.empty()) blob.push_back(L'\0');
                blob.push_back(L'\0');
                registered = WriteFileAtomic(JoinPath(ProbeBase(), L"updateexecute-original.bin"), blob.data(), blob.size() * sizeof(wchar_t));
                if (absent) registered = registered && WriteTextAtomic(JoinPath(ProbeBase(), L"updateexecute-original.missing"), "missing");
                early.erase(std::remove(early.begin(), early.end(), entry), early.end());
                late.push_back(entry);
                registered = registered && WriteBootCommands(key, L"UpdateExecute", late, error) && WriteBootCommands(key, L"BootExecute", early, error);
                auto bootBytes = backupBoot(), updateBytes = backupBoot(L"UpdateExecute");
                registered = registered && WriteFileAtomic(expectedBoot, bootBytes.data(), bootBytes.size()) &&
                             WriteFileAtomic(JoinPath(ProbeBase(), L"updateexecute-test.bin"), updateBytes.data(), updateBytes.size());
            }
            RegCloseKey(key);
            if (!registered) return 1;
        } else if (!writeBoot(desired)) return 1;
        std::cout << "PASS registry plan registered for a real boot; " << Narrow(error) << std::endl;
        return 0;
    }
    if (mode == L"registry-rollback") {
        auto outcome = RollbackLast(cfg, error, nullptr);
        std::cout << Narrow(error) << std::endl;
        return outcome == InstallOutcome::PendingRestart ? 0 : 1;
    }
    if (mode == L"registry-cleanup") {
        if (!PendingTransaction().empty()) return 1;
        RegistryItem current = absent;
        std::vector<uint8_t> original, expected, helperHash;
        if (!ReadRegistryItem(current, error) || current.present || !ReadFileBytes(savedBoot, original) ||
            !ReadFileBytes(expectedBoot, expected) || backupBoot() != expected ||
            !ReadFileBytes(JoinPath(ProbeBase(), L"helper-sha256.txt"), helperHash) ||
            Sha256File(helper) != std::string(helperHash.begin(), helperHash.end())) return 1;
        std::vector<uint8_t> updateOriginal, updateExpected;
        if (ReadFileBytes(JoinPath(ProbeBase(), L"updateexecute-test.bin"), updateExpected)) {
            if (backupBoot(L"UpdateExecute") != updateExpected ||
                !ReadFileBytes(JoinPath(ProbeBase(), L"updateexecute-original.bin"), updateOriginal)) return 1;
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sessionManager, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return 1;
            LONG status = FileExists(JoinPath(ProbeBase(), L"updateexecute-original.missing"))
                            ? RegDeleteValueW(key, L"UpdateExecute")
                            : RegSetValueExW(key, L"UpdateExecute", 0, REG_MULTI_SZ, updateOriginal.data(), (DWORD)updateOriginal.size());
            if (status == ERROR_SUCCESS) status = RegFlushKey(key);
            RegCloseKey(key);
            if (status != ERROR_SUCCESS) return 1;
        }
        if (!writeBoot(original) || backupBoot() != original || !DeleteFileW(helper.c_str())) return 1;
        std::cout << "PASS original BootExecute restored exactly; test helper removed" << std::endl;
        return 0;
    }
    // luupboot restarts right after applying a plan, so the first boot the probe sees confirms the result.
    bool done = PendingTransaction().empty() || FinalizePending(cfg, error);
    bool installed = mode == L"registry-first" || mode == L"registry-second";
    bool undo = mode == L"registry-undo-first" || mode == L"registry-undo-second";
    RegistryItem actual = value;
    bool matched = ReadRegistryItem(actual, error) && (undo ? !actual.present : SameRegistryData(actual, value));
    matched = matched && Sha256File(file) == (undo ? Sha256Hex("before", 6) : Sha256Hex("after", 5));
    bool ok = done && matched && PendingTransaction().empty() &&
              LoadState().value("registryRelease", "") == (installed ? "ProbeAfter" : "ProbeBefore");
    std::cout << (ok ? "PASS " : "FAIL ") << Narrow(mode) << " boot=" << CurrentBootId() << " " << Narrow(error) << std::endl;
    return ok ? 0 : 1;
}
