bool LoadRegistryRecord(const std::wstring& path, const std::string& hash, RegistryItem& item) {
    std::vector<uint8_t> raw;
    return ReadFileBytes(path, raw, 2 * 1024 * 1024) && Sha256Hex(raw.data(), raw.size()) == hash && DecodeRegistryItem(raw, item);
}

bool RegistryMatches(const std::wstring& path, const std::string& hash) {
    RegistryItem expected;
    std::wstring error;
    if (!LoadRegistryRecord(path, hash, expected)) return false;
    RegistryItem current = expected;
    return ReadRegistryItem(current, error) && SameRegistryData(current, expected);
}

bool RegistryRecordFile(const std::wstring& path, const RegistryItem& item, std::string& hash) {
    auto raw = EncodeRegistryItem(item);
    if (raw.empty()) return false;
    hash = Sha256Hex(raw.data(), raw.size());
    return WriteFileAtomic(path, raw.data(), raw.size());
}

bool PrepareRegistryOps(const CheckResult& check, const std::wstring& dir, std::vector<Op>& ops, std::wstring& error) {
    for (size_t i = 0; i < check.registryChanges.size(); i++) {
        const auto& change = check.registryChanges[i];
        RegistryItem current = change.before;
        if (change.before.key != change.after.key || change.before.name != change.after.name ||
            change.before.keyOnly != change.after.keyOnly || !ReadRegistryItem(current, error)) return false;
        if (!SameRegistryData(current, change.before)) {
            error = Tr(L"Registr se od kontroly změnil. Zopakujte kontrolu: ", L"The registry changed since the check. Check again: ") + current.key;
            return false;
        }
        Op op;
        op.registry = true;
        op.path = Narrow(change.after.key) + (change.after.keyOnly ? "" : "\\" + Narrow(change.after.name));
        op.staged = JoinPath(dir, L"registry\\" + std::to_wstring(i) + L"-new.bin");
        op.backup = JoinPath(dir, L"registry\\" + std::to_wstring(i) + L"-old.bin");
        if (!RegistryRecordFile(op.staged, change.after, op.newSha) || !RegistryRecordFile(op.backup, change.before, op.oldSha)) {
            error = L"Cannot save registry transaction records: " + Widen(op.path);
            return false;
        }
        ops.push_back(op);
    }
    return true;
}

bool PrepareRegistryRollback(const Op& previous, const std::vector<Op>& pending, const std::wstring& dir,
                             size_t index, Op& op, bool& conflict, std::wstring& error) {
    RegistryItem before, after;
    if (!LoadRegistryRecord(previous.backup, previous.oldSha, before) || !LoadRegistryRecord(previous.staged, previous.newSha, after)) {
        error = L"Registry rollback records are missing or damaged: " + Widen(previous.path);
        return false;
    }
    RegistryItem current = after;
    if (!ReadRegistryItem(current, error)) return false;
    if (SameRegistryData(current, before)) return true;
    if (!SameRegistryData(current, after)) { conflict = true; return true; }
    if (before.keyOnly && !before.present) {
        std::set<std::wstring, RegistryNameLess> removedKeys, removedValues;
        RegistryNameLess less;
        for (const auto& item : pending) if (item.registry) {
            RegistryItem desired;
            if (!LoadRegistryRecord(item.staged, item.newSha, desired)) { error = L"Invalid prepared registry rollback record."; return false; }
            if (desired.present) continue;
            if (desired.keyOnly) removedKeys.insert(desired.key);
            else if (!less(desired.key, before.key) && !less(before.key, desired.key)) removedValues.insert(desired.name);
        }
        std::vector<std::wstring> keys, values;
        if (!RegistryChildren(before.key, keys, values, error)) return false;
        if (!std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return removedKeys.count(before.key + L"\\" + key); }) ||
            !std::all_of(values.begin(), values.end(), [&](const auto& value) { return removedValues.count(value); })) {
            conflict = true;
            return true;
        }
    }
    op = previous;
    op.newSha = previous.oldSha;
    op.oldSha = previous.newSha;
    op.staged = JoinPath(dir, L"registry\\" + std::to_wstring(index) + L"-new.bin");
    op.backup = JoinPath(dir, L"registry\\" + std::to_wstring(index) + L"-old.bin");
    if (!RegistryRecordFile(op.staged, before, op.newSha) || !RegistryRecordFile(op.backup, after, op.oldSha)) {
        error = L"Cannot save registry rollback records: " + Widen(op.path);
        return false;
    }
    return true;
}

void OriginalTargets(std::vector<Op>& ops) {
    for (auto& op : ops) {
        op.newSha = op.oldSha;
        if (op.registry) op.staged = op.backup;
    }
}

void RegistryState(json& state, const json& tx, bool installed) {
    const char* field = installed ? "registryAfter" : "registryBefore";
    if (tx.contains(field)) state["registryRelease"] = tx[field];
}
