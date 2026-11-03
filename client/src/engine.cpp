#include "engine.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <map>

#include "http.h"
#include "pubkey.h"
#include "sha256.h"
#include "util.h"

extern "C" {
#include "../third_party/monocypher/monocypher-ed25519.h"
}
#include "../third_party/nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

std::wstring g_dataOverride;  // set by SelfTest to keep its state away from the real one
std::string g_testPubKey;     // set by SelfTest only: its feed is signed with the RFC 8032 test key

const wchar_t kServiceName[] = L"LiberNTUpdate";
const wchar_t kSessionManager[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";

bool HexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        std::string byte = hex.substr(i, 2);
        long v = strtol(byte.c_str(), &end, 16);
        if (*end) return false;
        out.push_back((uint8_t)v);
    }
    return true;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool VerifySignature(const std::vector<uint8_t>& msg, const std::string& sigHex) {
    std::vector<uint8_t> sig, pub;
    if (!HexToBytes(Trim(sigHex), sig) || sig.size() != 64) return false;
    if (!HexToBytes(g_testPubKey.empty() ? kFeedPublicKeyHex : g_testPubKey, pub) || pub.size() != 32) return false;
    return crypto_ed25519_check(sig.data(), pub.data(), msg.data(), msg.size()) == 0;
}

/// Fetches a file of the feed: over HTTP, or from a directory for file:/// feeds (self-test, offline media).
bool Fetch(const Config& cfg, const std::string& rel, std::vector<uint8_t>& out, std::wstring& error) {
    std::wstring url = cfg.feedUrl + Widen(rel);
    if (cfg.feedUrl.rfind(L"file:///", 0) == 0) {
        std::wstring path = url.substr(8);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (ReadFileBytes(path, out)) return true;
        error = path + L": " + LastErrorText(GetLastError());
        return false;
    }
    return HttpGet(url, out, error);
}

std::wstring TargetPath(const Config& cfg, const std::string& rel) {
    std::wstring p = Widen(rel);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return JoinPath(cfg.root, p);
}

std::wstring TxDir(const std::string& tx) { return JoinPath(JoinPath(DataDir(), L"tx"), Widen(tx)); }

/// Transaction id that sorts by time and never repeats, even for two operations within one second.
std::string NewTxId(const std::string& kind) {
    std::string base = NowId();
    for (int n = 1;; n++) {
        char seq[8];
        snprintf(seq, sizeof seq, "%02d", n);
        std::string id = base + "-" + seq + "-" + kind;
        if (GetFileAttributesW(TxDir(id).c_str()) == INVALID_FILE_ATTRIBUTES) return id;
    }
}

json LoadJson(const std::wstring& path, json fallback) {
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(path, raw)) return fallback;
    try {
        return json::parse(raw.begin(), raw.end());
    } catch (...) {
        return fallback;
    }
}

bool SaveJson(const std::wstring& path, const json& j) { return WriteTextAtomic(path, j.dump(1)); }

json LoadState() { return LoadJson(JoinPath(DataDir(), L"state.json"), json{{"installed", ""}, {"pending", ""}}); }
bool SaveState(const json& s) { return SaveJson(JoinPath(DataDir(), L"state.json"), s); }

void AddHistory(const json& entry) {
    std::wstring path = JoinPath(DataDir(), L"history.json");
    json h = LoadJson(path, json::array());
    if (!h.is_array()) h = json::array();
    bool replaced = false;
    for (auto& e : h)
        if (e.value("tx", "") == entry.value("tx", "")) { e = entry; replaced = true; }
    if (!replaced) h.push_back(entry);
    SaveJson(path, h);
}

json HistoryFor(const std::string& tx) {
    for (auto& e : LoadJson(JoinPath(DataDir(), L"history.json"), json::array()))
        if (e.value("tx", "") == tx) return e;
    return json::object();
}

/// A file someone has open or mapped (a loaded DLL, the running kernel) cannot be replaced now.
bool InUse(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
    DWORD e = GetLastError();
    return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
}

/// Removes the restart-time renames this transaction registered (used when it fails before the restart).
void UnscheduleRenames(const std::string& tx, const std::vector<std::wstring>& deletedTargets) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSessionManager, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ && size) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
        RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, (BYTE*)buf.data(), &size);
        // Entries are (source, target) pairs; an empty target means "delete", so the list holds empty strings and
        // has to be read pair by pair instead of stopping at the first empty one.
        std::vector<std::wstring> items;
        const wchar_t* p = buf.data();
        const wchar_t* end = buf.data() + size / sizeof(wchar_t);
        while (p < end) {
            size_t n = wcslen(p);
            items.emplace_back(p, n);
            p += n + 1;
            if (p < end && *p == 0 && items.size() % 2 == 0) break;  // double NUL ends the list
        }
        std::wstring marker = L"\\tx\\" + Widen(tx) + L"\\";
        auto lower = [](std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; };
        std::wstring lmarker = lower(marker);
        std::vector<std::wstring> kept;
        for (size_t i = 0; i + 1 < items.size(); i += 2) {
            std::wstring src = lower(items[i]);
            bool ours = src.find(lmarker) != std::wstring::npos;
            if (!ours && items[i + 1].empty())
                for (auto& t : deletedTargets)
                    if (src == lower(L"\\??\\" + t)) ours = true;
            if (!ours) { kept.push_back(items[i]); kept.push_back(items[i + 1]); }
        }
        if (kept.empty()) {
            RegDeleteValueW(key, L"PendingFileRenameOperations");
        } else {
            std::wstring blob;
            for (auto& s : kept) { blob += s; blob.push_back(L'\0'); }
            blob.push_back(L'\0');
            RegSetValueExW(key, L"PendingFileRenameOperations", 0, REG_MULTI_SZ, (const BYTE*)blob.data(), (DWORD)(blob.size() * sizeof(wchar_t)));
        }
    }
    RegCloseKey(key);
}

struct Op {
    std::string path, newSha, oldSha;
    std::wstring staged;  ///< file inside the transaction to move into place; empty = delete the target
    std::wstring backup;  ///< copy of the previous file; empty = the file did not exist
};

json OpsToJson(const std::vector<Op>& ops) {
    json a = json::array();
    for (auto& o : ops)
        a.push_back({{"path", o.path}, {"newSha", o.newSha}, {"oldSha", o.oldSha}, {"staged", Narrow(o.staged)}, {"backup", Narrow(o.backup)}});
    return a;
}

std::vector<Op> OpsFromJson(const json& a) {
    std::vector<Op> ops;
    for (auto& j : a)
        ops.push_back({j.value("path", ""), j.value("newSha", ""), j.value("oldSha", ""), Widen(j.value("staged", "")), Widen(j.value("backup", ""))});
    return ops;
}

/// Puts back the backups of the ops already applied (in reverse) after a failure in the middle.
void UndoDirect(const Config& cfg, const std::vector<Op>& ops, size_t applied) {
    for (size_t i = applied; i-- > 0;) {
        std::wstring target = TargetPath(cfg, ops[i].path);
        if (!ops[i].backup.empty()) CopyFileW(ops[i].backup.c_str(), target.c_str(), FALSE);
        else DeleteFileW(target.c_str());
    }
}

bool VerifyTargets(const Config& cfg, const std::vector<Op>& ops, std::wstring& bad) {
    for (auto& o : ops) {
        std::wstring target = TargetPath(cfg, o.path);
        bool ok = o.newSha.empty() ? !FileExists(target) : Sha256File(target) == o.newSha;
        if (!ok) { bad = Widen(o.path); return false; }
    }
    return true;
}

/// Applies a prepared transaction: directly when nothing is in use, otherwise everything at the next restart,
/// so the running system never mixes old and new core files.
InstallOutcome ApplyTx(const Config& cfg, const std::string& tx, json& txj, std::vector<Op>& ops, std::wstring& message,
                       const Progress& progress) {
    bool restart = false;
    for (auto& o : ops)
        if (InUse(TargetPath(cfg, o.path))) { restart = true; break; }
    txj["mode"] = restart ? "restart" : "direct";
    SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj);

    std::vector<std::wstring> deleted;
    for (size_t i = 0; i < ops.size(); i++) {
        auto& o = ops[i];
        std::wstring target = TargetPath(cfg, o.path);
        if (progress) progress((int)(70 + 25 * i / std::max<size_t>(ops.size(), 1)), L"Nahrazuji " + Widen(o.path));
        if (!EnsureDirs(target, true)) {
            message = L"Nelze vytvořit složku pro " + Widen(o.path) + L": " + LastErrorText(GetLastError());
            if (restart) UnscheduleRenames(tx, deleted); else UndoDirect(cfg, ops, i);
            return InstallOutcome::Failed;
        }
        BOOL ok;
        if (o.staged.empty()) {
            ok = restart ? MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) : (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND);
            if (restart) deleted.push_back(target);
        } else {
            ok = MoveFileExW(o.staged.c_str(), target.c_str(),
                             restart ? (MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING)
                                     : (MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH));
        }
        if (!ok) {
            DWORD err = GetLastError();
            message = L"Nelze nahradit " + Widen(o.path) + L": " + LastErrorText(err);
            if (restart) UnscheduleRenames(tx, deleted); else UndoDirect(cfg, ops, i);
            return InstallOutcome::Failed;
        }
    }
    if (restart) return InstallOutcome::PendingRestart;

    std::wstring bad;
    if (!VerifyTargets(cfg, ops, bad)) {
        UndoDirect(cfg, ops, ops.size());
        message = L"Kontrola po nahrazení selhala u " + bad + L"; původní soubory jsou vrácené.";
        return InstallOutcome::Failed;
    }
    return InstallOutcome::Installed;
}

/// Keeps the transaction folders of the last few updates (their backups make rollback possible).
void PruneTransactions(size_t keep) {
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(JoinPath(JoinPath(DataDir(), L"tx"), L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') dirs.push_back(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(dirs.begin(), dirs.end());
    std::string pending = PendingTransaction();
    for (size_t i = 0; i + keep < dirs.size(); i++) {
        if (Narrow(dirs[i]) == pending) continue;
        std::wstring dir = JoinPath(JoinPath(DataDir(), L"tx"), dirs[i]);
        std::wstring from = dir + L'\0';
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
}

json NewHistory(const std::string& tx, const std::string& kind, const std::string& release, size_t files) {
    return json{{"tx", tx}, {"kind", kind}, {"release", release}, {"started", NowIso()}, {"finished", ""},
                {"result", "running"}, {"files", files}, {"message", ""}};
}

void FinishHistory(json h, const std::string& result, const std::wstring& message) {
    h["finished"] = NowIso();
    h["result"] = result;
    h["message"] = Narrow(message);
    AddHistory(h);
    LogLine("tx " + h.value("tx", "") + " " + h.value("kind", "") + " " + result + ": " + Narrow(message));
}

/// Runs a prepared transaction to its end and records the outcome in state and history.
InstallOutcome RunTx(const Config& cfg, const std::string& tx, json& txj, std::vector<Op>& ops, json hist,
                     const std::string& releaseAfter, std::wstring& message, const Progress& progress) {
    message.clear();
    InstallOutcome r = ApplyTx(cfg, tx, txj, ops, message, progress);
    json state = LoadState();
    if (r == InstallOutcome::Installed) {
        txj["state"] = "done";
        state["installed"] = releaseAfter;
        if (message.empty()) message = std::to_wstring(ops.size()) + L" souborů nahrazeno.";
        FinishHistory(hist, "installed", message);
    } else if (r == InstallOutcome::PendingRestart) {
        txj["state"] = "pending";
        state["pending"] = tx;
        message = std::to_wstring(ops.size()) + L" souborů se nahradí při restartu.";
        hist["result"] = "pending-restart";
        hist["message"] = Narrow(message);
        AddHistory(hist);
        LogLine("tx " + tx + " pending restart");
    } else {
        txj["state"] = "failed";
        FinishHistory(hist, "failed", message);
    }
    SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj);
    SaveState(state);
    if (progress) progress(100, message);
    return r;
}

/// Builds and runs a transaction that restores the backups of tx `of` (manual rollback or failed verification).
InstallOutcome RevertTx(const Config& cfg, const std::string& of, const std::string& kind, std::wstring& message,
                        const Progress& progress) {
    json src = LoadJson(JoinPath(TxDir(of), L"tx.json"), json::object());
    if (!src.contains("ops")) { message = L"Záloha aktualizace " + Widen(of) + L" už neexistuje."; return InstallOutcome::Failed; }
    std::vector<Op> old = OpsFromJson(src["ops"]);
    std::string tx = NewTxId(kind);
    std::wstring dir = TxDir(tx);
    EnsureDirs(dir, false);
    std::vector<Op> ops;
    for (size_t i = old.size(); i-- > 0;) {
        Op o;
        o.path = old[i].path;
        o.oldSha = old[i].newSha;
        o.newSha = old[i].oldSha;
        std::wstring target = TargetPath(cfg, o.path);
        bool exists = FileExists(target);
        // Files the update never reached are already as they were; touching them again only adds risk.
        if (exists ? Sha256File(target) == o.newSha : o.newSha.empty()) continue;
        if (!old[i].backup.empty()) {
            // Restart-time moves consume their source, so restore from a copy and keep the original backup.
            o.staged = JoinPath(dir, L"new\\" + std::to_wstring(i) + L".bin");
            if (!EnsureDirs(o.staged, true) || !CopyFileW(old[i].backup.c_str(), o.staged.c_str(), FALSE) ||
                Sha256File(o.staged) != o.newSha) {
                message = L"Záloha souboru " + Widen(o.path) + L" je poškozená nebo chybí.";
                return InstallOutcome::Failed;
            }
        }
        if (exists) {
            o.backup = JoinPath(dir, L"backup\\" + std::to_wstring(i) + L".bin");
            if (!EnsureDirs(o.backup, true) || !CopyFileW(target.c_str(), o.backup.c_str(), FALSE)) {
                message = L"Nelze zálohovat " + Widen(o.path) + L": " + LastErrorText(GetLastError());
                return InstallOutcome::Failed;
            }
        }
        ops.push_back(o);
    }
    std::string releaseBefore = src.value("installedBefore", "");
    json txj{{"tx", tx}, {"kind", kind}, {"of", of}, {"release", releaseBefore}, {"installedBefore", LoadState().value("installed", "")},
             {"ops", OpsToJson(ops)}, {"state", "prepared"}};
    json hist = NewHistory(tx, kind, releaseBefore, ops.size());
    hist["of"] = of;
    InstallOutcome r = RunTx(cfg, tx, txj, ops, hist, releaseBefore, message, progress);
    if (r == InstallOutcome::Installed || r == InstallOutcome::PendingRestart) {
        json orig = HistoryFor(of);
        if (!orig.empty()) { orig["rolledBackBy"] = tx; AddHistory(orig); }
    }
    return r;
}

}  // namespace

std::wstring DataDir() {
    if (!g_dataOverride.empty()) return g_dataOverride;
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path))) return JoinPath(path, L"LiberNTUpdate");
    return L"C:\\ProgramData\\LiberNTUpdate";
}

Config LoadConfig() {
    Config c{L"http://updates.example.org/", L"C:\\"};
    json j = LoadJson(JoinPath(DataDir(), L"config.json"), json::object());
    if (j.contains("feedUrl")) c.feedUrl = Widen(j["feedUrl"].get<std::string>());
    if (j.contains("root")) c.root = Widen(j["root"].get<std::string>());
    if (!c.feedUrl.empty() && c.feedUrl.back() != L'/') c.feedUrl += L'/';
    return c;
}

std::string InstalledRelease() { return LoadState().value("installed", ""); }
std::string PendingTransaction() { return LoadState().value("pending", ""); }

std::vector<HistoryEntry> LoadHistory() {
    std::vector<HistoryEntry> out;
    for (auto& e : LoadJson(JoinPath(DataDir(), L"history.json"), json::array())) {
        HistoryEntry h;
        h.tx = e.value("tx", "");
        h.kind = e.value("kind", "");
        h.release = e.value("release", "");
        h.started = e.value("started", "");
        h.finished = e.value("finished", "");
        h.result = e.value("result", "");
        h.message = e.value("message", "");
        if (e.contains("rolledBackBy")) h.result += " (vráceno)";
        h.files = e.value("files", 0);
        out.push_back(h);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

bool CheckForUpdate(const Config& cfg, CheckResult& out, std::wstring& error, const Progress& progress) {
    out = CheckResult{};
    out.installed = InstalledRelease();
    out.pendingTx = PendingTransaction();
    if (progress) progress(2, L"Stahuji seznam vydání…");
    std::vector<uint8_t> feedRaw, sigRaw, manRaw;
    if (!Fetch(cfg, "feed.json", feedRaw, error) || !Fetch(cfg, "feed.json.sig", sigRaw, error)) return false;
    if (!VerifySignature(feedRaw, std::string(sigRaw.begin(), sigRaw.end()))) {
        error = L"Podpis seznamu vydání nesouhlasí. Aktualizace se odmítá.";
        LogLine("feed signature mismatch");
        return false;
    }
    json feed;
    try {
        feed = json::parse(feedRaw.begin(), feedRaw.end());
    } catch (...) {
        error = L"Seznam vydání není platný JSON.";
        return false;
    }
    if (!feed.contains("releases") || feed["releases"].empty()) { error = L"Feed neobsahuje žádné vydání."; return false; }
    const json& rel = feed["releases"].back();
    out.latest.id = rel.value("id", "");
    out.latest.commit = rel.value("commit", "");
    out.latest.date = rel.value("date", "");
    for (auto& n : rel.value("notes", json::array())) out.latest.notes.push_back(n.get<std::string>());

    auto loadManifest = [&](const json& r, json& m) {
        std::vector<uint8_t> raw;
        if (!Fetch(cfg, r.value("manifest", ""), raw, error)) return false;
        if (Sha256Hex(raw.data(), raw.size()) != r.value("manifestSha256", "")) {
            error = L"Manifest " + Widen(r.value("id", "")) + L" nesouhlasí s podepsaným seznamem vydání.";
            return false;
        }
        m = json::parse(raw.begin(), raw.end());
        return true;
    };
    if (progress) progress(5, L"Stahuji manifest " + Widen(out.latest.id) + L"…");
    json manifest;
    if (!loadManifest(rel, manifest)) return false;
    const json& files = manifest["files"];

    // The release this system is on (or else the one before the newest) tells which missing files are new
    // in the update; files that were already shipped but are absent were left out by Setup on purpose.
    const json* baseRel = nullptr;
    const json& releases = feed["releases"];
    for (auto& r : releases)
        if (!out.installed.empty() && r.value("id", "") == out.installed) baseRel = &r;
    if (!baseRel && releases.size() >= 2) baseRel = &releases[releases.size() - 2];
    std::map<std::string, std::string> basePaths;  // lowercase path -> sha
    if (baseRel && baseRel->value("id", "") != out.latest.id) {
        json base;
        if (!loadManifest(*baseRel, base)) return false;
        for (auto& f : base["files"]) {
            std::string k = f.value("path", "");
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            basePaths[k] = f.value("sha256", "");
        }
    }
    bool haveBase = !basePaths.empty();

    // Hashing 2 000+ system files is slow; reuse hashes of files whose size and time did not change.
    std::wstring cachePath = JoinPath(DataDir(), L"hashcache.json");
    json cache = LoadJson(cachePath, json::object());
    json newCache = json::object();
    std::map<std::string, std::string> local;  // lowercase path -> sha of the file on disk ("" = missing)
    std::map<std::string, std::string> newSha;  // lowercase path -> sha in the release
    size_t i = 0;
    for (auto& f : files) {
        std::string path = f.value("path", ""), key = path;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        newSha[key] = f.value("sha256", "");
        std::wstring target = TargetPath(cfg, path);
        if (progress && (i % 25 == 0)) progress((int)(10 + 85 * i / std::max<size_t>(files.size(), 1)), L"Porovnávám " + Widen(path));
        i++;
        std::string sha;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            uint64_t size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
            uint64_t time = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
            const json* hit = cache.contains(key) ? &cache[key] : nullptr;
            if (hit && hit->size() == 3 && (*hit)[0] == size && (*hit)[1] == time) sha = (*hit)[2];
            else sha = Sha256File(target);
            if (sha.empty()) sha = "unreadable";
            newCache[key] = json::array({size, time, sha});
        }
        local[key] = sha;
    }
    SaveJson(cachePath, newCache);

    // Variant files (UP/MP kernel, HAL): keep the variant installed here and take its new version.
    std::map<std::string, std::string> wanted = newSha;
    for (auto& al : manifest.value("aliases", json::array())) {
        std::string t = al.value("path", "");
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        if (local[t].empty()) continue;
        for (auto& c : al.value("candidates", json::array())) {
            std::string k = c.get<std::string>();
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            // The target matches the variant as it is installed here, or already its new version.
            if (k != t && !newSha[k].empty() && (newSha[k] == local[t] || (!local[k].empty() && local[k] == local[t]))) {
                wanted[t] = newSha[k];
                break;
            }
        }
    }

    for (auto& f : files) {
        std::string path = f.value("path", ""), key = path;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        const std::string& have = local[key];
        const std::string& want = wanted[key];
        if (have == want) continue;
        if (have.empty() && (!haveBase || basePaths.count(key))) continue;  // left out by Setup, not new in this release
        uint64_t size = f.value("size", (uint64_t)0);
        if (want != f.value("sha256", ""))
            for (auto& g : files)
                if (g.value("sha256", "") == want) { size = g.value("size", (uint64_t)0); break; }
        out.changes.push_back({path, want, size, have});
    }
    if (progress) progress(100, std::to_wstring(out.changes.size()) + L" změněných souborů.");
    return true;
}

InstallOutcome InstallUpdate(const Config& cfg, const CheckResult& check, std::wstring& message, const Progress& progress) {
    if (!PendingTransaction().empty()) { message = L"Předchozí aktualizace čeká na restart."; return InstallOutcome::Failed; }
    if (check.changes.empty()) { message = L"Systém už odpovídá vydání " + Widen(check.latest.id) + L"."; return InstallOutcome::NothingToDo; }
    std::string tx = NewTxId("install");
    std::wstring dir = TxDir(tx);
    EnsureDirs(dir, false);
    json hist = NewHistory(tx, "install", check.latest.id, check.changes.size());
    std::vector<Op> ops;
    uint64_t total = 0, done = 0;
    for (auto& c : check.changes) total += c.size;
    for (size_t i = 0; i < check.changes.size(); i++) {
        const Change& c = check.changes[i];
        Op o{c.path, c.newSha, c.oldSha == "unreadable" ? "" : c.oldSha, JoinPath(dir, L"new\\" + std::to_wstring(i) + L".bin"), L""};
        if (progress) progress((int)(60 * done / std::max<uint64_t>(total, 1)), L"Stahuji " + Widen(c.path));
        std::vector<uint8_t> data;
        std::wstring err;
        if (!Fetch(cfg, "objects/" + c.newSha.substr(0, 2) + "/" + c.newSha, data, err) || Sha256Hex(data.data(), data.size()) != c.newSha) {
            message = L"Stažení " + Widen(c.path) + L" selhalo: " + (err.empty() ? L"obsah nesouhlasí s manifestem" : err);
            FinishHistory(hist, "failed", message);
            return InstallOutcome::Failed;
        }
        if (!WriteFileAtomic(o.staged, data.data(), data.size())) {
            message = L"Nelze uložit " + o.staged + L": " + LastErrorText(GetLastError());
            FinishHistory(hist, "failed", message);
            return InstallOutcome::Failed;
        }
        done += c.size;
        std::wstring target = TargetPath(cfg, c.path);
        if (FileExists(target)) {
            o.backup = JoinPath(dir, L"backup\\" + std::to_wstring(i) + L".bin");
            if (!EnsureDirs(o.backup, true) || !CopyFileW(target.c_str(), o.backup.c_str(), FALSE)) {
                message = L"Nelze zálohovat " + Widen(c.path) + L": " + LastErrorText(GetLastError());
                FinishHistory(hist, "failed", message);
                return InstallOutcome::Failed;
            }
            o.oldSha = Sha256File(o.backup);
        } else {
            o.oldSha.clear();
        }
        ops.push_back(o);
    }
    json txj{{"tx", tx}, {"kind", "install"}, {"release", check.latest.id}, {"commit", check.latest.commit},
             {"installedBefore", check.installed}, {"ops", OpsToJson(ops)}, {"state", "prepared"}};
    SaveJson(JoinPath(dir, L"tx.json"), txj);
    InstallOutcome r = RunTx(cfg, tx, txj, ops, hist, check.latest.id, message, progress);
    if (r == InstallOutcome::Installed) PruneTransactions(5);
    return r;
}

bool FinalizePending(const Config& cfg, std::wstring& message) {
    std::string tx = PendingTransaction();
    if (tx.empty()) return false;
    json txj = LoadJson(JoinPath(TxDir(tx), L"tx.json"), json::object());
    std::vector<Op> ops = OpsFromJson(txj.value("ops", json::array()));
    json hist = HistoryFor(tx);
    if (hist.empty()) hist = NewHistory(tx, txj.value("kind", "install"), txj.value("release", ""), ops.size());
    json state = LoadState();
    state["pending"] = "";
    std::wstring bad;
    if (!ops.empty() && VerifyTargets(cfg, ops, bad)) {
        state["installed"] = txj.value("release", "");
        SaveState(state);
        txj["state"] = "done";
        SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj);
        message = L"Aktualizace " + Widen(tx) + L" dokončena po restartu (" + std::to_wstring(ops.size()) + L" souborů).";
        FinishHistory(hist, "installed", message);
        PruneTransactions(5);
        return true;
    }
    SaveState(state);
    txj["state"] = "failed";
    SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj);
    message = L"Po restartu nesouhlasí " + bad + L"; vracím původní soubory.";
    FinishHistory(hist, "failed", message);
    std::wstring rb;
    InstallOutcome r = RevertTx(cfg, tx, "auto-rollback", rb, nullptr);
    message += L" " + rb;
    if (r == InstallOutcome::PendingRestart) RestartSystem(rb);
    return true;
}

bool CancelPending(std::wstring& message) {
    std::string tx = PendingTransaction();
    if (tx.empty()) { message = L"Žádná aktualizace nečeká na restart."; return false; }
    json txj = LoadJson(JoinPath(TxDir(tx), L"tx.json"), json::object());
    std::vector<std::wstring> deletes;
    Config cfg = LoadConfig();
    for (auto& o : OpsFromJson(txj.value("ops", json::array())))
        if (o.staged.empty()) deletes.push_back(TargetPath(cfg, o.path));
    UnscheduleRenames(tx, deletes);
    json state = LoadState();
    state["pending"] = "";
    SaveState(state);
    txj["state"] = "cancelled";
    SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj);
    json hist = HistoryFor(tx);
    message = L"Čekající aktualizace " + Widen(tx) + L" je zrušená; systém zůstává beze změny.";
    if (!hist.empty()) FinishHistory(hist, "cancelled", message);
    return true;
}

InstallOutcome RollbackLast(const Config& cfg, std::wstring& message, const Progress& progress) {
    if (!PendingTransaction().empty()) { message = L"Nejdřív je potřeba restartovat kvůli čekající aktualizaci."; return InstallOutcome::Failed; }
    auto history = LoadJson(JoinPath(DataDir(), L"history.json"), json::array());
    for (size_t i = history.size(); i-- > 0;) {
        const json& e = history[i];
        if (e.value("kind", "") == "install" && e.value("result", "") == "installed" && !e.contains("rolledBackBy"))
            return RevertTx(cfg, e.value("tx", ""), "rollback", message, progress);
    }
    message = L"V historii není žádná aktualizace, kterou by šlo vrátit.";
    return InstallOutcome::NothingToDo;
}

bool EnsureFinalizeService(std::wstring& error) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring bin = L"\"" + std::wstring(exe) + L"\" /service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) { error = L"Správce služeb: " + LastErrorText(GetLastError()); return false; }
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"LiberNT Update", SERVICE_CHANGE_CONFIG, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_CHANGE_CONFIG);
        if (svc)
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, bin.c_str(), nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr);
    }
    bool ok = svc != nullptr;
    if (!ok) error = L"Služba LiberNTUpdate: " + LastErrorText(GetLastError());
    if (svc) {
        SERVICE_DESCRIPTIONW d{const_cast<LPWSTR>(L"Dokončí a ověří aktualizace LiberNT po restartu, při chybě je vrátí.")};
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool RestartSystem(std::wstring& error) {
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp{1};
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    if (ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_UPGRADE | SHTDN_REASON_FLAG_PLANNED)) return true;
    error = L"Restart: " + LastErrorText(GetLastError());
    return false;
}

bool SelfTest(std::wstring& report) {
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

    // Transaction logic against a scratch system drive and a file:/// feed.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = JoinPath(tmp, L"lntu-selftest-" + Widen(NowId()));
    std::wstring root = JoinPath(base, L"root"), feed = JoinPath(base, L"feed");
    g_dataOverride = JoinPath(base, L"data");
    Config cfg{L"file:///" + feed + L"\\", root};
    std::replace(cfg.feedUrl.begin(), cfg.feedUrl.end(), L'\\', L'/');
    auto put = [](const std::wstring& path, const std::string& text) { return WriteTextAtomic(path, text); };
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
    check(r == InstallOutcome::Installed && msg == L"1 souborů nahrazeno." &&
              !FileExists(JoinPath(root, L"Windows\\system32\\added.dll")),
          L"partial update: rollback touches only the changed file: " + msg);

    // A file in use forces the restart path; without admin rights scheduling fails cleanly instead.
    HANDLE lock = CreateFileW(JoinPath(root, L"Windows\\system32\\keep.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CheckResult c2;
    c2.latest.id = "R3";
    c2.changes = {{"Windows/system32/keep.dll", obj("keep v2"), 7, Sha256Hex("keep", 4)}};
    r = InstallUpdate(cfg, c2, msg, nullptr);
    if (r == InstallOutcome::PendingRestart) {
        check(PendingTransaction().size() > 0, L"restart path: transaction pending");
        // Cancel it again so the self-test leaves no rename behind.
        std::string tx = PendingTransaction();
        UnscheduleRenames(tx, {});
        json st = LoadState();
        st["pending"] = "";
        SaveState(st);
        HKEY key;
        bool left = false;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSessionManager, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            DWORD size = 0;
            std::vector<wchar_t> v(1 << 16);
            size = (DWORD)(v.size() * sizeof(wchar_t));
            if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, nullptr, (BYTE*)v.data(), &size) == ERROR_SUCCESS) {
                std::wstring all(v.data(), size / sizeof(wchar_t));
                std::transform(all.begin(), all.end(), all.begin(), ::towlower);
                std::wstring m = L"\\tx\\" + Widen(tx) + L"\\";
                std::transform(m.begin(), m.end(), m.begin(), ::towlower);
                left = all.find(m) != std::wstring::npos;
            }
            RegCloseKey(key);
        }
        check(!left, L"restart path: scheduled renames removed again");
    } else {
        check(r == InstallOutcome::Failed && PendingTransaction().empty(), L"restart path without admin rights fails cleanly: " + msg);
    }
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    check(LoadHistory().size() >= 3, L"history has the transactions");

    // Full check against a signed file:/// feed: MP kernel variant, a file Setup left out, a new file.
    {
        std::vector<uint8_t> seed, sk(64), pk(32), sigb(64);
        HexToBytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
        crypto_ed25519_key_pair(sk.data(), pk.data(), seed.data());
        static const char* hexd = "0123456789abcdef";
        std::string pkhex;
        for (uint8_t v : pk) { pkhex += hexd[v >> 4]; pkhex += hexd[v & 15]; }
        g_testPubKey = pkhex;
        auto manifest = [&](const std::string& id, const std::vector<std::pair<std::string, std::string>>& fl) {
            json m{{"id", id}, {"commit", id}, {"date", "2026-01-01T00:00:00Z"}, {"files", json::array()},
                   {"aliases", json::array({json{{"path", "Windows/system32/ntoskrnl.exe"},
                                                 {"candidates", {"Windows/system32/ntoskrnl.exe", "Windows/system32/ntkrnlmp.exe"}}}})}};
            for (auto& [p, content] : fl) m["files"].push_back({{"path", p}, {"sha256", obj(content)}, {"size", content.size()}});
            std::string raw = m.dump(1);
            put(JoinPath(feed, L"releases\\" + Widen(id) + L"\\manifest.json"), raw);
            return json{{"id", id}, {"commit", id}, {"date", "2026-01-01T00:00:00Z"}, {"manifest", "releases/" + id + "/manifest.json"},
                        {"manifestSha256", Sha256Hex(raw.data(), raw.size())}, {"notes", json::array({"test"})}};
        };
        json f{{"product", "LiberNT"}, {"format", 1}, {"releases", json::array()}};
        f["releases"].push_back(manifest("V1", {{"Windows/system32/ntoskrnl.exe", "UP1"}, {"Windows/system32/ntkrnlmp.exe", "MP1"},
                                                {"Windows/system32/a.dll", "A1"}, {"Windows/system32/opt.exe", "OPT"}}));
        f["releases"].push_back(manifest("V2", {{"Windows/system32/ntoskrnl.exe", "UP2"}, {"Windows/system32/ntkrnlmp.exe", "MP2"},
                                                {"Windows/system32/a.dll", "A2"}, {"Windows/system32/opt.exe", "OPT"},
                                                {"Windows/system32/new.dll", "NEW"}}));
        std::string fraw = f.dump(1);
        put(JoinPath(feed, L"feed.json"), fraw);
        crypto_ed25519_sign(sigb.data(), sk.data(), (const uint8_t*)fraw.data(), fraw.size());
        std::string sighex;
        for (uint8_t v : sigb) { sighex += hexd[v >> 4]; sighex += hexd[v & 15]; }
        put(JoinPath(feed, L"feed.json.sig"), sighex);

        std::wstring root2 = JoinPath(base, L"root2");
        put(JoinPath(root2, L"Windows\\system32\\ntoskrnl.exe"), "MP1");  // Setup installed the MP kernel
        put(JoinPath(root2, L"Windows\\system32\\ntkrnlmp.exe"), "MP1");
        put(JoinPath(root2, L"Windows\\system32\\a.dll"), "A1");
        Config cfg2{cfg.feedUrl, root2};
        json st = LoadState();
        st["installed"] = "";
        SaveState(st);
        CheckResult cr;
        std::wstring err;
        bool got = CheckForUpdate(cfg2, cr, err, nullptr);
        check(got, L"signed feed accepted: " + err);
        std::map<std::string, std::string> ch;
        for (auto& x : cr.changes) ch[x.path] = x.newSha;
        check(cr.changes.size() == 4, L"4 changes found (got " + std::to_wstring(cr.changes.size()) + L")");
        check(ch["Windows/system32/ntoskrnl.exe"] == Sha256Hex("MP2", 3), L"kernel keeps the MP variant");
        check(!ch.count("Windows/system32/opt.exe"), L"file left out by Setup is not added");
        check(ch.count("Windows/system32/new.dll") == 1, L"file new in the release is added");
        InstallOutcome r2 = InstallUpdate(cfg2, cr, msg, nullptr);
        check(r2 == InstallOutcome::Installed && ReadFileBytes(JoinPath(root2, L"Windows\\system32\\ntoskrnl.exe"), b) &&
                  std::string(b.begin(), b.end()) == "MP2",
              L"installed kernel is the new MP kernel");
        CheckResult again;
        CheckForUpdate(cfg2, again, err, nullptr);
        check(again.changes.empty(), L"second check finds nothing to do");

        // The new MP kernel copied over ntoskrnl.exe by hand stays; it is not swapped for the UP one.
        std::wstring root3 = JoinPath(base, L"root3");
        put(JoinPath(root3, L"Windows\\system32\\ntoskrnl.exe"), "MP2");
        put(JoinPath(root3, L"Windows\\system32\\ntkrnlmp.exe"), "MP1");
        CheckResult hand;
        CheckForUpdate(Config{cfg.feedUrl, root3}, hand, err, nullptr);
        bool kernelTouched = false;
        for (auto& x : hand.changes) kernelTouched = kernelTouched || x.path == "Windows/system32/ntoskrnl.exe";
        check(!kernelTouched, L"kernel already at the new MP version is kept");
        put(JoinPath(feed, L"feed.json"), fraw + " ");
        check(!CheckForUpdate(cfg2, again, err, nullptr), L"tampered feed rejected");
        g_testPubKey.clear();
    }

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
