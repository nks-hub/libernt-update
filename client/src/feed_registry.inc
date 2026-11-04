bool CheckRegistry(const Config& cfg, const json& releases, const json& manifest, const json& state,
                   CheckResult& out, std::wstring& error) {
    if (!manifest.contains("registry")) {
        if (state.value("registryRelease", "").empty()) return true;
        error = L"The new release omits the registry baseline. The update was refused.";
        return false;
    }
    auto snapshot = [&](const json& item, const json& descriptor, RegistrySnapshot& result) {
        if (!descriptor.is_object() || descriptor.at("format") != 1 || !descriptor.at("size").is_number_unsigned()) {
            error = L"Invalid registry snapshot descriptor.";
            return false;
        }
        auto hash = descriptor.at("sha256").get<std::string>();
        auto size = descriptor.at("size").get<uint64_t>();
        if (!ValidSha256(hash) || !size || size > 16 * 1024 * 1024 || item.value("registrySha256", "") != hash) {
            error = L"Registry descriptor does not match the signed release.";
            return false;
        }
        auto cache = JoinPath(DataDir(), L"registry\\" + Widen(hash) + L".json");
        std::vector<uint8_t> raw;
        if (!ReadFileBytes(cache, raw, (size_t)size) || raw.size() != size || Sha256Hex(raw.data(), raw.size()) != hash) {
            if (!Fetch(cfg, "objects/" + hash.substr(0, 2) + "/" + hash, raw, error, (size_t)size)) return false;
            if (raw.size() != size || Sha256Hex(raw.data(), raw.size()) != hash) {
                error = L"Registry snapshot does not match its signed hash and size.";
                return false;
            }
            if (!WriteFileAtomic(cache, raw.data(), raw.size())) { error = L"Cannot cache the verified registry snapshot."; return false; }
        }
        return ParseRegistrySnapshot(raw, result, error);
    };
    const json& latest = releases.back();
    if (!IsTestData() && !RegistryUpdatesSupported()) {
        error = Tr(L"Tato verze SMSS nepodporuje bezpečnou aktualizaci registru. Nejdřív nainstalujte vydání s UpdateExecute.",
                   L"This SMSS does not support registry updates. Install an OS release with UpdateExecute first.");
        return false;
    }
    RegistrySnapshot after;
    if (!snapshot(latest, manifest["registry"], after)) return false;
    std::string baseline = state.value("registryRelease", "");
    if (baseline.empty()) baseline = cfg.registryBaseRelease;
    if (baseline.empty() && out.installed.empty()) baseline = cfg.baseRelease;
    const json* base = nullptr;
    for (const auto& item : releases) if (item.value("id", "") == baseline) base = &item;
    if (!base) {
        error = Tr(L"Výchozí registr není ověřený. Nastavte registryBaseRelease na vydání původního instalačního obrazu.",
                   L"The registry baseline is unknown. Set registryBaseRelease to the verified original installation image release.");
        return false;
    }
    RegistrySnapshot before;
    if (baseline == out.latest.id) before = after;
    else {
        auto path = base->value("manifest", "");
        std::vector<uint8_t> raw;
        if (!SafeManifestPath(path) || !Fetch(cfg, path, raw, error)) return false;
        if (Sha256Hex(raw.data(), raw.size()) != base->value("manifestSha256", "")) {
            error = L"Registry baseline manifest does not match the signed release.";
            return false;
        }
        auto previous = json::parse(raw.begin(), raw.end());
        if (previous.value("id", "") != baseline || !previous.contains("registry")) {
            error = L"The original release has no factory registry snapshot. A verified baseline is required.";
            return false;
        }
        if (!snapshot(*base, previous["registry"], before)) return false;
    }
    if (!PlanRegistryChanges(before, after, out.registryChanges, out.registryPreserved, error)) return false;
    out.registryBefore = baseline;
    out.registryAfter = out.latest.id;
    return true;
}
