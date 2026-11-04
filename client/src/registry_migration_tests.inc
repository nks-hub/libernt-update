static bool RegistryMigrationSelfTest(const std::wstring& base, const std::wstring& native,
                                      const std::wstring& subkey, std::wstring& report) {
    bool ok = true;
    auto check = [&](bool pass, const std::wstring& name) {
        report += (pass ? L"PASS " : L"FAIL ") + name + L"\r\n";
        ok = ok && pass;
    };
    auto previousData = g_dataOverride;
    auto previousBoot = g_testBootId;
    g_dataOverride = JoinPath(base, L"registry-data");
    std::wstring sandbox = native + L"\\Migration", relative = subkey + L"\\Migration";
    SetRegistryTestRoot(sandbox);
    Config cfg{L"file:///" + JoinPath(base, L"registry-feed") + L"\\", JoinPath(base, L"registry-root")};
    cfg.registryBaseRelease = "G1";
    std::replace(cfg.feedUrl.begin(), cfg.feedUrl.end(), L'\\', L'/');
    auto value = [&](const std::wstring& keyName, const std::wstring& name, DWORD number) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (relative + L"\\" + keyName).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                            nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        bool written = RegSetValueExW(key, name.c_str(), 0, REG_DWORD, (BYTE*)&number, sizeof(number)) == ERROR_SUCCESS;
        RegCloseKey(key);
        return written;
    };
    for (const auto* hive : {L"SYSTEM\\CurrentControlSet", L"SOFTWARE", L"DEFAULT"}) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (relative + L"\\" + hive).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                            nullptr, &key, nullptr) == ERROR_SUCCESS) RegCloseKey(key);
        else ok = false;
    }
    HKEY commandsKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, relative.c_str(), 0, KEY_ALL_ACCESS, &commandsKey) == ERROR_SUCCESS) {
        std::wstring error;
        const std::wstring entry = L"luupboot \\??\\C:\\ExampleUpdate";
        const std::wstring other = L"luupboot \\??\\C:\\OtherUpdate";
        std::vector<std::wstring> early, late;
        bool saved = WriteBootCommands(commandsKey, L"BootExecute", {L"autocheck autochk *", other, entry}, error) &&
                     SetBootApplierCommands(commandsKey, true, entry, error) &&
                     ReadBootCommands(commandsKey, L"BootExecute", early, error) && ReadBootCommands(commandsKey, L"UpdateExecute", late, error);
        check(saved && early == std::vector<std::wstring>({L"autocheck autochk *", other}) && late == std::vector<std::wstring>({entry}),
              L"late boot registration preserves unrelated native commands");
        saved = SetBootApplierCommands(commandsKey, false, entry, error) && ReadBootCommands(commandsKey, L"BootExecute", early, error) &&
                ReadBootCommands(commandsKey, L"UpdateExecute", late, error);
        check(saved && late.empty() && early == std::vector<std::wstring>({L"autocheck autochk *", other, entry}),
              L"file-only systems retain the early native command");
        RegCloseKey(commandsKey);
    } else check(false, L"boot registration sandbox opens");
    auto raw = [](const json& document) { auto text = document.dump(); return std::vector<uint8_t>(text.begin(), text.end()); };
    json old{{"format", 1}, {"hives", {{"SYSTEM", {{"keys", {"CurrentControlSet\\Services"}}, {"values", json::array()}}},
             {"SOFTWARE", {{"keys", {"App", "Obsolete"}}, {"values", json::array({
                 {{"key", "App"}, {"name", "Setting"}, {"type", 4}, {"data", "01000000"}},
                 {{"key", "App"}, {"name", "Remove"}, {"type", 4}, {"data", "01000000"}},
                 {{"key", "Obsolete"}, {"name", "Value"}, {"type", 4}, {"data", "01000000"}}})}}},
             {"DEFAULT", {{"keys", json::array()}, {"values", json::array()}}}}}};
    json next = old;
    next["hives"]["SOFTWARE"]["values"] = json::array({
        {{"key", "App"}, {"name", "Setting"}, {"type", 4}, {"data", "02000000"}}});
    next["hives"]["SOFTWARE"]["keys"] = json::array({"App", "Classes", "Classes\\*"});
    next["hives"]["SYSTEM"]["keys"].push_back("CurrentControlSet\\Services\\Demo");
    next["hives"]["SYSTEM"]["keys"].push_back("CurrentControlSet\\Services\\Demo\\Parameters");
    next["hives"]["SYSTEM"]["values"].push_back({{"key", "CurrentControlSet\\Services\\Demo"}, {"name", "Start"}, {"type", 4}, {"data", "03000000"}});
    RegistrySnapshot parsedOld, parsedNext;
    std::wstring error;
    check(ParseRegistrySnapshot(raw(old), parsedOld, error) && ParseRegistrySnapshot(raw(next), parsedNext, error),
          L"registry snapshots accept service keys, empty keys and file association wildcards");
    auto duplicate = next;
    duplicate["hives"]["SOFTWARE"]["keys"].push_back("aPP");
    RegistrySnapshot invalid;
    check(!ParseRegistrySnapshot(raw(duplicate), invalid, error), L"registry snapshot rejects case-insensitive duplicate keys");
    duplicate = next;
    duplicate["hives"]["SOFTWARE"]["keys"].erase(1);
    check(!ParseRegistrySnapshot(raw(duplicate), invalid, error), L"registry snapshot requires explicit parent keys");
    for (auto& [keyName, item] : parsedOld.keys) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (relative + L"\\" + keyName).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                            nullptr, &key, nullptr) == ERROR_SUCCESS) RegCloseKey(key);
        else ok = false;
    }
    check(value(L"SOFTWARE\\App", L"Setting", 77) && value(L"SOFTWARE\\App", L"Remove", 1) &&
          value(L"SOFTWARE\\Obsolete", L"Value", 1) && value(L"SOFTWARE\\Obsolete", L"Local", 77),
          L"registry migration fixture contains local configuration");
    std::wstring feed = cfg.feedUrl.substr(8);
    std::replace(feed.begin(), feed.end(), L'/', L'\\');
    auto object = [&](const std::vector<uint8_t>& bytes) {
        auto hash = Sha256Hex(bytes.data(), bytes.size());
        WriteFileAtomic(JoinPath(feed, L"objects\\" + Widen(hash.substr(0, 2)) + L"\\" + Widen(hash)), bytes.data(), bytes.size());
        return hash;
    };
    std::vector<uint8_t> seed, secret(64), publicKey(32), signature(64);
    HexToBytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
    crypto_ed25519_key_pair(secret.data(), publicKey.data(), seed.data());
    auto hex = [](const auto& bytes) {
        std::string text;
        for (auto byte : bytes) { text += "0123456789abcdef"[byte >> 4]; text += "0123456789abcdef"[byte & 15]; }
        return text;
    };
    SetFeedTestPublicKey(hex(publicKey));
    auto release = [&](const char* id, unsigned sequence, const json& snapshot, const char* contents) {
        auto bytes = raw(snapshot);
        auto registryHash = object(bytes);
        std::string text(contents);
        std::vector<uint8_t> file(text.begin(), text.end());
        json manifest{{"id", id}, {"files", json::array({{{"path", "Windows/system32/demo.dll"}, {"size", file.size()}, {"sha256", object(file)}}})},
                      {"registry", {{"format", 1}, {"size", bytes.size()}, {"sha256", registryHash}}}};
        bytes = raw(manifest);
        WriteFileAtomic(JoinPath(feed, Widen(id) + L".json"), bytes.data(), bytes.size());
        return json{{"id", id}, {"commit", id}, {"date", "2026-01-01T00:00:00Z"}, {"sequence", sequence},
                    {"manifest", std::string(id) + ".json"}, {"manifestSha256", Sha256Hex(bytes.data(), bytes.size())},
                    {"registrySha256", registryHash}};
    };
    json feedJson{{"product", "LiberNT"}, {"format", 2}, {"releases", json::array({release("G1", 1, old, "before"), release("G2", 2, next, "after")})}};
    auto writeFeed = [&]() {
        auto bytes = raw(feedJson);
        if (!WriteFileAtomic(JoinPath(feed, L"feed-v2.json"), bytes.data(), bytes.size())) return false;
        crypto_ed25519_sign(signature.data(), secret.data(), bytes.data(), bytes.size());
        return WriteTextAtomic(JoinPath(feed, L"feed-v2.json.sig"), hex(signature));
    };
    writeFeed();
    auto file = JoinPath(cfg.root, L"Windows\\system32\\demo.dll");
    WriteTextAtomic(file, "before");
    SaveState(json{{"installed", "G1"}, {"pending", ""}});
    CheckResult result;
    Config unknown = cfg;
    unknown.registryBaseRelease.clear();
    check(!CheckForUpdate(unknown, result, error, nullptr), L"legacy file updates do not imply a registry baseline");
    check(CheckForUpdate(cfg, result, error, nullptr) && result.registryChanges.size() >= 7 && result.registryPreserved == 2,
          L"signed registry migration keeps custom settings and nonempty removed keys");
    auto hash = feedJson["releases"][0]["registrySha256"].get<std::string>();
    DeleteFileW(JoinPath(DataDir(), L"registry\\" + Widen(hash) + L".json").c_str());
    auto stored = JoinPath(feed, L"objects\\" + Widen(hash.substr(0, 2)) + L"\\" + Widen(hash));
    WriteTextAtomic(stored, "damaged");
    check(!CheckForUpdate(cfg, result, error, nullptr), L"registry snapshot hash is checked before any transaction");
    object(raw(old));
    check(CheckForUpdate(cfg, result, error, nullptr), L"verified registry snapshot can be fetched again");
    value(L"SOFTWARE\\App", L"Remove", 2);
    std::wstring message;
    check(InstallUpdate(cfg, result, message, nullptr) == InstallOutcome::Failed && Sha256File(file) == Sha256Hex("before", 6),
          L"registry change after the check refuses the whole installation");
    value(L"SOFTWARE\\App", L"Remove", 1);
    check(CheckForUpdate(cfg, result, error, nullptr) && InstallUpdate(cfg, result, message, nullptr) == InstallOutcome::PendingRestart,
          L"mixed file and registry changes are scheduled together");
    auto boot = [&]() {
        static unsigned index = 0;
        g_testBootId = "registry-boot-" + std::to_string(++index);
        return NT_SUCCESS(LuupBoot(NtPath(DataDir()).c_str(), FALSE, nullptr));
    };
    check(boot() && Sha256File(file) == Sha256Hex("after", 5), L"boot plan applies the signed file and registry migration");
    RegistryItem start{sandbox + L"\\SYSTEM\\CurrentControlSet\\Services\\Demo", L"Start"};
    check(ReadRegistryItem(start, error) && start.present && start.type == REG_DWORD && start.data == std::vector<uint8_t>({3, 0, 0, 0}),
          L"new service registry data is installed by the shared native backend");
    FinalizePending(cfg, message);
    check(boot() && FinalizePending(cfg, message) && LoadState().value("registryRelease", "") == "G2",
          L"registry baseline advances only after confirmation");
    check(RollbackLast(cfg, message, nullptr) == InstallOutcome::PendingRestart && boot(), L"manual rollback restores file and registry records together");
    FinalizePending(cfg, message);
    check(boot() && FinalizePending(cfg, message) && LoadState().value("registryRelease", "") == "G1" &&
          Sha256File(file) == Sha256Hex("before", 6), L"registry rollback confirms the original baseline and file");
    RegistryItem service{start.key, L"", true};
    RegistryItem custom{sandbox + L"\\SOFTWARE\\App", L"Setting"};
    RegistryItem local{sandbox + L"\\SOFTWARE\\Obsolete", L"Local"};
    check(ReadRegistryItem(service, error) && !service.present && ReadRegistryItem(custom, error) && custom.data == std::vector<uint8_t>({77, 0, 0, 0}) &&
          ReadRegistryItem(local, error) && local.present, L"rollback removes every new service key and preserves local configuration");
    bool installedAgain = CheckForUpdate(cfg, result, error, nullptr) && InstallUpdate(cfg, result, message, nullptr) == InstallOutcome::PendingRestart && boot();
    FinalizePending(cfg, message);
    installedAgain = installedAgain && boot() && FinalizePending(cfg, message);
    check(installedAgain, L"a previously rolled back registry release can be installed explicitly");
    value(L"SYSTEM\\CurrentControlSet\\Services\\Demo", L"Local", 77);
    WriteTextAtomic(file, "local file");
    bool restored = RollbackLast(cfg, message, nullptr) == InstallOutcome::PendingRestart && boot();
    FinalizePending(cfg, message);
    restored = restored && boot() && FinalizePending(cfg, message);
    RegistryItem later{start.key, L"Local"};
    check(restored && LoadState().value("registryRelease", "") == "G1" && ReadRegistryItem(later, error) && later.present &&
          ReadRegistryItem(start, error) && !start.present && Sha256File(file) == Sha256Hex("local file", 10),
          L"partial rollback keeps later registry values and records the restored registry baseline");
    for (auto& [keyName, item] : parsedNext.keys) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (relative + L"\\" + keyName).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                            nullptr, &key, nullptr) == ERROR_SUCCESS) RegCloseKey(key);
    }
    value(L"SYSTEM\\CurrentControlSet\\Services\\Demo", L"Start", 3);
    for (const auto* keyName : {L"SOFTWARE\\App", L"SOFTWARE\\Obsolete"}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (relative + L"\\" + keyName).c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, !wcscmp(keyName, L"SOFTWARE\\App") ? L"Remove" : L"Value");
            RegCloseKey(key);
        }
    }
    WriteTextAtomic(file, "after");
    cfg.baseRelease = "G1";
    bool already = CheckForUpdate(cfg, result, error, nullptr);
    check(already && !result.ChangeCount() && result.HasUpdate() && InstallUpdate(cfg, result, message, nullptr) == InstallOutcome::NothingToDo &&
          LoadState().value("registryRelease", "") == "G2", L"matching registry data advances its baseline without rewriting values");
    auto third = next;
    third["hives"]["SYSTEM"]["values"][0]["data"] = "04000000";
    feedJson["releases"].push_back(release("G3", 3, third, "after"));
    writeFeed();
    bool newest = CheckForUpdate(cfg, result, error, nullptr);
    check(newest && result.registryChanges.size() == 1 && result.registryChanges[0].after.data == std::vector<uint8_t>({4, 0, 0, 0}),
          L"the following release compares against the confirmed registry baseline");
    bool thirdInstalled = InstallUpdate(cfg, result, message, nullptr) == InstallOutcome::PendingRestart && boot();
    FinalizePending(cfg, message);
    thirdInstalled = thirdInstalled && boot() && FinalizePending(cfg, message);
    value(L"SYSTEM\\CurrentControlSet\\Services\\Demo", L"Start", 3);
    check(thirdInstalled && RollbackLast(cfg, message, nullptr) == InstallOutcome::NothingToDo &&
          LoadState().value("registryRelease", "") == "G2", L"already restored values confirm the rollback baseline without another boot");
    std::vector<uint8_t> modernFeed;
    auto modernPath = JoinPath(feed, L"feed-v2.json");
    ReadFileBytes(modernPath, modernFeed);
    DeleteFileW(modernPath.c_str());
    check(!CheckForUpdate(cfg, result, error, nullptr) && error.find(L"feed-v2.json") != std::wstring::npos,
          L"a client that accepted the registry protocol cannot fall back to the legacy feed");
    WriteFileAtomic(modernPath, modernFeed.data(), modernFeed.size());
    WriteTextAtomic(JoinPath(feed, L"feed-v2.json.sig"), std::string(128, '0'));
    check(!CheckForUpdate(cfg, result, error, nullptr), L"a damaged registry feed signature cannot fall back to the legacy feed");
    writeFeed();
    SetFeedTestPublicKey("");
    SetRegistryTestRoot(L"");
    g_dataOverride = previousData;
    g_testBootId = previousBoot;
    return ok;
}
