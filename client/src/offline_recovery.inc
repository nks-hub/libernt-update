namespace {

json ReadRecoveryJson(const std::wstring& path) {
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(path, raw, 8 * 1024 * 1024)) throw std::runtime_error("Unreadable recovery journal");
    return json::parse(raw.begin(), raw.end());
}

bool RecoveryTransactionId(const std::string& id) {
    return !id.empty() && id.size() <= 160 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool ConfirmedRecoveryTransaction(const std::string& id, const std::string& installed) {
    if (!RecoveryTransactionId(id)) return false;
    try {
        json transaction = ReadRecoveryJson(JoinPath(TxDir(id), L"tx.json"));
        bool confirmedRestart = transaction.value("state", "") == "installed" && transaction.value("finalInstalled", "") == installed;
        bool completedDirect = transaction.value("mode", "") == "direct" && transaction.value("state", "") == "done" &&
                               !transaction.contains("finalInstalled");
        return transaction.value("tx", "") == id && transaction.value("kind", "") == "install" &&
               (confirmedRestart || completedDirect) && transaction.value("release", "") == installed &&
               transaction.contains("ops") && transaction["ops"].is_array() && !transaction["ops"].empty();
    } catch (...) {
        return false;
    }
}

/// Pending transaction, else the newest install when it is the confirmed current release. An older install
/// would undo more than the last update, for example after a successful recovery is repeated.
std::string CurrentRecoveryTransaction(const json& state, const json& history) {
    std::string pending = state.at("pending").get<std::string>(), installed = state.at("installed").get<std::string>();
    if (!pending.empty()) return pending;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->value("kind", "") != "install") continue;
        std::string candidate = it->value("tx", "");
        bool current = !it->contains("rolledBackBy") && it->value("release", "") == installed &&
                       ConfirmedRecoveryTransaction(candidate, installed);
        return current ? candidate : std::string();
    }
    return {};
}

bool PrepareOfflinePlan(const Config& cfg, const std::string& dataRelative, const std::string& tx,
                        const std::vector<Op>& ops, bool forward, OfflineHives& hives,
                        std::wstring& plan, std::vector<Op>& verify, bool& clientUpdate, std::wstring& error) {
    plan = L"LUUP 1\n";
    for (size_t index = 0; index < ops.size(); index++) {
        size_t i = forward ? index : ops.size() - index - 1;
        const auto& op = ops[i];
        if ((!op.newSha.empty() && !RecoveryHash(op.newSha)) || (!op.oldSha.empty() && !RecoveryHash(op.oldSha))) {
            error = L"Invalid digest in the recovery journal.";
            return false;
        }
        const auto& desiredHash = forward ? op.newSha : op.oldSha;
        const auto& expectedHash = forward ? op.oldSha : op.newSha;
        auto desired = RecoveryDataPath(forward ? op.staged : op.backup, dataRelative, DataDir());
        auto expected = RecoveryDataPath(forward ? op.backup : op.staged, dataRelative, DataDir());
        Op checked = op;
        checked.newSha = desiredHash;
        auto scratch = JoinPath(TxDir(tx), L"offline\\" + std::to_wstring(i));
        if (op.registry) {
            RegistryItem before, after;
            if (!LoadRegistryRecord(desired, desiredHash, after) || !LoadRegistryRecord(expected, expectedHash, before) ||
                after.key != before.key || after.name != before.name || after.keyOnly != before.keyOnly ||
                !hives.Map(after, error) || !hives.Map(before, error)) {
                if (error.empty()) error = L"Missing or invalid offline registry records.";
                return false;
            }
            RegistryItem current = after;
            if (!ReadRegistryItem(current, error) || (!SameRegistryData(current, after) && !SameRegistryData(current, before))) {
                error = L"An offline registry value changed after the update; it was preserved.";
                return false;
            }
            desired = scratch + L"-desired.bin";
            expected = scratch + L"-expected.bin";
            std::string expectedDigest;
            if (!RegistryRecordFile(desired, after, checked.newSha) || !RegistryRecordFile(expected, before, expectedDigest)) {
                error = L"Cannot save mapped offline registry records.";
                return false;
            }
            checked.staged = desired;
            plan += L"G\t" + NtPath(desired) + L"\t" + NtPath(expected) + L"\n";
        } else {
            if (!SafeManifestPath(op.path)) { error = L"Invalid target path in the recovery journal."; return false; }
            bool boot = _strnicmp(op.path.c_str(), "BootPartition/", 14) == 0;
            if (boot && cfg.bootRoot.empty()) { error = L"Offline boot file recovery requires an explicit --boot-root."; return false; }
            auto relative = boot ? op.path.substr(14) : op.path;
            if (boot && _strnicmp(relative.c_str(), "EFI/", 4) == 0 && cfg.bootRoot == cfg.root) {
                error = L"EFI file recovery requires a separate ESP.";
                return false;
            }
            if (!PlainRecoveryPath(boot ? cfg.bootRoot : cfg.root, relative)) {
                error = L"Unreadable or redirected offline target: " + Widen(op.path);
                return false;
            }
            checked.target = TargetPath(cfg, op.path);
            DWORD attrs = GetFileAttributesW(checked.target.c_str());
            DWORD fileError = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
            if ((attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) ||
                (fileError && fileError != ERROR_FILE_NOT_FOUND && fileError != ERROR_PATH_NOT_FOUND)) {
                error = L"An offline target is a directory or cannot be inspected: " + Widen(op.path);
                return false;
            }
            std::string have = attrs == INVALID_FILE_ATTRIBUTES ? "" : Sha256File(checked.target);
            if (attrs != INVALID_FILE_ATTRIBUTES && have.empty()) { error = L"Cannot read an offline target."; return false; }
            if (have != desiredHash) {
                bool ownAside = !forward && have.empty() && IsClientFile(op.path) && !desiredHash.empty() &&
                    PlainRecoveryPath(cfg.root, relative + ".old-" + tx, true) &&
                    Sha256File(checked.target + L".old-" + Widen(tx)) == desiredHash;
                if (have != expectedHash && !ownAside) { error = L"An offline file changed after the update; it was preserved: " + Widen(op.path); return false; }
                if (!PlainRecoveryPath(boot ? cfg.bootRoot : cfg.root, relative + ".lntu-new")) {
                    error = L"A redirected replacement temporary file was refused.";
                    return false;
                }
                if (!desiredHash.empty() && (desired.empty() || Sha256File(desired) != desiredHash)) {
                    error = L"The required offline backup is missing or damaged: " + Widen(op.path);
                    return false;
                }
                expected = L"-";
                if (!expectedHash.empty() && !ownAside) {
                    std::vector<uint8_t> raw;
                    expected = scratch + L"-expected.bin";
                    if (!ReadFileBytes(checked.target, raw, 512u * 1024u * 1024u) ||
                        Sha256Hex(raw.data(), raw.size()) != expectedHash || !WriteFileAtomic(expected, raw.data(), raw.size())) {
                        error = L"Cannot preserve the expected offline file.";
                        return false;
                    }
                    expected = NtPath(expected);
                }
                plan += desiredHash.empty() ? L"X\t" + NtPath(checked.target) + L"\t" + expected + L"\n"
                                           : L"R\t" + NtPath(desired) + L"\t" + NtPath(checked.target) + L"\t" + expected + L"\n";
            }
            clientUpdate = clientUpdate || IsClientFile(op.path);
        }
        verify.push_back(checked);
    }
    return true;
}

bool RecoverOfflineChecked(const OfflineRecovery& options, std::wstring& report) {
    Config cfg = options.target;
    if (!IsUserAnAdmin() || IsTestData()) { report = L"Offline recovery requires an administrator outside self-test mode."; return false; }
    if (!SafeManifestPath(options.dataRelative) || !RecoveryTarget(cfg, report)) return false;
    if (!PlainRecoveryPath(cfg.root, options.dataRelative, true)) { report = L"The offline updater directory is missing or redirected."; return false; }
    std::wstring dataRelative = Widen(options.dataRelative);
    std::replace(dataRelative.begin(), dataRelative.end(), L'/', L'\\');
    OfflineDataScope scope(JoinPath(cfg.root, dataRelative));
    UpdateLock operation(report);
    if (!operation || !TrustedUpdateTree(DataDir(), report)) return false;
    json state = ReadRecoveryJson(JoinPath(DataDir(), L"state.json"));
    json history = ReadRecoveryJson(JoinPath(DataDir(), L"history.json"));
    if (!state.is_object() || !history.is_array()) { report = L"Invalid offline state or history."; return false; }
    std::string selected = CurrentRecoveryTransaction(state, history), installed = state.at("installed").get<std::string>();
    if (!RecoveryTransactionId(selected)) { report = L"There is no recoverable current transaction."; return false; }
    if (options.listOnly) { report = L"Recoverable transaction: " + Widen(selected) + L"; installed: " + Widen(installed); return true; }
    if (options.transaction != selected) { report = L"Pass the current transaction reported by --list as --tx."; return false; }
    json transaction = ReadRecoveryJson(JoinPath(TxDir(selected), L"tx.json"));
    if (transaction.at("tx").get<std::string>() != selected || !transaction.at("ops").is_array() ||
        transaction["ops"].empty() || transaction["ops"].size() > 100000) { report = L"Invalid offline transaction."; return false; }
    std::string kind = transaction.at("kind").get<std::string>();
    if (kind != "install" && kind != "rollback" && kind != "auto-rollback") { report = L"Unsupported offline transaction kind."; return false; }
    bool forward = kind != "install";
    std::string release = forward ? transaction.at("release").get<std::string>() : transaction.at("installedBefore").get<std::string>();
    auto ops = OpsFromJson(transaction["ops"]);
    RecoveryPrivileges privileges(report);
    if (!privileges) return false;
    OfflineHives hives(cfg.root);
    std::wstring plan;
    std::vector<Op> verify;
    bool clientUpdate = false;
    if (!PrepareOfflinePlan(cfg, options.dataRelative, selected, ops, forward, hives, plan, verify, clientUpdate, report)) return false;
    auto planPath = JoinPath(TxDir(selected), L"offline\\plan.txt");
    if (!WriteUtf16(planPath, plan)) { report = L"Cannot save the offline recovery plan."; return false; }
    state["pending"] = selected;
    if (!SaveState(state) || !SaveJson(BootFile(L"committed"), json{{"tx", selected}, {"result", "offline-recovery"}})) {
        report = L"Cannot prevent automatic reapplication; no target was changed.";
        return false;
    }
    LUUP_RESULT result{};
    auto log = JoinPath(TxDir(selected), L"offline\\plan.log");
    NTSTATUS status = LuupApplyPlan(NtPath(planPath).c_str(), &result, NtPath(log).c_str());
    std::wstring bad;
    if (!NT_SUCCESS(status) || result.Failed || !VerifyTargets(cfg, verify, bad)) {
        report = L"Offline recovery is incomplete; backups and the journal were retained. " + bad + L" Log: " + log;
        return false;
    }
    if ((clientUpdate && !hives.DisableGuard(selected, report)) || !hives.Unload(report)) return false;
    transaction["offlineRecovered"] = NowIso();
    json entry = HistoryFor(selected);
    if (entry.empty()) entry = NewHistory(selected, kind, transaction.value("release", ""), ops.size());
    return CommitOutcome(cfg, selected, transaction, entry, forward ? "installed" : "failed", release,
                         L"Recovered offline from transaction " + Widen(selected) + L". Backups were retained.", report, forward);
}

}

bool RecoverOffline(const OfflineRecovery& options, std::wstring& report) {
    report.clear();
    try { return RecoverOfflineChecked(options, report); }
    catch (const json::exception&) { report = L"Invalid JSON data in the offline recovery journal."; return false; }
    catch (const std::runtime_error& error) { report = Widen(error.what()); return false; }
}
