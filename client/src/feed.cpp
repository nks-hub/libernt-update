// Signed-feed validation and comparison. Keeping this separate prevents the transaction engine from becoming a
// second feed parser and makes the downgrade rules easier to review.
#include "engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <thread>

#include "feed_internal.h"
#include "http.h"
#include "pubkey.h"
#include "runtime.h"
#include "sha256.h"
#include "util.h"

extern "C" {
#include "../third_party/monocypher/monocypher-ed25519.h"
}
#include "../third_party/nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

std::string g_testPublicKey;

json LoadJson(const std::wstring& path, json fallback) {
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(path, raw)) return fallback;
    try {
        return json::parse(raw.begin(), raw.end());
    } catch (...) {
        return fallback;
    }
}

json LoadState() { return LoadJson(JoinPath(DataDir(), L"state.json"), json{{"installed", ""}, {"pending", ""}}); }
bool SaveState(const json& state) { return WriteTextAtomic(JoinPath(DataDir(), L"state.json"), state.dump(1)); }

std::string Trim(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

bool HexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        if (!std::isxdigit((unsigned char)hex[i]) || !std::isxdigit((unsigned char)hex[i + 1])) return false;
        char* end = nullptr;
        std::string byte = hex.substr(i, 2);
        long value = strtol(byte.c_str(), &end, 16);
        if (*end) return false;
        out.push_back((uint8_t)value);
    }
    return true;
}

bool ValidSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool VerifySignature(const std::vector<uint8_t>& message, const std::string& signatureHex) {
    std::vector<uint8_t> signature, publicKey;
    if (!HexToBytes(Trim(signatureHex), signature) || signature.size() != 64) return false;
    if (!HexToBytes(g_testPublicKey.empty() ? kFeedPublicKeyHex : g_testPublicKey, publicKey) || publicKey.size() != 32) return false;
    return crypto_ed25519_check(signature.data(), publicKey.data(), message.data(), message.size()) == 0;
}

bool Fetch(const Config& cfg, const std::string& relative, std::vector<uint8_t>& out, std::wstring& error,
           size_t limit = 16u * 1024u * 1024u, bool* missing = nullptr) {
    if (missing) *missing = false;
    if (relative == "feed.json.sig" || relative == "feed-v2.json.sig") limit = 256;
    std::wstring url = cfg.feedUrl + Widen(relative);
    if (cfg.feedUrl.rfind(L"file:///", 0) == 0) {
        std::wstring path = url.substr(8);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (ReadFileBytes(path, out, limit)) return true;
        DWORD status = GetLastError();
        if (missing) *missing = status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
        error = path + L": " + LastErrorText(status);
        return false;
    }
    unsigned status = 0;
    bool ok = HttpGet(url, out, error, limit, &status);
    if (missing) *missing = status == 404;
    return ok;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
    return value;
}

bool SafeManifestPath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
    for (unsigned char c : path)
        if (c < 32 || c == 127 || c == '%' || c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

#include "feed_registry.inc"

}  // namespace

void SetFeedTestPublicKey(const std::string& publicKeyHex) { g_testPublicKey = publicKeyHex; }

bool CheckForUpdate(const Config& cfg, CheckResult& out, std::wstring& error, const Progress& progress) {
    error.clear();
    UpdateLock operation(error);
    if (!operation || !SecureUpdateData(error)) return false;
    try {
    out = CheckResult{};
    json clientState = LoadState();
    out.installed = clientState.value("installed", "");
    out.pendingTx = clientState.value("pending", "");
    if (cfg.feedUrl.empty()) {
        error = Tr(L"Není nastavená adresa aktualizací: doplň \"feedUrl\" do ", L"No update address is set: add \"feedUrl\" to ") +
                JoinPath(DataDir(), L"config.json") + L".";
        return false;
    }
    if (progress) progress(2, Tr(L"Stahuji seznam vydání…", L"Downloading the release list…"));
    std::vector<uint8_t> feedRaw, sigRaw;
    bool missing = false;
    std::string feedName = "feed-v2.json";
    if (!Fetch(cfg, feedName, feedRaw, error, 16u * 1024u * 1024u, &missing)) {
        if (!missing || clientState.value("feedProtocol", 1) >= 2) return false;
        feedName = "feed.json";
        error.clear();
        if (!Fetch(cfg, feedName, feedRaw, error)) return false;
    }
    if (!Fetch(cfg, feedName + ".sig", sigRaw, error)) return false;
    if (!VerifySignature(feedRaw, std::string(sigRaw.begin(), sigRaw.end()))) {
        error = Tr(L"Podpis seznamu vydání nesouhlasí. Aktualizace se odmítá.", L"The signature of the release list does not match. The update is refused.");
        LogLine("feed signature mismatch");
        return false;
    }
    json feed;
    try {
        feed = json::parse(feedRaw.begin(), feedRaw.end());
    } catch (...) {
        error = Tr(L"Seznam vydání není platný JSON.", L"The release list is not valid JSON.");
        return false;
    }
    if (!feed.is_object() || !feed.contains("releases") || !feed["releases"].is_array() || feed["releases"].empty()) {
        error = Tr(L"Feed neobsahuje platný seznam vydání.", L"The feed has no valid release list.");
        return false;
    }
    if (feed.value("format", 1) != (feedName == "feed-v2.json" ? 2 : 1)) { error = L"Unsupported update feed format."; return false; }
    if (feedName == "feed-v2.json" && clientState.value("feedProtocol", 1) < 2) {
        clientState["feedProtocol"] = 2;
        if (!SaveState(clientState)) { error = L"Cannot save the accepted update protocol."; return false; }
    }
    const json& releases = feed["releases"];
    long long previousSequence = 0;
    bool installedPresent = out.installed.empty();
    std::string acceptedRelease = clientState.value("feedRelease", "");
    bool acceptedReleasePresent = acceptedRelease.empty();
    try {
        for (auto& candidate : releases) {
            if (!candidate.is_object() || candidate.value("id", "").empty() || candidate.value("manifest", "").empty() ||
                !ValidSha256(candidate.value("manifestSha256", ""))) {
                error = Tr(L"Feed obsahuje neúplné vydání.", L"The feed contains an incomplete release.");
                return false;
            }
            long long sequence = candidate.value("sequence", 0LL);
            if (sequence && sequence <= previousSequence) {
                error = Tr(L"Pořadí vydání ve feedu není platné.", L"The release order in the feed is invalid.");
                return false;
            }
            if (sequence) previousSequence = sequence;
            if (candidate.value("id", "") == out.installed) installedPresent = true;
            if (candidate.value("id", "") == acceptedRelease) acceptedReleasePresent = true;
        }
    } catch (const json::exception&) {
        error = Tr(L"Feed obsahuje neplatná data.", L"The feed contains invalid data.");
        return false;
    }
    const json& release = releases.back();
    long long latestSequence = release.value("sequence", 0LL);
    long long acceptedSequence = clientState.value("feedSequence", 0LL);
    if ((acceptedSequence && latestSequence < acceptedSequence) || !installedPresent || !acceptedReleasePresent) {
        error = Tr(L"Podepsaný feed je starší než už přijaté vydání. Aktualizace se odmítá.",
                   L"The signed feed is older than a release already accepted on this system. The update is refused.");
        LogLine("feed replay or downgrade refused");
        return false;
    }
    out.latest.id = release.value("id", "");
    out.latest.commit = release.value("commit", "");
    out.latest.date = release.value("date", "");
    for (auto& note : release.value("notes", json::array()))
        if (note.is_string()) out.latest.notes.push_back(note.get<std::string>());

    auto loadManifest = [&](const json& item, json& manifest) {
        std::vector<uint8_t> raw;
        std::string manifestPath = item.value("manifest", "");
        if (!SafeManifestPath(manifestPath)) {
            error = Tr(L"Feed obsahuje neplatnou cestu k manifestu.", L"The feed contains an invalid manifest path.");
            return false;
        }
        if (!Fetch(cfg, manifestPath, raw, error)) return false;
        if (Sha256Hex(raw.data(), raw.size()) != item.value("manifestSha256", "")) {
            error = L"Manifest " + Widen(item.value("id", "")) +
                    Tr(L" nesouhlasí s podepsaným seznamem vydání.", L" does not match the signed release list.");
            return false;
        }
        try {
            manifest = json::parse(raw.begin(), raw.end());
        } catch (const json::exception&) {
            error = Tr(L"Manifest není platný JSON.", L"The manifest is not valid JSON.");
            return false;
        }
        if (!manifest.is_object() || manifest.value("id", "") != item.value("id", "") || !manifest.contains("files") ||
            !manifest["files"].is_array()) {
            error = Tr(L"Manifest má neplatnou strukturu.", L"The manifest has an invalid structure.");
            return false;
        }
        return true;
    };
    if (progress) progress(5, Tr(L"Stahuji manifest ", L"Downloading manifest ") + Widen(out.latest.id) + L"…");
    json manifest;
    if (!loadManifest(release, manifest)) return false;
    if (manifest.contains("registry") != (feedName == "feed-v2.json")) {
        error = L"Registry updates require a version 2 feed and a factory registry snapshot.";
        return false;
    }
    if (!CheckRegistry(cfg, releases, manifest, clientState, out, error)) return false;
    const json& files = manifest["files"];

    auto collectFiles = [&](const json& entries, std::map<std::string, std::string>& hashes,
                            std::map<std::string, std::string>& names, const std::wstring& invalidMessage) {
        if (!entries.is_array()) {
            error = invalidMessage;
            return false;
        }
        for (auto& file : entries) {
            if (!file.is_object() || !file.contains("path") || !file["path"].is_string() ||
                !file.contains("sha256") || !file["sha256"].is_string() ||
                (file.contains("size") && !file["size"].is_number_unsigned())) {
                error = invalidMessage;
                return false;
            }
            std::string path = file["path"].get<std::string>();
            std::string sha = file["sha256"].get<std::string>();
            std::string key = Lower(path);
            if (!SafeManifestPath(path) || !ValidSha256(sha) || hashes.count(key)) {
                error = invalidMessage;
                return false;
            }
            hashes[key] = sha;
            names[key] = path;
        }
        return true;
    };
    std::map<std::string, std::string> newSha;
    std::map<std::string, std::string> newNames;
    if (!collectFiles(files, newSha, newNames,
                      Tr(L"Manifest obsahuje neplatný nebo duplicitní soubor.",
                         L"The manifest contains an invalid or duplicate file entry.")))
        return false;

    const json* baseRelease = nullptr;
    for (auto& item : releases)
        if (!out.installed.empty() && item.value("id", "") == out.installed) baseRelease = &item;
    if (!baseRelease && !cfg.baseRelease.empty())
        for (auto& item : releases)
            if (item.value("id", "") == cfg.baseRelease) baseRelease = &item;
    if (!baseRelease && out.installed.empty()) {
        error = Tr(L"Výchozí vydání není známé. Nastavte ověřené baseRelease v config.json.",
                   L"The installed release is unknown. Set a verified baseRelease in config.json.");
        return false;
    }
    std::map<std::string, std::string> basePaths;
    std::map<std::string, std::string> baseNames;
    if (baseRelease && baseRelease->value("id", "") != out.latest.id) {
        json base;
        if (!loadManifest(*baseRelease, base)) return false;
        if (!collectFiles(base["files"], basePaths, baseNames,
                          Tr(L"Základní manifest obsahuje neplatný nebo duplicitní soubor.",
                             L"The base manifest contains an invalid or duplicate file entry.")))
            return false;
    }
    bool haveBase = !basePaths.empty();

    json emptyAliases = json::array();
    const json& aliases = manifest.contains("aliases") ? manifest["aliases"] : emptyAliases;
    if (!aliases.is_array()) {
        error = Tr(L"Manifest obsahuje neplatné varianty souborů.", L"The manifest contains invalid file aliases.");
        return false;
    }
    for (auto& alias : aliases) {
        if (!alias.is_object() || !alias.contains("path") || !alias["path"].is_string() ||
            !alias.contains("candidates") || !alias["candidates"].is_array() ||
            !SafeManifestPath(alias["path"].get<std::string>()) || !newSha.count(Lower(alias["path"].get<std::string>()))) {
            error = Tr(L"Manifest obsahuje neplatné varianty souborů.", L"The manifest contains invalid file aliases.");
            return false;
        }
        for (auto& candidate : alias["candidates"]) {
            if (!candidate.is_string() || !SafeManifestPath(candidate.get<std::string>()) ||
                !newSha.count(Lower(candidate.get<std::string>()))) {
                error = Tr(L"Manifest obsahuje neplatné varianty souborů.", L"The manifest contains invalid file aliases.");
                return false;
            }
        }
    }

    if (latestSequence > acceptedSequence || acceptedRelease != out.latest.id) {
        if (latestSequence) clientState["feedSequence"] = latestSequence;
        clientState["feedRelease"] = out.latest.id;
        if (!SaveState(clientState)) {
            error = Tr(L"Nelze uložit přijaté pořadí aktualizací.", L"The accepted update sequence cannot be saved.");
            return false;
        }
    }

    std::map<std::string, std::string> pathsToHash = newNames;
    for (auto& [key, path] : baseNames)
        if (!pathsToHash.count(key)) pathsToHash[key] = path;
    struct HashWork {
        std::string key;
        std::string path;
        std::wstring target;
        std::string hash;
    };
    std::vector<HashWork> hashWork;
    hashWork.reserve(pathsToHash.size());
    for (auto& [key, path] : pathsToHash) {
        std::wstring target = TargetPath(cfg, path);
        if (target.empty()) {
            error = Tr(L"Nelze určit EFI oddíl. Nastavte bootRoot v config.json.",
                       L"Cannot identify the EFI partition. Set bootRoot in config.json.");
            return false;
        }
        hashWork.push_back({key, path, target, ""});
    }
    std::atomic<size_t> nextHash{0};
    std::atomic<size_t> completedHashes{0};
    std::mutex progressLock;
    size_t reportedHashes = 0;
    auto hashWorker = [&]() {
        for (;;) {
            size_t index = nextHash.fetch_add(1);
            if (index >= hashWork.size()) return;
            HashWork& item = hashWork[index];
            bool exists = FileExists(item.target);
            item.hash = exists ? Sha256File(item.target) : "";
            if (exists && item.hash.empty()) item.hash = "unreadable";
            size_t completed = completedHashes.fetch_add(1) + 1;
            if (progress && (completed % 25 == 0 || completed == hashWork.size())) {
                std::lock_guard<std::mutex> lock(progressLock);
                if (completed > reportedHashes) {
                    reportedHashes = completed;
                    progress((int)(10 + 85 * completed / std::max<size_t>(hashWork.size(), 1)),
                             Tr(L"Porovnávám ", L"Comparing ") + Widen(item.path));
                }
            }
        }
    };
    unsigned available = std::thread::hardware_concurrency();
    unsigned workerCount = (unsigned)std::min<size_t>(hashWork.size(), std::min<unsigned>(4, available ? available : 2));
    std::vector<std::thread> workers;
    workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
    for (unsigned i = 1; i < workerCount; i++) {
        try {
            workers.emplace_back(hashWorker);
        } catch (...) {
            break;
        }
    }
    hashWorker();
    for (auto& worker : workers) worker.join();
    std::map<std::string, std::string> local;
    for (auto& item : hashWork) local[item.key] = std::move(item.hash);

    std::map<std::string, std::string> wanted = newSha;
    for (auto& alias : aliases) {
        std::string target = Lower(alias.value("path", ""));
        if (local[target].empty()) continue;
        for (auto& candidate : alias.value("candidates", json::array())) {
            if (!candidate.is_string()) continue;
            std::string key = Lower(candidate.get<std::string>());
            if (key != target && !newSha[key].empty() && (newSha[key] == local[target] || (!local[key].empty() && local[key] == local[target]))) {
                wanted[target] = newSha[key];
                break;
            }
        }
    }

    for (auto& file : files) {
        std::string path = file.value("path", ""), key = Lower(path);
        const std::string& have = local[key];
        const std::string& want = wanted[key];
        if (have == want) continue;
        if (have.empty() && key.rfind("bootpartition/", 0) == 0) continue;
        if (have.empty() && (!haveBase || basePaths.count(key))) continue;
        uint64_t size = file.value("size", (uint64_t)0);
        if (want != file.value("sha256", ""))
            for (auto& candidate : files)
                if (candidate.value("sha256", "") == want) {
                    size = candidate.value("size", (uint64_t)0);
                    break;
                }
        out.changes.push_back({path, want, size, have});
    }
    for (auto& [key, oldSha] : basePaths) {
        if (newSha.count(key) || local[key].empty()) continue;
        if (local[key] == oldSha) out.changes.push_back({baseNames[key], "", 0, local[key]});
        else LogLine("removed path kept because local contents differ: " + baseNames[key]);
    }
    if (progress) progress(100, Tr(L"Změn: ", L"Changes: ") + std::to_wstring(out.ChangeCount()) + L".");
    return true;
    } catch (const json::exception&) {
        error = Tr(L"Podepsaný feed nebo manifest má neplatnou strukturu.", L"The signed feed or manifest has an invalid structure.");
        out.changes.clear();
        out.registryChanges.clear();
        return false;
    }
}
