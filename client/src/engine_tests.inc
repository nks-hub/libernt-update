// Exercises private transaction recovery against an isolated directory and simulated boot identities.
#include <memory>
#include <thread>
#include <sddl.h>
#include "registry_tests.inc"
#include "migration_tests.inc"
#include "offline_tests.inc"
bool SelfTest(std::wstring& report) {
    g_testBootId = "test-boot-0";
    bool ok = true;
    auto check = [&](bool cond, const std::wstring& what) {
        report += (cond ? L"PASS " : L"FAIL ") + what + L"\r\n";
        ok = ok && cond;
    };
    check(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", L"SHA-256 empty");
    check(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"SHA-256 abc");
    std::string million(1000000, 'a');
    check(Sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", L"SHA-256 1M x a");

    // RFC 8032, Ed25519 test 1 (empty message).
    std::vector<uint8_t> pub, sig;
    HexToBytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pub);
    HexToBytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", sig);
    check(crypto_ed25519_check(sig.data(), pub.data(), nullptr, 0) == 0, L"Ed25519 RFC 8032 vector");
    sig[0] ^= 1;
    check(crypto_ed25519_check(sig.data(), pub.data(), nullptr, 0) != 0, L"Ed25519 rejects a damaged signature");

    wchar_t volumeRoot[MAX_PATH];
    if (GetVolumeNameForVolumeMountPointW(L"C:\\", volumeRoot, MAX_PATH))
        check(EnsureDirs(JoinPath(volumeRoot, L"freeldr.sys"), true), L"a file in the root of a volume GUID path needs no folder");

    // Transaction logic against a scratch system drive and a file:/// feed.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = JoinPath(tmp, L"lntu-selftest-" + Widen(NowId()) + L"-" + std::to_wstring(GetCurrentProcessId()));
    std::wstring root = JoinPath(base, L"root"), feed = JoinPath(base, L"feed");
    g_dataOverride = JoinPath(base, L"data");
    ok = RegistrySelfTest(base, report) && ok;
    ok = MigrationSelfTest(base, report) && ok;
    ok = OfflineSelfTest(base, report) && ok;
    Config cfg{L"file:///" + feed + L"\\", root};
    std::replace(cfg.feedUrl.begin(), cfg.feedUrl.end(), L'\\', L'/');
    auto put = [](const std::wstring& path, const std::string& text) { return WriteTextAtomic(path, text); };
    {
        // LiberNT system files: owned by TrustedInstaller, readable but not deletable by SYSTEM and administrators.
        const wchar_t* protectedSddl = L"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464G:SY"
                                       L"D:P(A;;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;;FR;;;SY)(A;;FR;;;BA)";
        const SECURITY_INFORMATION what = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
        auto sddl = [&](const std::wstring& path) {
            std::vector<BYTE> sd(4096);
            DWORD need = 0;
            LPWSTR text = nullptr;
            std::wstring result;
            if (GetFileSecurityW(path.c_str(), what, sd.data(), (DWORD)sd.size(), &need) &&
                ConvertSecurityDescriptorToStringSecurityDescriptorW(sd.data(), SDDL_REVISION_1, what, &text, nullptr)) result = text;
            if (text) LocalFree(text);
            return result;
        };
        std::wstring error, target = JoinPath(base, L"protected\\system.dll"), fresh = target + L".lntu-new";
        auto privileges = std::make_unique<RecoveryPrivileges>(error);
        // Like System32\drivers, the folder grants modify but not FILE_DELETE_CHILD, which would bypass the file rules.
        const wchar_t* folderSddl = L"D:P(A;;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)"
                                    L"(A;;0x1301bf;;;SY)(A;;0x1301bf;;;BA)";
        PSECURITY_DESCRIPTOR sd = nullptr, folder = nullptr;
        bool fixture = *privileges && put(target, "old") &&
                       ConvertStringSecurityDescriptorToSecurityDescriptorW(protectedSddl, SDDL_REVISION_1, &sd, nullptr) &&
                       ConvertStringSecurityDescriptorToSecurityDescriptorW(folderSddl, SDDL_REVISION_1, &folder, nullptr) &&
                       SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY) && SetFileSecurityW(target.c_str(), what, sd) &&
                       SetFileSecurityW(JoinPath(base, L"protected").c_str(), DACL_SECURITY_INFORMATION, folder);
        if (sd) LocalFree(sd);
        if (folder) LocalFree(folder);
        privileges.reset();  // the restore privilege opens files with backup intent and would hide the protection
        std::wstring expected = fixture ? sddl(target) : L"";
        if (fixture && DeleteFileW(target.c_str())) {
            fixture = false;
            error = L"this system lets the account delete the protected file " + expected;
        }
        if (fixture) {
            std::wstring blocked = target + L".blocked";
            put(blocked, "blocked");
            std::wstring blockedRules = sddl(blocked);
            // An open handle makes the rename fail after the unlock: both files must get their own rules back.
            HANDLE open = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            bool refused = open != INVALID_HANDLE_VALUE &&
                           !NT_SUCCESS(LuupRenameFile(NtPath(blocked).c_str(), NtPath(target).c_str(), TRUE));
            if (open != INVALID_HANDLE_VALUE) CloseHandle(open);
            std::vector<uint8_t> kept;
            check(refused && sddl(target) == expected && sddl(blocked) == blockedRules && DeleteFileW(blocked.c_str()) &&
                  ReadFileBytes(target, kept) && std::string(kept.begin(), kept.end()) == "old",
                  L"a failed protected replacement restores the rules of both files");
            put(fresh, "new");
            bool replaced = NT_SUCCESS(LuupRenameFile(NtPath(fresh).c_str(), NtPath(target).c_str(), TRUE));
            std::vector<uint8_t> bytes;
            check(replaced && ReadFileBytes(target, bytes) && std::string(bytes.begin(), bytes.end()) == "new" &&
                  !expected.empty() && sddl(target) == expected,
                  L"a protected system file is replaced and keeps its TrustedInstaller owner and access rules");
            check(!DeleteFileW(target.c_str()) && NT_SUCCESS(LuupDeleteFile(NtPath(target).c_str())) && !FileExists(target),
                  L"a protected system file can be removed by an update");
        } else {
            if (error.empty()) error = LastErrorText(GetLastError());
            report += L"SKIP protected system file fixture: " + error + L"\r\n";
        }
    }
    {
        UpdateLock owner(report);
        bool denied = false;
        std::thread contender([&]() { std::wstring error; UpdateLock other(error, 0); denied = !other; });
        contender.join();
        check(owner && denied, L"another updater cannot acquire the transaction lock");
    }
    auto obj = [&](const std::string& text) {
        std::string sha = Sha256Hex(text.data(), text.size());
        put(JoinPath(feed, L"objects\\" + Widen(sha.substr(0, 2)) + L"\\" + Widen(sha)), text);
        return sha;
    };
    put(JoinPath(root, L"Windows\\system32\\old.dll"), "old");
    put(JoinPath(root, L"Windows\\system32\\keep.dll"), "keep");
    CheckResult c;
    c.latest.id = "R2";
    c.changes = {{"Windows/system32/old.dll", obj("new contents"), 12, Sha256Hex("old", 3)},
                 {"Windows/system32/added.dll", obj("added"), 5, ""}};
    std::wstring msg;
    InstallOutcome r = InstallUpdate(cfg, c, msg, nullptr);
    check(r == InstallOutcome::Installed, L"direct install: " + msg);
    std::vector<uint8_t> b;
    check(ReadFileBytes(JoinPath(root, L"Windows\\system32\\old.dll"), b) && std::string(b.begin(), b.end()) == "new contents",
          L"replaced file has the new contents");
    check(FileExists(JoinPath(root, L"Windows\\system32\\added.dll")), L"new file created");
    check(InstalledRelease() == "R2", L"state records release R2");

    r = RollbackLast(cfg, msg, nullptr);
    check(r == InstallOutcome::Installed, L"rollback: " + msg);
    check(ReadFileBytes(JoinPath(root, L"Windows\\system32\\old.dll"), b) && std::string(b.begin(), b.end()) == "old",
          L"rollback restored the old file");
    check(!FileExists(JoinPath(root, L"Windows\\system32\\added.dll")), L"rollback removed the added file");

    // An update that reached only some files: the rollback restores just those.
    r = InstallUpdate(cfg, c, msg, nullptr);
    check(r == InstallOutcome::Installed, L"second install: " + msg);
    put(JoinPath(root, L"Windows\\system32\\old.dll"), "old");
    r = RollbackLast(cfg, msg, nullptr);
    check(r == InstallOutcome::Installed && msg == std::wstring(Tr(L"Nahrazeno souborů: ", L"Files replaced: ")) + L"1." &&
              !FileExists(JoinPath(root, L"Windows\\system32\\added.dll")),
          L"partial update: rollback touches only the changed file: " + msg);

    // A file someone else replaced after the update is neither ours nor the original: rollback leaves it alone.
    r = InstallUpdate(cfg, c, msg, nullptr);
    put(JoinPath(root, L"Windows\\system32\\added.dll"), "edited by the user");
    r = RollbackLast(cfg, msg, nullptr);
    check(ReadFileBytes(JoinPath(root, L"Windows\\system32\\added.dll"), b) && std::string(b.begin(), b.end()) == "edited by the user" &&
              ReadFileBytes(JoinPath(root, L"Windows\\system32\\old.dll"), b) && std::string(b.begin(), b.end()) == "old",
          L"rollback keeps a file changed after the update: " + msg);
    check(InstalledRelease().empty(), L"partial rollback does not claim the previous release");
    DeleteFileW(JoinPath(root, L"Windows\\system32\\added.dll").c_str());

    // A file in use forces the restart path: luupboot's code applies the plan in-process here, as at boot.
    std::wstring keep = JoinPath(root, L"Windows\\system32\\keep.dll"), extra = JoinPath(root, L"Windows\\system32\\extra.dll");
    auto content = [&](const std::wstring& p) { return ReadFileBytes(p, b) ? std::string(b.begin(), b.end()) : std::string("<missing>"); };
    auto installLocked = [&](const std::string& id, const std::string& text) {
        HANDLE lock = CreateFileW(keep.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CheckResult cr;
        cr.installed = InstalledRelease();
        cr.latest.id = id;
        cr.changes = {{"Windows/system32/keep.dll", obj(text), text.size(), Sha256File(keep)},
                      {"Windows/system32/extra.dll", obj(id), id.size(), FileExists(extra) ? Sha256File(extra) : ""}};
        InstallOutcome res = InstallUpdate(cfg, cr, msg, nullptr);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        return res;
    };
    std::wstring dataNt = NtPath(DataDir());
    std::wstring obsolete = JoinPath(DataDir(), L"tx\\00000000-000000-01-obsolete\\marker");
    put(obsolete, "old transaction");
    int bootIndex = 0;
    auto boot = [&](BOOLEAN force) {
        g_testBootId = "test-boot-" + std::to_string(++bootIndex);
        return LuupBoot(dataNt.c_str(), force, nullptr, nullptr);
    };
    r = installLocked("R3", "keep v2");
    check(r == InstallOutcome::PendingRestart && FileExists(BootFile(L"plan.txt")) && content(keep) == "keep",
          L"restart path: plan written, files untouched: " + msg);
    check(FinalizePending(cfg, msg) && FileExists(BootFile(L"plan.txt")) && !PendingTransaction().empty(),
          L"service restart retains a scheduled update");
    boot(FALSE);
    check(content(keep) == "keep v2" && content(extra) == "R3" && FileExists(BootFile(L"trial")), L"boot applies the plan");
    check(FinalizePending(cfg, msg) && FileExists(BootFile(L"trial")) && !PendingTransaction().empty(),
          L"first boot retains recovery until the new kernel is booted");
    WriteTextAtomic(BootFile(L"trial"), "2");
    check(FinalizePending(cfg, msg) && !PendingTransaction().empty(), L"a service restart is not a new kernel boot");
    WriteTextAtomic(BootFile(L"trial"), "1");
    check(!CancelPending(msg) && FileExists(BootFile(L"trial")), L"cancel refuses an already applied update");
    boot(FALSE);
    check(FinalizePending(cfg, msg) && InstalledRelease() == "R3" && !FileExists(BootFile(L"trial")), L"service confirms after the second boot");
    check(!FileExists(obsolete), L"confirmed boot updates prune obsolete transaction backups");

    r = installLocked("R4", "keep v3");
    boot(FALSE);
    boot(FALSE);  // second boot without the service: still waiting
    check(content(keep) == "keep v3", L"one unconfirmed boot keeps the update");
    boot(FALSE);  // third boot without the service: roll back
    check(content(keep) == "keep v2" && content(extra) == "R3", L"third unconfirmed boot rolls back");
    check(FinalizePending(cfg, msg) && InstalledRelease() == "R3", L"service records the boot rollback");

    r = installLocked("R5", "keep v4");
    boot(TRUE);  // recovery entry chosen before the plan ran
    check(content(keep) == "keep v2" && FinalizePending(cfg, msg) && InstalledRelease() == "R3", L"recovery boot cancels a pending plan");

    r = installLocked("R6", "keep v5");
    boot(FALSE);
    boot(TRUE);  // recovery entry chosen after the plan ran
    check(content(keep) == "keep v2" && content(extra) == "R3" && FinalizePending(cfg, msg), L"recovery boot rolls back an applied plan");

    r = installLocked("R7", "keep v6");
    check(r == InstallOutcome::PendingRestart && CancelPending(msg) && !FileExists(BootFile(L"plan.txt")) && PendingTransaction().empty(),
          L"/cancel drops the plan");
    check(LoadHistory().size() >= 3, L"history has the transactions");

    // Crash after a direct write, before recording its result: the active journal is enough to recover it.
    std::string interrupted = NewTxId("interrupted");
    std::wstring damaged = JoinPath(root, L"Windows\\system32\\interrupted.dll");
    std::wstring backup = JoinPath(TxDir(interrupted), L"backup\\0.bin");
    put(backup, "before");
    put(damaged, "after");
    Op interruptedOp{"Windows/system32/interrupted.dll", Sha256Hex("after", 5), Sha256Hex("before", 6), L"", backup, damaged};
    SaveJson(JoinPath(TxDir(interrupted), L"tx.json"), json{{"state", "applying"}, {"mode", "direct"},
             {"installedBefore", "R3"}, {"release", "R8"}, {"ops", OpsToJson({interruptedOp})}});
    json active = LoadState();
    active["pending"] = interrupted;
    SaveState(active);
    check(FinalizePending(cfg, msg) && content(damaged) == "before" && PendingTransaction().empty() && InstalledRelease() == "R3",
          L"an interrupted direct installation restores the recorded backup");

    r = installLocked("R8", "keep interrupted");
    std::string interruptedBoot = PendingTransaction();
    json bootTx = LoadJson(JoinPath(TxDir(interruptedBoot), L"tx.json"), json::object());
    auto bootOps = OpsFromJson(bootTx["ops"]);
    CopyFileW(bootOps[0].staged.c_str(), keep.c_str(), FALSE);
    WriteTextAtomic(BootFile(L"applying"), "applying\n");
    MoveFileExW(BootFile(L"plan.txt").c_str(), BootFile(L"plan.done").c_str(), MOVEFILE_WRITE_THROUGH);
    boot(FALSE);
    check(content(keep) == "keep v2" && FinalizePending(cfg, msg) && PendingTransaction().empty(),
          L"a boot interrupted after plan rename is rolled back");

    r = installLocked("R9", "keep conflict");
    boot(FALSE);
    put(keep, "user repair");
    NTSTATUS conflicted = boot(TRUE);
    check(!NT_SUCCESS(conflicted) && content(keep) == "user repair" && FileExists(BootFile(L"rollback.txt")),
          L"boot rollback preserves a user repair and remains recoverable");
    put(keep, "keep conflict");
    boot(TRUE);
    check(FinalizePending(cfg, msg) && content(keep) == "keep v2", L"boot rollback can finish after a conflict is resolved");

    r = installLocked("R10", "keep cancelled");
    std::string cancelledTx = PendingTransaction();
    SaveJson(BootFile(L"committed"), json{{"tx", cancelledTx}, {"result", "cancelled"}, {"installed", "R3"}, {"message", "cancelled"}});
    boot(FALSE);
    check(content(keep) == "keep v2" && FinalizePending(cfg, msg) && PendingTransaction().empty(),
          L"a cancellation committed before a crash never applies its old plan");

    r = installLocked("OfflineRecovery", "keep offline");
    std::string offlineTx = PendingTransaction();
    SaveJson(BootFile(L"committed"), json{{"tx", offlineTx}, {"result", "offline-recovery"}});
    boot(FALSE);
    check(r == InstallOutcome::PendingRestart && content(keep) == "keep v2" && FinalizePending(cfg, msg) &&
          PendingTransaction() == offlineTx && FileExists(BootFile(L"plan.txt")),
          L"an incomplete offline recovery prevents boot reapplication and service confirmation");
    check(!CancelPending(msg) && PendingTransaction() == offlineTx, L"an incomplete offline recovery cannot be cancelled");
    DeleteFileW(BootFile(L"committed").c_str());
    check(CancelPending(msg), L"untouched offline recovery test fixture is cancelled");

    std::wstring asideTarget = JoinPath(root, L"Program Files\\LiberNT Update\\LiberNTUpdate.exe");
    std::string asideTx = NewTxId("self-update-interrupted");
    std::wstring asideBackup = JoinPath(TxDir(asideTx), L"backup\\0.bin");
    put(asideBackup, "previous client");
    put(asideTarget + L".old-" + Widen(asideTx), "previous client");
    Op asideOp{"Program Files/LiberNT Update/LiberNTUpdate.exe", Sha256Hex("next client", 11),
               Sha256Hex("previous client", 15), L"", asideBackup, asideTarget};
    SaveJson(JoinPath(TxDir(asideTx), L"tx.json"), json{{"state", "applying"}, {"mode", "direct"},
             {"installedBefore", "R3"}, {"release", "R11"}, {"ops", OpsToJson({asideOp})}});
    active = LoadState();
    active["pending"] = asideTx;
    SaveState(active);
    check(FinalizePending(cfg, msg) && content(asideTarget) == "previous client" && PendingTransaction().empty(),
          L"a self-update interrupted after renaming restores the previous client");

    // A partial boot rollback stays pending, preserves its sources and can finish on the next boot.
    std::wstring retryBackup1 = JoinPath(base, L"retry-backup-1.bin");
    std::wstring retryBackup2 = JoinPath(base, L"retry-backup-2.bin");
    std::wstring retryTarget1 = JoinPath(base, L"retry-target-1.bin");
    std::wstring retryTarget2 = JoinPath(base, L"retry-target-2.bin");
    put(retryBackup1, "old-1");
    put(retryTarget1, "new-1");
    put(retryTarget2, "new-2");
    DeleteFileW(retryBackup2.c_str());
    WriteUtf16(BootFile(L"rollback.txt"), L"LUUP 1\nC\t" + NtPath(retryBackup1) + L"\t" + NtPath(retryTarget1) +
                                                  L"\nC\t" + NtPath(retryBackup2) + L"\t" + NtPath(retryTarget2) + L"\n");
    WriteTextAtomic(BootFile(L"trial"), "2");
    NTSTATUS failedRollback = boot(TRUE);
    check(!NT_SUCCESS(failedRollback) && FileExists(BootFile(L"rollback.txt")) && FileExists(BootFile(L"trial")) &&
              !FileExists(BootFile(L"rolledback")) && content(retryTarget1) == "old-1" && content(retryTarget2) == "new-2" &&
              FileExists(retryBackup1),
          L"partial boot rollback stays pending and keeps its source");
    put(retryBackup2, "old-2");
    NTSTATUS retriedRollback = boot(TRUE);
    check(NT_SUCCESS(retriedRollback) && !FileExists(BootFile(L"rollback.txt")) && !FileExists(BootFile(L"trial")) &&
              FileExists(BootFile(L"rolledback")) && content(retryTarget1) == "old-1" && content(retryTarget2) == "old-2" &&
              FileExists(retryBackup1) && FileExists(retryBackup2),
          L"boot rollback finishes safely on retry");
    ClearBootFiles();

    // Full check against a signed file:/// feed: MP kernel variant, a file Setup left out, a new file.
    {
        std::vector<uint8_t> seed, sk(64), pk(32), sigb(64);
        HexToBytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
        crypto_ed25519_key_pair(sk.data(), pk.data(), seed.data());
        static const char* hexd = "0123456789abcdef";
        std::string pkhex;
        for (uint8_t v : pk) { pkhex += hexd[v >> 4]; pkhex += hexd[v & 15]; }
        SetFeedTestPublicKey(pkhex);
        long long sequence = 0;
        auto manifest = [&](const std::string& id, const std::vector<std::pair<std::string, std::string>>& fl) {
            json m{{"id", id}, {"commit", id}, {"date", "2026-01-01T00:00:00Z"}, {"files", json::array()},
                   {"aliases", json::array({json{{"path", "Windows/system32/ntoskrnl.exe"},
                                                 {"candidates", {"Windows/system32/ntoskrnl.exe", "Windows/system32/ntkrnlmp.exe"}}}})}};
            for (auto& [p, content] : fl) m["files"].push_back({{"path", p}, {"sha256", obj(content)}, {"size", content.size()}});
            std::string raw = m.dump(1);
            put(JoinPath(feed, L"releases\\" + Widen(id) + L"\\manifest.json"), raw);
            return json{{"sequence", ++sequence}, {"id", id}, {"commit", id}, {"date", "2026-01-01T00:00:00Z"}, {"manifest", "releases/" + id + "/manifest.json"},
                        {"manifestSha256", Sha256Hex(raw.data(), raw.size())}, {"notes", json::array({"test"})}};
        };
        json f{{"product", "LiberNT"}, {"format", 1}, {"releases", json::array()}};
        f["releases"].push_back(manifest("V1", {{"Windows/system32/ntoskrnl.exe", "UP1"}, {"Windows/system32/ntkrnlmp.exe", "MP1"},
                                                {"Windows/system32/a.dll", "A1"}, {"Windows/system32/opt.exe", "OPT"},
                                                {"Windows/system32/optional.exe", "OPTIONAL"}}));
        f["releases"].push_back(manifest("V2", {{"Windows/system32/ntoskrnl.exe", "UP2"}, {"Windows/system32/ntkrnlmp.exe", "MP2"},
                                                {"Windows/system32/a.dll", "A2"}, {"Windows/system32/optional.exe", "OPTIONAL"},
                                                {"Windows/system32/new.dll", "NEW"}}));
        auto writeSignedFeed = [&](const json& value) {
            std::string raw = value.dump(1);
            put(JoinPath(feed, L"feed.json"), raw);
            crypto_ed25519_sign(sigb.data(), sk.data(), (const uint8_t*)raw.data(), raw.size());
            std::string sighex;
            for (uint8_t v : sigb) { sighex += hexd[v >> 4]; sighex += hexd[v & 15]; }
            put(JoinPath(feed, L"feed.json.sig"), sighex);
            return raw;
        };
        std::string fraw = writeSignedFeed(f);

        std::wstring root2 = JoinPath(base, L"root2");
        put(JoinPath(root2, L"Windows\\system32\\ntoskrnl.exe"), "MP1");  // Setup installed the MP kernel
        put(JoinPath(root2, L"Windows\\system32\\ntkrnlmp.exe"), "MP1");
        put(JoinPath(root2, L"Windows\\system32\\a.dll"), "A1");
        put(JoinPath(root2, L"Windows\\system32\\opt.exe"), "OPT");
        Config cfg2{cfg.feedUrl, root2};
        cfg2.baseRelease = "V1";
        json st = LoadState();
        st["installed"] = "";
        SaveState(st);
        Config unknown = cfg2;
        unknown.baseRelease.clear();
        CheckResult unknownResult;
        std::wstring unknownError;
        check(!CheckForUpdate(unknown, unknownResult, unknownError, nullptr), L"an unknown installed release is not guessed");
        CheckResult cr;
        std::wstring err;
        bool got = CheckForUpdate(cfg2, cr, err, nullptr);
        check(got, L"signed feed accepted: " + err);
        json savedMachine = LoadState();
        json intermediate = manifest("Vmid", {{"Windows/system32/ntoskrnl.exe", "UP2"}, {"Windows/system32/ntkrnlmp.exe", "MP2"},
                                              {"Windows/system32/introduced.dll", "REQUIRED"}});
        json third = manifest("V3", {{"Windows/system32/ntoskrnl.exe", "UP2"}, {"Windows/system32/ntkrnlmp.exe", "MP2"},
                                    {"Windows/system32/introduced.dll", "REQUIRED"}});
        intermediate["sequence"] = 2;
        third["sequence"] = 3;
        json threeReleases = f;
        threeReleases["releases"] = json::array({f["releases"][0], intermediate, third});
        json firstMachine = savedMachine;
        firstMachine["installed"] = "V1";
        firstMachine["feedRelease"] = "V1";
        firstMachine["feedSequence"] = 1;
        SaveState(firstMachine);
        writeSignedFeed(threeReleases);
        CheckResult skipped;
        bool fromFirst = CheckForUpdate(cfg2, skipped, err, nullptr);
        bool introduced = false;
        for (auto& change : skipped.changes) introduced |= change.path == "Windows/system32/introduced.dll";
        check(fromFirst && introduced, L"upgrading R1 to R3 includes a missing library introduced in R2");
        SaveState(savedMachine);
        writeSignedFeed(f);
        std::map<std::string, std::string> ch;
        for (auto& x : cr.changes) ch[x.path] = x.newSha;
        check(cr.changes.size() == 5, L"5 changes found, including one removal (got " + std::to_wstring(cr.changes.size()) + L")");
        check(ch["Windows/system32/ntoskrnl.exe"] == Sha256Hex("MP2", 3), L"kernel keeps the MP variant");
        check(!ch.count("Windows/system32/optional.exe"), L"file left out by Setup is not added");
        check(ch.count("Windows/system32/new.dll") == 1, L"file new in the release is added");
        check(ch.count("Windows/system32/opt.exe") == 1 && ch["Windows/system32/opt.exe"].empty(), L"file removed from the release is removed safely");
        InstallOutcome r2 = InstallUpdate(cfg2, cr, msg, nullptr);
        check(r2 == InstallOutcome::Installed && ReadFileBytes(JoinPath(root2, L"Windows\\system32\\ntoskrnl.exe"), b) &&
                  std::string(b.begin(), b.end()) == "MP2",
              L"installed kernel is the new MP kernel");
        check(!FileExists(JoinPath(root2, L"Windows\\system32\\opt.exe")), L"removed file no longer exists");
        CheckResult again;
        CheckForUpdate(cfg2, again, err, nullptr);
        check(again.changes.empty(), L"second check finds nothing to do");

        json replay = f;
        replay["releases"].erase(replay["releases"].begin() + 1);
        writeSignedFeed(replay);
        check(!CheckForUpdate(cfg2, again, err, nullptr), L"older but validly signed feed is rejected");
        json legacyReplay = replay;
        for (auto& release : legacyReplay["releases"]) release.erase("sequence");
        json legacyState = LoadState();
        legacyState["feedSequence"] = 0;
        legacyState["feedRelease"] = "V2";
        SaveState(legacyState);
        writeSignedFeed(legacyReplay);
        check(!CheckForUpdate(cfg2, again, err, nullptr), L"older legacy feed without a sequence is rejected");
        legacyState["feedSequence"] = 2;
        SaveState(legacyState);
        writeSignedFeed(f);

        std::wstring latestManifestPath = JoinPath(feed, L"releases\\V2\\manifest.json");
        std::vector<uint8_t> validManifestRaw;
        ReadFileBytes(latestManifestPath, validManifestRaw);
        json invalidManifest = json::parse(validManifestRaw.begin(), validManifestRaw.end());
        invalidManifest["files"][0]["sha256"] = std::string(64, 'g');
        std::string invalidManifestRaw = invalidManifest.dump(1);
        put(latestManifestPath, invalidManifestRaw);
        json invalidFeed = f;
        invalidFeed["releases"][1]["manifestSha256"] = Sha256Hex(invalidManifestRaw.data(), invalidManifestRaw.size());
        writeSignedFeed(invalidFeed);
        check(!CheckForUpdate(cfg2, again, err, nullptr), L"signed manifest with a non-hex digest is rejected");

        invalidManifest = json::parse(validManifestRaw.begin(), validManifestRaw.end());
        invalidManifest["files"].push_back(invalidManifest["files"][0]);
        invalidManifestRaw = invalidManifest.dump(1);
        put(latestManifestPath, invalidManifestRaw);
        invalidFeed = f;
        invalidFeed["releases"][1]["manifestSha256"] = Sha256Hex(invalidManifestRaw.data(), invalidManifestRaw.size());
        writeSignedFeed(invalidFeed);
        check(!CheckForUpdate(cfg2, again, err, nullptr), L"signed manifest with a duplicate path is rejected");
        WriteFileAtomic(latestManifestPath, validManifestRaw.data(), validManifestRaw.size());
        json malformed = f;
        malformed["releases"][1]["commit"] = json::array();
        writeSignedFeed(malformed);
        check(!CheckForUpdate(cfg2, again, err, nullptr), L"a signed release with the wrong field type is rejected without throwing");
        writeSignedFeed(f);

        // The new MP kernel copied over ntoskrnl.exe by hand stays; it is not swapped for the UP one.
        std::wstring root3 = JoinPath(base, L"root3");
        put(JoinPath(root3, L"Windows\\system32\\ntoskrnl.exe"), "MP2");
        put(JoinPath(root3, L"Windows\\system32\\ntkrnlmp.exe"), "MP1");
        CheckResult hand;
        Config cfg3{cfg.feedUrl, root3};
        cfg3.baseRelease = "V1";
        CheckForUpdate(cfg3, hand, err, nullptr);
        bool kernelTouched = false;
        for (auto& x : hand.changes) kernelTouched = kernelTouched || x.path == "Windows/system32/ntoskrnl.exe";
        check(!kernelTouched, L"kernel already at the new MP version is kept");
        put(JoinPath(feed, L"feed.json"), fraw + " ");
        check(!CheckForUpdate(cfg2, again, err, nullptr), L"tampered feed rejected");
        SetFeedTestPublicKey("");
    }

    g_testBootId.clear();
    g_dataOverride.clear();
    std::wstring from = base + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    report += ok ? L"RESULT PASS\r\n" : L"RESULT FAIL\r\n";
    return ok;
}
