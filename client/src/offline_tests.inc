bool OfflineSelfTest(const std::wstring& base, std::wstring& report) {
    bool ok = true;
    auto check = [&](bool result, const wchar_t* name) { report += (result ? L"PASS " : L"FAIL ") + std::wstring(name) + L"\r\n"; ok = ok && result; };
    auto previousData = g_dataOverride;
    g_dataOverride = JoinPath(base, L"offline-data");
    std::wstring backup = JoinPath(DataDir(), L"tx\\one\\backup\\0.bin");
    WriteTextAtomic(backup, "old");
    const std::string relative = "ProgramData/LiberNTUpdate";
    const std::wstring recorded = L"C:\\ProgramData\\LiberNTUpdate\\tx\\one\\backup\\0.bin";
    check(RecoveryDataPath(recorded, relative, DataDir()) == backup, L"offline journal data is remapped below the selected updater directory");
    check(RecoveryDataPath(L"C:\\Windows\\system32\\other.dll", relative, DataDir()).empty() &&
          RecoveryDataPath(L"C:\\ProgramData\\LiberNTUpdate\\tx\\..\\..\\other.dll", relative, DataDir()).empty() &&
          RecoveryDataPath(recorded + L":stream", relative, DataDir()).empty(), L"offline journal rejects host paths, traversal and streams");
    Config cfg{L"", JoinPath(base, L"offline-root")};
    auto target = JoinPath(cfg.root, L"Windows\\system32\\component.dll");
    WriteTextAtomic(target, "new");
    Op op{"Windows/system32/component.dll", Sha256Hex("new", 3), Sha256Hex("old", 3), L"", recorded, L"C:\\Windows\\system32\\component.dll"};
    OfflineHives hives(cfg.root);
    std::wstring plan, error;
    std::vector<Op> verify;
    bool client = false;
    bool prepared = PrepareOfflinePlan(cfg, relative, "one", {op}, false, hives, plan, verify, client, error);
    check(prepared && verify.size() == 1 && verify[0].target == target, L"offline targets ignore recorded drive letters and missing consumed staging files");
    auto planPath = JoinPath(DataDir(), L"offline-plan.txt");
    LUUP_RESULT result{};
    bool applied = prepared && WriteUtf16(planPath, plan) && NT_SUCCESS(LuupApplyPlan(NtPath(planPath).c_str(), &result, nullptr));
    check(applied && result.Done == 1 && !result.Failed && Sha256File(target) == op.oldSha, L"offline file rollback uses the guarded native plan");
    verify.clear();
    check(PrepareOfflinePlan(cfg, relative, "one", {op}, false, hives, plan, verify, client, error) && plan == L"LUUP 1\n",
          L"offline file rollback is repeatable after completion");
    WriteTextAtomic(target, "local edit");
    verify.clear();
    check(!PrepareOfflinePlan(cfg, relative, "one", {op}, false, hives, plan, verify, client, error) &&
          Sha256File(target) == Sha256Hex("local edit", 10), L"offline recovery preserves a file changed after the update");
    WriteTextAtomic(target, "new");
    DeleteFileW(backup.c_str());
    verify.clear();
    check(!PrepareOfflinePlan(cfg, relative, "one", {op}, false, hives, plan, verify, client, error) &&
          Sha256File(target) == op.newSha, L"offline recovery refuses a missing backup before changing the target");
    WriteTextAtomic(backup, "old");
    auto outside = JoinPath(base, L"outside-offline-volume.txt");
    WriteTextAtomic(outside, "outside");
    auto temporary = target + L".lntu-new";
    bool linked = CreateSymbolicLinkW(temporary.c_str(), outside.c_str(), 2) != 0;
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER) linked = CreateSymbolicLinkW(temporary.c_str(), outside.c_str(), 0) != 0;
    if (linked) {
        verify.clear();
        check(!PrepareOfflinePlan(cfg, relative, "one", {op}, false, hives, plan, verify, client, error) &&
              Sha256File(outside) == Sha256Hex("outside", 7), L"offline recovery refuses a temporary symlink and preserves its external target");
        DeleteFileW(temporary.c_str());
    } else {
        report += L"SKIP offline temporary symlink fixture: " + LastErrorText(GetLastError()) + L"\r\n";
    }
    Op aside = op;
    aside.path = "Program Files/LiberNT Update/LiberNTUpdate.exe";
    auto clientTarget = TargetPath(cfg, aside.path);
    WriteTextAtomic(clientTarget + L".old-one", "old");
    verify.clear();
    bool asideReady = PrepareOfflinePlan(cfg, relative, "one", {aside}, false, hives, plan, verify, client, error);
    result = {};
    check(asideReady && WriteUtf16(planPath, plan) && NT_SUCCESS(LuupApplyPlan(NtPath(planPath).c_str(), &result, nullptr)) &&
          !result.Failed && Sha256File(clientTarget) == aside.oldSha, L"offline recovery restores a self-update interrupted after the executable rename");
    const std::string confirmed = "20261105-123737-01-install";
    json confirmedTx{{"tx", confirmed}, {"kind", "install"}, {"state", "installed"},
                     {"release", "RecoveryBadDriver"}, {"finalInstalled", "RecoveryBadDriver"},
                     {"ops", json::array({json{{"path", "Windows/system32/drivers/probe.sys"}}})}};
    SaveJson(JoinPath(TxDir(confirmed), L"tx.json"), confirmedTx);
    check(ConfirmedRecoveryTransaction(confirmed, "RecoveryBadDriver"),
          L"offline recovery accepts a confirmed journal when history still says pending restart");
    confirmedTx["finalInstalled"] = "OtherRelease";
    SaveJson(JoinPath(TxDir(confirmed), L"tx.json"), confirmedTx);
    check(!ConfirmedRecoveryTransaction(confirmed, "RecoveryBadDriver"),
          L"offline recovery rejects a confirmed journal for a different installed release");
    json directTx{{"tx", confirmed}, {"kind", "install"}, {"mode", "direct"}, {"state", "done"},
                  {"release", "RecoveryBadLoader"}, {"ops", confirmedTx["ops"]}};
    SaveJson(JoinPath(TxDir(confirmed), L"tx.json"), directTx);
    bool directAccepted = ConfirmedRecoveryTransaction(confirmed, "RecoveryBadLoader") &&
                          !ConfirmedRecoveryTransaction(confirmed, "RecoveryBadDriver");
    directTx["mode"] = "restart";
    SaveJson(JoinPath(TxDir(confirmed), L"tx.json"), directTx);
    check(directAccepted && !ConfirmedRecoveryTransaction(confirmed, "RecoveryBadLoader"),
          L"offline recovery accepts a completed direct install such as an EFI loader update");
    directTx["mode"] = "direct";
    SaveJson(JoinPath(TxDir(confirmed), L"tx.json"), directTx);
    const std::string older = "20261105-120000-01-install";
    SaveJson(JoinPath(TxDir(older), L"tx.json"), json{{"tx", older}, {"kind", "install"}, {"mode", "direct"}, {"state", "done"},
                                                      {"release", "RecoveryBefore"}, {"ops", confirmedTx["ops"]}});
    json history = json::array({json{{"tx", older}, {"kind", "install"}, {"release", "RecoveryBefore"}, {"result", "installed"}},
                                json{{"tx", confirmed}, {"kind", "install"}, {"release", "RecoveryBadLoader"}, {"result", "installed"}}});
    json current{{"pending", ""}, {"installed", "RecoveryBadLoader"}};
    json restored{{"pending", ""}, {"installed", "RecoveryBefore"}};
    check(CurrentRecoveryTransaction(current, history) == confirmed && CurrentRecoveryTransaction(restored, history).empty(),
          L"offline recovery never offers an older install after the newest one was recovered");
    SaveState(json{{"installed", "RecoveryBadLoader"}, {"pending", confirmed}});
    SaveJson(BootFile(L"committed"), json{{"tx", confirmed}, {"result", "offline-recovery"}});
    bool blocked = OfflineRecoveryIncomplete();
    SaveJson(BootFile(L"committed"), json{{"tx", confirmed}, {"result", "installed"}});
    check(blocked && !OfflineRecoveryIncomplete(), L"automatic restart is skipped while an offline recovery is incomplete");
    g_dataOverride = previousData;
    return ok;
}
