// Kept in the engine's translation unit because recovery uses the private transaction journal and operations.
static bool CommitOutcome(const Config& cfg, const std::string& tx, json& txj, const json& hist,
                          const std::string& result, const std::string& installed, const std::wstring& text, std::wstring& message,
                          bool registryApplied = false) {
    json marker{{"tx", tx}, {"result", result}, {"installed", installed}, {"message", Narrow(text)}, {"registryApplied", registryApplied}};
    std::wstring archive = JoinPath(TxDir(tx), g_offlineRecovery ? L"boot.offline.done" : L"boot.done");
    bool retired = GetFileAttributesW(archive.c_str()) != INVALID_FILE_ATTRIBUTES;
    // Boot must stop using the plans before pending can be cleared, even if the service is killed here.
    if (!retired && !SaveJson(BootFile(L"committed"), marker)) { message = L"Cannot commit the boot result; recovery was retained."; return false; }
    txj["state"] = result;
    txj["finalInstalled"] = installed;
    txj["registryApplied"] = registryApplied;
    json state = LoadState();
    state["installed"] = installed;
    RegistryState(state, txj, result == "installed" || registryApplied);
    state["pending"] = "";
    if (!SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj)) { message = L"Cannot save the recovery journal."; return false; }
    if (!retired && !MoveFileExW(BootDir().c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH)) {
        message = L"Cannot retire the committed boot plans: " + LastErrorText(GetLastError());
        return false;
    }
    if (!SaveState(state)) { message = L"Cannot save the recovery result; the terminal journal was retained."; return false; }
    message = text;
    if (!FinishHistory(hist, result, message)) {
        message = L"The result was saved, but history.json could not be updated.";
        return false;
    }
    if (!g_offlineRecovery) RemoveRecoveryEntry(cfg);
    if (!g_offlineRecovery && (result == "installed" || result == "cancelled")) RemoveSelfUpdateGuard(tx);
    if (!g_offlineRecovery && result == "installed") PruneTransactions(5, tx);
    return true;
}

/// True while an offline recovery of the pending transaction is incomplete; restarting cannot finish it.
static bool OfflineRecoveryIncomplete() {
    json committed = LoadJson(BootFile(L"committed"), json::object());
    return committed.value("result", "") == "offline-recovery" && committed.value("tx", "") == PendingTransaction();
}

bool FinalizePending(const Config& cfg, std::wstring& message) {
    UpdateLock operation(message);
    if (!operation || !SecureUpdateData(message)) return false;
    std::string tx = PendingTransaction();
    if (tx.empty()) return false;
    json txj = LoadJson(JoinPath(TxDir(tx), L"tx.json"), json::object());
    std::vector<Op> ops = OpsFromJson(txj.value("ops", json::array()));
    if (ops.empty()) { message = L"Active transaction has no readable journal; recovery was retained."; return true; }
    json hist = HistoryFor(tx);
    if (hist.empty()) hist = NewHistory(tx, txj.value("kind", "install"), txj.value("release", ""), ops.size());
    std::string before = txj.value("installedBefore", "");
    auto finish = [&](const std::string& result, const std::string& installed, const std::wstring& text, bool registryApplied = false) {
        return CommitOutcome(cfg, tx, txj, hist, result, installed, text, message, registryApplied);
    };
    json committed = LoadJson(BootFile(L"committed"), json::object());
    if (committed.value("tx", "") == tx) {
        if (committed.value("result", "") == "offline-recovery") {
            message = L"Offline recovery is incomplete. Boot the independent recovery system and repeat it.";
            return true;
        }
        finish(committed.value("result", "failed"), committed.value("installed", ""), Widen(committed.value("message", "")), committed.value("registryApplied", false));
        return true;
    }
    if (txj.contains("finalInstalled")) {
        finish(txj.value("state", "failed").c_str(), txj.value("finalInstalled", ""), L"The recovery result was committed.", txj.value("registryApplied", false));
        return true;
    }
    if (txj.value("state", "") == "done") {
        finish("installed", txj.value("release", ""), L"The completed transaction was recovered.");
        return true;
    }
    if (txj.value("mode", "") == "direct") {
        size_t conflicts = 0;
        for (size_t i = ops.size(); i-- > 0;) {
            auto& op = ops[i];
            std::wstring target = Target(cfg, op);
            std::string have = FileExists(target) ? Sha256File(target) : "";
            if (have == op.oldSha) continue;
            bool ownAside = have.empty() && IsClientFile(op.path) && Sha256File(target + L".old-" + Widen(tx)) == op.oldSha;
            if (have != op.newSha && !ownAside) { conflicts++; continue; }
            if ((!op.backup.empty() && (Sha256File(op.backup) != op.oldSha || !CopyFileW(op.backup.c_str(), target.c_str(), FALSE))) ||
                (op.backup.empty() && !DeleteFileW(target.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)) {
                message = L"Interrupted installation needs recovery: " + Widen(op.path);
                return true;
            }
        }
        finish(conflicts ? "partial-rollback" : "failed", conflicts ? "" : before,
               Tr(L"Přerušená instalace byla vrácena. Konfliktní soubory zůstaly zachované.",
                  L"The interrupted installation was undone. Conflicting files were preserved."));
        return true;
    }

    std::vector<uint8_t> raw;
    LogBootFailures();
    if (ReadFileBytes(BootFile(L"rolledback"), raw)) {
        bool cancelled = std::string(raw.begin(), raw.end()).rfind("cancelled", 0) == 0;
        std::vector<Op> originals = ops;
        OriginalTargets(originals);
        std::wstring mismatch;
        bool complete = VerifyTargets(cfg, originals, mismatch);
        finish(cancelled ? "cancelled" : complete ? "failed" : "partial-rollback", complete ? before : "",
               cancelled ? Tr(L"Aktualizace byla před použitím zrušena.", L"The update was cancelled before applying it.")
                         : Tr(L"Předchozí soubory byly obnoveny; změněné soubory zůstaly zachované.",
                              L"The previous files were restored; conflicting files were preserved."));
        return true;
    }
    if (FileExists(BootFile(L"plan.txt")) || FileExists(BootFile(L"applying"))) {
        message = FileExists(BootFile(L"applying"))
                    ? Tr(L"Přerušená boot instalace bude při restartu vrácena.", L"The interrupted boot installation will be undone at restart.")
                    : Tr(L"Aktualizace stále čeká na restart.", L"The update is still waiting for a restart.");
        return true;
    }
    if (ReadFileBytes(BootFile(L"result.txt"), raw) && std::string(raw.begin(), raw.end()).rfind("rollback", 0) == 0) {
        message = Tr(L"Návrat není dokončený. Zálohy a záchranná položka zůstávají zachované.",
                     L"Rollback is incomplete. Backups and the recovery entry were retained.");
        return true;
    }
    std::wstring bad;
    if (VerifyTargets(cfg, ops, bad)) {
        ULONG trial = 0;
        if (ReadFileBytes(BootFile(L"trial"), raw))
            for (uint8_t c : raw) { if (c < '0' || c > '9') break; trial = trial * 10 + c - '0'; }
        std::string boot = BootIdentity();
        if (trial < 2 || boot.empty() || txj.value("appliedBoot", "") == boot || txj.value("createdBoot", "") == boot) {
            if (!txj.contains("appliedBoot") && !boot.empty()) {
                txj["appliedBoot"] = boot;
                if (!SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj)) {
                    message = L"Cannot save the boot confirmation state; recovery was retained.";
                    return true;
                }
            }
            message = Tr(L"Soubory byly vyměněny. Další restart ověří nové jádro; záchrana je zachovaná.",
                         L"Files were replaced. Another restart will verify the new kernel; recovery is retained.");
            return true;
        }
        finish(txj.value("conflicts", 0) ? "partial-rollback" : "installed", txj.value("release", ""),
               Tr(L"Aktualizace byla ověřena po startu nové verze.", L"The update was verified after booting the new version."), true);
        return true;
    }
    if (!FileExists(BootFile(L"plan.done"))) {
        // The journal was committed before scheduling completed. No target was changed yet.
        finish("failed", before, L"The interrupted update preparation was cancelled.");
        return true;
    }
    message = L"Boot verification failed for " + bad + L". Recovery will run at the next boot.";
    WriteTextAtomic(BootFile(L"trial"), "2");
    return true;
}

bool CancelPending(std::wstring& message) {
    UpdateLock operation(message);
    if (!operation || !SecureUpdateData(message)) return false;
    std::string tx = PendingTransaction();
    if (tx.empty()) { message = Tr(L"Žádná aktualizace nečeká na restart.", L"No update waits for a restart."); return false; }
    if (LoadJson(BootFile(L"committed"), json::object()).value("result", "") == "offline-recovery") {
        message = L"Offline recovery must be completed from the independent recovery system.";
        return false;
    }
    json txj = LoadJson(JoinPath(TxDir(tx), L"tx.json"), json::object());
    if (txj.value("mode", "") != "restart" || FileExists(BootFile(L"applying")) || FileExists(BootFile(L"plan.done")) ||
        FileExists(BootFile(L"trial")) || !FileExists(BootFile(L"plan.txt"))) {
        message = Tr(L"Aktualizace už začala. Použijte obnovení při restartu.", L"The update already started. Use recovery at restart.");
        return false;
    }
    json hist = HistoryFor(tx);
    return CommitOutcome(LoadConfig(), tx, txj, hist, "cancelled", txj.value("installedBefore", ""),
                         Tr(L"Čekající aktualizace je zrušená; soubory nebyly změněny.",
                            L"The pending update was cancelled; no files were changed."), message);
}
