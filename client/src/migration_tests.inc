#include <aclapi.h>
#include <sddl.h>

static bool MigrationUsersHaveReadOnlyAccess(const std::wstring& path) {
    HANDLE source = nullptr, restricted = nullptr, token = nullptr;
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sidBytes = sizeof(sid);
    SID_AND_ATTRIBUTES users{sid, 0};
    bool ok = CreateWellKnownSid(WinBuiltinUsersSid, nullptr, sid, &sidBytes) &&
              OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &source) &&
              CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &users, &restricted) &&
              DuplicateTokenEx(restricted, TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation, &token);
    PSECURITY_DESCRIPTOR security = nullptr;
    ok = ok && GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                     OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, nullptr, nullptr, &security) == ERROR_SUCCESS;
    auto access = [&](DWORD desired, BOOL& allowed) {
        GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        BYTE privileges[512]{};
        DWORD size = sizeof(privileges), granted = 0;
        return AccessCheck(security, token, desired, &mapping, reinterpret_cast<PRIVILEGE_SET*>(privileges), &size, &granted, &allowed) != 0;
    };
    BOOL read = FALSE, write = FALSE;
    ok = ok && access(FILE_LIST_DIRECTORY, read) && access(FILE_ADD_FILE, write) && read && !write;
    if (security) LocalFree(security);
    if (token) CloseHandle(token);
    if (restricted) CloseHandle(restricted);
    if (source) CloseHandle(source);
    return ok;
}

static bool MigrationSelfTest(const std::wstring& base, std::wstring& report) {
    bool ok = true;
    auto check = [&](bool condition, const wchar_t* text) {
        report += (condition ? L"PASS " : L"FAIL ") + std::wstring(text) + L"\r\n";
        ok = ok && condition;
    };
    const auto previous = g_dataOverride;
    g_dataOverride = JoinPath(base, L"legacy-data");
    EnsureDirs(g_dataOverride, false);
    const auto original = g_dataOverride;
    WriteTextAtomic(JoinPath(original, L"boot\\plan.txt"), "untrusted pending plan");
    WriteTextAtomic(JoinPath(original, L"state.json"), "untrusted state");
    WriteTextAtomic(JoinPath(original, L"history.json"), "legacy history");
    const auto planHash = Sha256File(JoinPath(original, L"boot\\plan.txt"));
    const auto historyHash = Sha256File(JoinPath(original, L"history.json"));
    const auto stateHash = Sha256File(JoinPath(original, L"state.json"));
    const auto feed = JoinPath(base, L"migration-feed");
    json manifest{{"id", "MigrationBase"}, {"files", json::array()}};
    const auto rawManifest = manifest.dump();
    WriteTextAtomic(JoinPath(feed, L"manifest.json"), rawManifest);
    const auto rawFeed = json{{"format", 1}, {"releases", json::array({
        {{"id", "MigrationBase"}, {"sequence", 1u}, {"manifest", "manifest.json"},
         {"manifestSha256", Sha256Hex(rawManifest.data(), rawManifest.size())}}})}}.dump();
    std::vector<uint8_t> seed, secret(64), key(32), signature(64);
    HexToBytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
    crypto_ed25519_key_pair(secret.data(), key.data(), seed.data());
    crypto_ed25519_sign(signature.data(), secret.data(), (const uint8_t*)rawFeed.data(), rawFeed.size());
    auto hex = [](const auto& bytes) {
        std::string text;
        for (auto byte : bytes) { text += "0123456789abcdef"[byte >> 4]; text += "0123456789abcdef"[byte & 15]; }
        return text;
    };
    SetFeedTestPublicKey(hex(key));
    WriteTextAtomic(JoinPath(feed, L"feed.json"), rawFeed);
    WriteTextAtomic(JoinPath(feed, L"feed.json.sig"), hex(signature));
    Config config{L"file:///" + feed + L"\\", JoinPath(base, L"migration-root")};
    std::replace(config.feedUrl.begin(), config.feedUrl.end(), L'\\', L'/');
    config.autoMode = "off";
    config.baseRelease = "MigrationBase";
    std::wstring archive, error;
    if (!IsUserAnAdmin()) {
        check(!MigrateLegacyData(config, archive, error) && error.find(L"administrator token") != std::wstring::npos && archive.empty() &&
                  Sha256File(JoinPath(original, L"boot\\plan.txt")) == planHash, L"a non-administrator cannot migrate legacy update data");
    } else {
        WriteTextAtomic(JoinPath(original, L"boot\\trial"), "1");
        check(!MigrateLegacyData(config, archive, error) && error.find(L"unconfirmed") != std::wstring::npos && archive.empty() &&
                  Sha256File(JoinPath(original, L"boot\\plan.txt")) == planHash,
              L"migration refuses to discard recovery for an applied unconfirmed update");
        DeleteFileW(JoinPath(original, L"boot\\trial").c_str());
        auto trial = JoinPath(original, L"boot\\trial");
        bool markerDirectory = CreateDirectoryW(trial.c_str(), nullptr) != 0;
        check(markerDirectory && !MigrateLegacyData(config, archive, error) && error.find(L"unconfirmed") != std::wstring::npos && archive.empty(),
              L"migration refuses a directory in place of a recovery marker");
        if (markerDirectory) RemoveDirectoryW(trial.c_str());
        const auto deniedRoot = JoinPath(base, L"migration-denied-source");
        const auto deniedBoot = JoinPath(deniedRoot, L"boot");
        bool seeded = WriteTextAtomic(JoinPath(deniedRoot, L"state.json"), "preserved legacy state");
        PSECURITY_DESCRIPTOR denied = nullptr;
        bool descriptor = ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(D;;0x1;;;WD)(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &denied, nullptr) != 0;
        SECURITY_ATTRIBUTES deniedAttributes{sizeof deniedAttributes, denied, FALSE};
        bool hidden = seeded && descriptor && CreateDirectoryW(deniedBoot.c_str(), &deniedAttributes);
        g_dataOverride = deniedRoot;
        bool refused = hidden && !MigrateLegacyData(config, archive, error) &&
                       error.find(L"Cannot inspect legacy boot recovery") != std::wstring::npos &&
                       error.find(L"(5)") != std::wstring::npos && archive.empty();
        g_dataOverride = original;
        bool removed = !hidden || RemoveDirectoryW(deniedBoot.c_str());
        if (denied) LocalFree(denied);
        check(refused && removed && Sha256File(JoinPath(deniedRoot, L"state.json")) == Sha256Hex("preserved legacy state", 22),
              L"migration refuses unreadable recovery markers without moving data");
        config.baseRelease = "UnknownBase";
        check(!MigrateLegacyData(config, archive, error) && error.find(L"baseRelease") != std::wstring::npos && archive.empty() &&
                  Sha256File(JoinPath(original, L"boot\\plan.txt")) == planHash,
              L"migration refuses an unknown signed baseline without moving legacy data");
        config.baseRelease = "MigrationBase";
        WriteTextAtomic(JoinPath(feed, L"feed.json"), rawFeed + " ");
        check(!MigrateLegacyData(config, archive, error) && error.find(Tr(L"Podpis", L"The signature")) != std::wstring::npos &&
                  archive.empty() && FileExists(JoinPath(original, L"history.json")),
              L"migration refuses a damaged feed before archiving");
        WriteTextAtomic(JoinPath(feed, L"feed.json"), rawFeed);
        HANDLE held = CreateFileW(original.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        check(held != INVALID_HANDLE_VALUE && !MigrateLegacyData(config, archive, error) && archive.empty() &&
                  error.find(L"Cannot pin a plain legacy") != std::wstring::npos &&
                  Sha256File(JoinPath(original, L"boot\\plan.txt")) == planHash,
              L"migration refuses a directory that cannot be pinned for rename");
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        bool migrated = MigrateLegacyData(config, archive, error);
        if (!migrated) report += L"Migration error: " + error + L"; archive=" + archive + L"\r\n";
        check(migrated && !archive.empty() && Sha256File(JoinPath(archive, L"boot\\plan.txt")) == planHash &&
                  Sha256File(JoinPath(archive, L"history.json")) == historyHash && Sha256File(JoinPath(archive, L"state.json")) == stateHash,
              L"migration archives the original plans and history without changing their bytes");
        check(migrated && !FileExists(JoinPath(original, L"boot\\plan.txt")) && !FileExists(JoinPath(original, L"history.json")) &&
                  LoadState().value("pending", "").empty() && LoadState().value("installed", "").empty(),
              L"migration does not adopt untrusted plans or claim an installed release");
        check(migrated && LoadConfig().autoMode == "off" && LoadConfig().baseRelease == "MigrationBase" &&
                  LoadConfig().feedUrl == config.feedUrl && TrustedUpdateTree(original, error),
              L"migration creates a protected state with explicitly verified settings");
        check(migrated && MigrationUsersHaveReadOnlyAccess(original), L"migrated data grants Users read access without permission to add files");
    }
    const auto recoverySource = JoinPath(base, L"migration-rename-source");
    const auto recoveryStage = JoinPath(base, L"migration-rename-stage");
    const auto recoveryArchive = JoinPath(base, L"migration-rename-archive");
    WriteTextAtomic(JoinPath(recoverySource, L"preserved.txt"), "original data");
    EnsureDirs(recoveryStage, false);
    HANDLE old = OpenMigrationDirectory(recoverySource, error);
    HANDLE replacement = CreateFileW(recoveryStage.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    archive.clear();
    bool restored = old != INVALID_HANDLE_VALUE && replacement != INVALID_HANDLE_VALUE &&
        !SwitchMigrationDirectories(old, replacement, recoverySource, recoveryArchive, archive, error);
    check(restored && archive.empty() && Sha256File(JoinPath(recoverySource, L"preserved.txt")) == Sha256Hex("original data", 13),
          L"failed replacement rename restores the original data directory");
    if (replacement != INVALID_HANDLE_VALUE) CloseHandle(replacement);
    const auto occupied = JoinPath(base, L"migration-occupied-target");
    WriteTextAtomic(JoinPath(occupied, L"unrelated.txt"), "unrelated data");
    replacement = OpenMigrationDirectory(recoveryStage, error);
    bool retained = old != INVALID_HANDLE_VALUE && replacement != INVALID_HANDLE_VALUE &&
        !SwitchMigrationDirectories(old, replacement, occupied, recoveryArchive, archive, error);
    check(retained && archive == recoveryArchive && Sha256File(JoinPath(recoveryArchive, L"preserved.txt")) == Sha256Hex("original data", 13) &&
              Sha256File(JoinPath(occupied, L"unrelated.txt")) == Sha256Hex("unrelated data", 14),
          L"an occupied rollback target is preserved and the legacy archive remains available");
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    if (replacement != INVALID_HANDLE_VALUE) CloseHandle(replacement);
    SetFeedTestPublicKey("");
    g_dataOverride = previous;
    return ok;
}
