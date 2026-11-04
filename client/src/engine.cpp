#include "engine.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <ctime>
#include <map>

#include "feed_internal.h"
#include "http.h"
#include "runtime.h"
#include "sha256.h"
#include "util.h"

#include "../boot/bootplan.h"
extern "C" {
#include "../third_party/monocypher/monocypher-ed25519.h"
}
#include "../third_party/nlohmann/json.hpp"

using json = nlohmann::json;

bool StartSelfUpdateWatchdog(const std::string& tx);
bool g_selfUpdateStarted = false;

namespace {

std::wstring g_dataOverride;  // set by SelfTest to keep its state away from the real one
std::string g_testBootId;
std::string BootIdentity() { return g_testBootId.empty() ? CurrentBootId() : g_testBootId; }

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

/// Fetches a file of the feed: over HTTP, or from a directory for file:/// feeds (self-test, offline media).
bool Fetch(const Config& cfg, const std::string& rel, std::vector<uint8_t>& out, std::wstring& error, size_t limit) {
    std::wstring url = cfg.feedUrl + Widen(rel);
    if (cfg.feedUrl.rfind(L"file:///", 0) == 0) {
        std::wstring path = url.substr(8);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (ReadFileBytes(path, out, limit)) return true;
        error = path + L": " + LastErrorText(GetLastError());
        return false;
    }
    return HttpGet(url, out, error, limit);
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
    if (!SaveJson(path, h)) LogLine("cannot save history.json: " + Narrow(LastErrorText(GetLastError())));
}

json HistoryFor(const std::string& tx) {
    for (auto& e : LoadJson(JoinPath(DataDir(), L"history.json"), json::array()))
        if (e.value("tx", "") == tx) return e;
    return json::object();
}

std::wstring OwnExe() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return exe;
}

/// The client's folder (manifest path): its running exes can be renamed aside and replaced without a restart.
bool IsClientFile(const std::string& manifestPath) {
    static const char prefix[] = "program files/libernt update/";
    return manifestPath.size() > sizeof prefix - 1 && _strnicmp(manifestPath.c_str(), prefix, sizeof prefix - 1) == 0;
}

/// A file someone has open or mapped (a loaded DLL, the running kernel) cannot be replaced now.
bool InUse(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
    DWORD e = GetLastError();
    return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
}

std::wstring BootDir() { return JoinPath(DataDir(), L"boot"); }
std::wstring BootFile(const wchar_t* name) { return JoinPath(BootDir(), name); }
std::wstring NtPath(const std::wstring& win) {
    return win.rfind(L"\\\\?\\", 0) == 0 ? L"\\??\\" + win.substr(4) : L"\\??\\" + win;
}

bool WriteUtf16(const std::wstring& path, const std::wstring& text) {
    std::string bytes("\xFF\xFE", 2);
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    return WriteTextAtomic(path, bytes);
}

/// Boot entry in freeldr.ini that starts the kernel and HAL from before the update with LNTUROLLBACK, which tells
/// luupboot.exe to undo the update. Only BIOS installs boot through freeldr.ini; elsewhere this does nothing.
const wchar_t kRecoverySection[] = L"LiberNTUpdateRecovery";

std::wstring FreeldrIni(const Config& cfg) { return JoinPath(cfg.root, L"freeldr.ini"); }

void RemoveRecoveryEntry(const Config& cfg) {
    std::wstring ini = FreeldrIni(cfg);
    if (!FileExists(ini)) return;
    DWORD attrs = GetFileAttributesW(ini.c_str());
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
    WritePrivateProfileStringW(L"Operating Systems", kRecoverySection, nullptr, ini.c_str());
    WritePrivateProfileStringW(kRecoverySection, nullptr, nullptr, ini.c_str());
    SetFileAttributesW(ini.c_str(), attrs);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    DeleteFileW(JoinPath(sys, L"lntuprev.exe").c_str());
    DeleteFileW(JoinPath(sys, L"lntuphal.dll").c_str());
}

bool AddRecoveryEntry(const Config& cfg) {
    std::wstring ini = FreeldrIni(cfg);
    if (!FileExists(ini)) return true;
    wchar_t sys[MAX_PATH], def[128], type[64], path[260], opts[512];
    GetSystemDirectoryW(sys, MAX_PATH);
    if (!CopyFileW(JoinPath(sys, L"ntoskrnl.exe").c_str(), JoinPath(sys, L"lntuprev.exe").c_str(), FALSE) ||
        !CopyFileW(JoinPath(sys, L"hal.dll").c_str(), JoinPath(sys, L"lntuphal.dll").c_str(), FALSE)) {
        LogLine("recovery entry skipped: cannot copy the current kernel and HAL: " + Narrow(LastErrorText(GetLastError())));
        return false;
    }
    GetPrivateProfileStringW(L"FREELOADER", L"DefaultOS", L"", def, 128, ini.c_str());
    GetPrivateProfileStringW(def, L"BootType", L"Windows2003", type, 64, ini.c_str());
    GetPrivateProfileStringW(def, L"SystemPath", L"", path, 260, ini.c_str());
    GetPrivateProfileStringW(def, L"Options", L"", opts, 512, ini.c_str());
    if (!*path) return false;
    DWORD attrs = GetFileAttributesW(ini.c_str());
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
    bool ok = WritePrivateProfileStringW(L"Operating Systems", kRecoverySection, L"\"LiberNT (undo the last update)\"", ini.c_str()) &&
              WritePrivateProfileStringW(kRecoverySection, L"BootType", type, ini.c_str()) &&
              WritePrivateProfileStringW(kRecoverySection, L"SystemPath", path, ini.c_str());
    std::wstring o = std::wstring(opts) + L" /KERNEL=lntuprev.exe /HAL=lntuphal.dll /LNTUROLLBACK";
    ok = ok && WritePrivateProfileStringW(kRecoverySection, L"Options", o.c_str(), ini.c_str()) &&
         WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    SetFileAttributesW(ini.c_str(), attrs);
    return ok;
}

/// Installs luupboot.exe into system32 and registers it in BootExecute after autochk.
bool EnsureBootApplier(std::wstring& error) {
    wchar_t exe[MAX_PATH], sys[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring src = JoinPath(std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\')), L"luupboot.exe");
    std::wstring dst = JoinPath(sys, L"luupboot.exe");
    if (!FileExists(src)) { error = src + Tr(L" chybí.", L" is missing."); return false; }
    if (Sha256File(src) != Sha256File(dst) && !CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        error = Tr(L"Nelze nainstalovat ", L"Cannot install ") + dst + L": " + LastErrorText(GetLastError());
        return false;
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSessionManager, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        error = Tr(L"Nelze otevřít Session Manager v registru.", L"Cannot open Session Manager in the registry.");
        return false;
    }
    DWORD size = 0, type = 0;
    std::vector<std::wstring> items;
    LONG query = RegQueryValueExW(key, L"BootExecute", nullptr, &type, nullptr, &size);
    if (query != ERROR_FILE_NOT_FOUND && (query != ERROR_SUCCESS || type != REG_MULTI_SZ)) {
        error = Tr(L"Nelze bezpečně přečíst BootExecute.", L"BootExecute cannot be read safely.");
        RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (query == ERROR_SUCCESS) {
        query = RegQueryValueExW(key, L"BootExecute", nullptr, &type, (BYTE*)buf.data(), &size);
        if (query != ERROR_SUCCESS || type != REG_MULTI_SZ) {
            error = Tr(L"Nelze bezpečně přečíst BootExecute.", L"BootExecute cannot be read safely.");
            RegCloseKey(key);
            return false;
        }
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) items.emplace_back(p);
    }
    std::wstring entry = L"luupboot " + NtPath(DataDir());
    std::vector<std::wstring> next;
    for (auto& s : items)
        if (_wcsnicmp(s.c_str(), L"luupboot", 8) != 0) next.push_back(s);
    next.push_back(entry);
    bool ok = true;
    if (next != items) {
        std::wstring blob;
        for (auto& s : next) { blob += s; blob.push_back(L'\0'); }
        blob.push_back(L'\0');
        ok = RegSetValueExW(key, L"BootExecute", 0, REG_MULTI_SZ, (const BYTE*)blob.data(), (DWORD)(blob.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
        if (ok) RegFlushKey(key);
        else error = Tr(L"Nelze zapsat BootExecute.", L"Cannot write BootExecute.");
    }
    RegCloseKey(key);
    return ok;
}

struct Op {
    std::string path, newSha, oldSha;
    std::wstring staged;  ///< file inside the transaction to move into place; empty = delete the target
    std::wstring backup;  ///< copy of the previous file; empty = the file did not exist
    std::wstring target;  ///< disk path fixed when the transaction was made; later stages never map path again
};

/// Disk path of an op. Transactions written before target was recorded fall back to mapping the manifest path.
std::wstring Target(const Config& cfg, const Op& o) { return o.target.empty() ? TargetPath(cfg, o.path) : o.target; }

json OpsToJson(const std::vector<Op>& ops) {
    json a = json::array();
    for (auto& o : ops)
        a.push_back({{"path", o.path}, {"newSha", o.newSha}, {"oldSha", o.oldSha}, {"staged", Narrow(o.staged)}, {"backup", Narrow(o.backup)},
                     {"target", Narrow(o.target)}});
    return a;
}

std::vector<Op> OpsFromJson(const json& a) {
    std::vector<Op> ops;
    for (auto& j : a)
        ops.push_back({j.value("path", ""), j.value("newSha", ""), j.value("oldSha", ""), Widen(j.value("staged", "")), Widen(j.value("backup", "")),
                       Widen(j.value("target", ""))});
    return ops;
}

/// Puts back the backups of the ops already applied (in reverse) after a failure in the middle. Every step goes to
/// update.log; the returned text is empty when all files are back and names the ones that are not otherwise.
std::wstring UndoDirect(const Config& cfg, const std::vector<Op>& ops, size_t applied, const std::string& tx) {
    size_t failed = 0;
    std::wstring first;
    for (size_t i = applied; i-- > 0;) {
        std::wstring target = Target(cfg, ops[i]);
        std::string have = FileExists(target) ? Sha256File(target) : "";
        if (have == ops[i].oldSha) continue;
        if (have != ops[i].newSha) {
            if (!failed++) first = Widen(ops[i].path);
            LogLine("undo conflict preserved: " + ops[i].path);
            continue;
        }
        BOOL ok = !ops[i].backup.empty() ? CopyFileW(ops[i].backup.c_str(), target.c_str(), FALSE)
                                         : (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND);
        if (ok && (ops[i].backup.empty() || Sha256File(target) == ops[i].oldSha)) continue;
        LogLine("tx " + tx + ": UNDO FAILED for " + ops[i].path + ": " + Narrow(LastErrorText(GetLastError())) +
                " (backup " + Narrow(ops[i].backup) + ")");
        if (!failed++) first = Widen(ops[i].path);
    }
    LogLine("tx " + tx + ": undo of " + std::to_string(applied) + " files, " + std::to_string(failed) + " failed");
    if (!failed) return Tr(L" Původní soubory jsou vrácené.", L" The previous files are back.");
    return Tr(L" POZOR: nepodařilo se vrátit souborů: ", L" WARNING: files that could not be restored: ") + std::to_wstring(failed) +
           Tr(L" (první ", L" (first ") + first + Tr(L"), zálohy zůstávají v ", L"); the backups stay in ") + TxDir(tx) +
           Tr(L" a podrobnosti jsou v update.log.", L" and update.log has the details.");
}

/// Checks every replaced file against its expected hash; logs each mismatch, bad = the first one and the count.
bool VerifyTargets(const Config& cfg, const std::vector<Op>& ops, std::wstring& bad) {
    size_t mismatches = 0;
    for (auto& o : ops) {
        std::wstring target = Target(cfg, o);
        std::string have = FileExists(target) ? Sha256File(target) : "";
        if (have == o.newSha) continue;
        LogLine("verify: " + o.path + " has " + (have.empty() ? std::string("no file") : have.substr(0, 16)) + ", expected " +
                (o.newSha.empty() ? std::string("no file") : o.newSha.substr(0, 16)));
        if (!mismatches++) bad = Widen(o.path);
    }
    if (mismatches > 1) bad += Tr(L" a další soubory: ", L" and more files: ") + std::to_wstring(mismatches - 1);
    return mismatches == 0;
}

void ClearBootFiles() {
    for (const wchar_t* f : {L"plan.txt", L"rollback.txt", L"plan.done", L"rollback.done", L"plan.cancelled", L"trial", L"result.txt",
                             L"rolledback", L"plan.log", L"rollback.log", L"applying", L"committed"})
        DeleteFileW(BootFile(f).c_str());
}

/// Copies the lines luupboot.exe could not apply (UTF-16 plan.log / rollback.log) into update.log.
void LogBootFailures() {
    for (const wchar_t* f : {L"plan.log", L"rollback.log"}) {
        std::vector<uint8_t> raw;
        if (!ReadFileBytes(BootFile(f), raw) || raw.size() < 4) continue;
        std::wstring text(reinterpret_cast<const wchar_t*>(raw.data()) + 1, (raw.size() - 2) / sizeof(wchar_t));
        size_t pos = 0, next;
        while ((next = text.find(L'\n', pos)) != std::wstring::npos) {
            LogLine("luupboot " + Narrow(f) + ": " + Narrow(text.substr(pos, next - pos)));
            pos = next + 1;
        }
    }
}

/// Writes the plan luupboot.exe applies at the next boot and the plan that undoes it. Paths are NT paths.
bool WriteBootPlans(const Config& cfg, const std::vector<Op>& ops, std::wstring& error) {
    std::wstring plan = L"LUUP 1\n", undo = L"LUUP 1\n";
    for (auto& o : ops) {
        std::wstring target = NtPath(Target(cfg, o));
        std::wstring expected = o.backup.empty() ? L"-" : NtPath(o.backup);
        plan += o.staged.empty() ? L"X\t" + target + L"\t" + expected + L"\n"
                                : L"R\t" + NtPath(o.staged) + L"\t" + target + L"\t" + expected + L"\n";
    }
    for (size_t i = ops.size(); i-- > 0;) {
        std::wstring target = NtPath(Target(cfg, ops[i]));
        std::wstring expected = ops[i].staged.empty() ? L"-" : NtPath(ops[i].staged);
        undo += ops[i].backup.empty() ? L"X\t" + target + L"\t" + expected + L"\n"
                                     : L"R\t" + NtPath(ops[i].backup) + L"\t" + target + L"\t" + expected + L"\n";
    }
    ClearBootFiles();
    // The plan goes last: its presence is what makes luupboot act.
    if (!EnsureDirs(BootFile(L"plan.txt"), true) || !WriteUtf16(BootFile(L"rollback.txt"), undo) || !WriteUtf16(BootFile(L"plan.txt"), plan)) {
        error = Tr(L"Nelze zapsat plán aktualizace do ", L"Cannot write the update plan to ") + BootDir() + L": " + LastErrorText(GetLastError());
        ClearBootFiles();
        return false;
    }
    return true;
}

/// Applies a prepared transaction: directly when nothing is in use, otherwise everything at the next restart
/// (luupboot.exe, before any Win32 process starts), so the running system never mixes old and new core files.
InstallOutcome ApplyTx(const Config& cfg, const std::string& tx, json& txj, std::vector<Op>& ops, std::wstring& message,
                       const Progress& progress) {
    bool restart = false;
    for (auto& o : ops) {
        std::wstring t = Target(cfg, o);
        if (InUse(t) && !IsClientFile(o.path)) { restart = true; break; }
    }
    txj["mode"] = restart ? "restart" : "direct";
    txj["createdBoot"] = BootIdentity();
    if (!SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj)) {
        message = Tr(L"Nelze uložit stav připravené aktualizace.", L"The prepared update state cannot be saved.");
        return InstallOutcome::Failed;
    }

    for (size_t i = 0; i < ops.size(); i++) {
        auto& o = ops[i];
        std::wstring target = Target(cfg, o);
        if (progress)
            progress((int)(70 + 25 * i / std::max<size_t>(ops.size(), 1)),
                     (o.staged.empty() ? Tr(L"Odstraňuji ", L"Removing ") : Tr(L"Nahrazuji ", L"Replacing ")) + Widen(o.path));
        // luupboot cannot create folders, so they exist before the restart.
        if (!EnsureDirs(target, true)) {
            message = Tr(L"Nelze vytvořit složku pro ", L"Cannot create the folder for ") + Widen(o.path) + L": " + LastErrorText(GetLastError());
            LogLine("tx " + tx + ": " + Narrow(message));
            if (!restart) message += UndoDirect(cfg, ops, i, tx);
            return InstallOutcome::Failed;
        }
        if (restart) continue;
        std::string current = FileExists(target) ? Sha256File(target) : "";
        if (current != o.oldSha) {
            message = L"Update target changed before replacement: " + Widen(o.path);
            message += UndoDirect(cfg, ops, i, tx);
            return InstallOutcome::Failed;
        }
        // The running client: Windows lets a mapped exe be renamed, so it moves aside and the new one takes its name.
        std::wstring aside;
        if (!o.staged.empty() && IsClientFile(o.path) && InUse(target)) {
            aside = target + L".old-" + Widen(tx);
            if (!MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                message = Tr(L"Nelze odsunout běžící ", L"Cannot move aside the running ") + Widen(o.path) + L": " + LastErrorText(GetLastError());
                LogLine("tx " + tx + ": " + Narrow(message));
                message += UndoDirect(cfg, ops, i, tx);
                return InstallOutcome::Failed;
            }
            LogLine("tx " + tx + ": running " + o.path + " moved aside to " + Narrow(aside));
        }
        BOOL ok = o.staged.empty() ? (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
                                   : MoveFileExW(o.staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
        if (!ok) {
            message = Tr(L"Nelze nahradit ", L"Cannot replace ") + Widen(o.path) + L": " + LastErrorText(GetLastError());
            LogLine("tx " + tx + ": " + Narrow(message));
            if (!aside.empty() && !MoveFileExW(aside.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
                LogLine("tx " + tx + ": UNDO FAILED, running exe stays at " + Narrow(aside) + ": " + Narrow(LastErrorText(GetLastError())));
            message += UndoDirect(cfg, ops, i, tx);
            return InstallOutcome::Failed;
        }
    }
    if (restart) {
        // Nothing on disk has changed yet, so a failure here leaves the system as it was.
        if ((g_dataOverride.empty() && (!EnsureBootApplier(message) || !AddRecoveryEntry(cfg))) || !WriteBootPlans(cfg, ops, message)) {
            if (message.empty()) message = L"Cannot prepare the recovery boot entry.";
            LogLine("tx " + tx + ": cannot schedule the restart-time update: " + Narrow(message));
            return InstallOutcome::Failed;
        }
        return InstallOutcome::PendingRestart;
    }

    std::wstring bad;
    if (!VerifyTargets(cfg, ops, bad)) {
        message = Tr(L"Kontrola po nahrazení selhala u ", L"Verification after the replacement failed for ") + bad +
                  Tr(L"; vracím původní soubory.", L"; restoring the previous files.");
        LogLine("tx " + tx + ": " + Narrow(message));
        message += UndoDirect(cfg, ops, ops.size(), tx);
        return InstallOutcome::Failed;
    }
    return InstallOutcome::Installed;
}

/// Keeps the transaction folders of the last few updates (their backups make rollback possible).
void PruneTransactions(size_t keep, const std::string& protect = "") {
    std::string guard = SelfUpdateGuardTransaction();
    if (guard == "?") return;
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
        std::string id = Narrow(dirs[i]);
        if (id == pending || id == protect || id == guard) continue;
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
    txj["state"] = "applying";
    json activeState = LoadState();
    activeState["pending"] = tx;
    if (!SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj) || !SaveState(activeState)) {
        message = Tr(L"Nelze uložit aktivní transakci. Soubory nebyly změněny.", L"Cannot save the active transaction. No files were changed.");
        return InstallOutcome::Failed;
    }
    InstallOutcome r = ApplyTx(cfg, tx, txj, ops, message, progress);
    json state = LoadState();
    if (r == InstallOutcome::Installed) {
        txj["state"] = "done";
        state["pending"] = "";
        state["installed"] = releaseAfter;
        if (message.empty()) message = Tr(L"Nahrazeno souborů: ", L"Files replaced: ") + std::to_wstring(ops.size()) + L".";
        FinishHistory(hist, "installed", message);
    } else if (r == InstallOutcome::PendingRestart) {
        txj["state"] = "pending";
        state["pending"] = tx;
        message = Tr(L"Při restartu se nahradí souborů: ", L"Files to replace at the restart: ") + std::to_wstring(ops.size()) + L".";
        hist["result"] = "pending-restart";
        hist["message"] = Narrow(message);
        AddHistory(hist);
        LogLine("tx " + tx + " pending restart");
    } else {
        txj["state"] = "failed";
        state["pending"] = "";
        if (txj.value("mode", "") == "direct") {
            std::vector<Op> original = ops;
            for (auto& op : original) op.newSha = op.oldSha;
            std::wstring mismatch;
            if (!VerifyTargets(cfg, original, mismatch)) {
                state["pending"] = tx;
                txj["state"] = "recovering";
            }
        }
        FinishHistory(hist, "failed", message);
    }
    bool savedTx = SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj);
    bool savedState = SaveState(state);
    if ((!savedTx || !savedState) && (r == InstallOutcome::Installed || r == InstallOutcome::PendingRestart)) {
        std::wstring persistence = Tr(L"Nelze bezpečně uložit stav aktualizace.", L"The update state cannot be saved safely.");
        if (r == InstallOutcome::Installed) persistence += UndoDirect(cfg, ops, ops.size(), tx);
        else {
            ClearBootFiles();
            RemoveRecoveryEntry(cfg);
        }
        state["installed"] = txj.value("installedBefore", "");
        std::vector<Op> original = ops;
        for (auto& op : original) op.newSha = op.oldSha;
        std::wstring mismatch;
        state["pending"] = VerifyTargets(cfg, original, mismatch) ? "" : tx;
        SaveState(state);
        txj["state"] = state.value("pending", "").empty() ? "failed" : "recovering";
        SaveJson(JoinPath(TxDir(tx), L"tx.json"), txj);
        message = persistence;
        FinishHistory(hist, "failed", message);
        if (progress) progress(0, message);
        return InstallOutcome::Failed;
    }
    if (progress) progress(100, message);
    return r;
}

/// Builds and runs a transaction that restores the backups of tx `of` (manual rollback or failed verification).
InstallOutcome RevertTx(const Config& cfg, const std::string& of, const std::string& kind, std::wstring& message,
                        const Progress& progress) {
    json src = LoadJson(JoinPath(TxDir(of), L"tx.json"), json::object());
    if (!src.contains("ops")) { message = Tr(L"Záloha aktualizace ", L"The backup of update ") + Widen(of) + Tr(L" už neexistuje.", L" no longer exists."); return InstallOutcome::Failed; }
    std::vector<Op> old = OpsFromJson(src["ops"]);
    std::string tx = NewTxId(kind);
    std::wstring dir = TxDir(tx);
    EnsureDirs(dir, false);
    std::vector<Op> ops;
    size_t conflicts = 0;
    for (size_t i = old.size(); i-- > 0;) {
        Op o;
        o.path = old[i].path;
        o.oldSha = old[i].newSha;
        o.newSha = old[i].oldSha;
        o.target = Target(cfg, old[i]);
        std::wstring target = o.target;
        bool exists = FileExists(target);
        std::string have = exists ? Sha256File(target) : "";
        // Files the update never reached are already as they were; touching them again only adds risk.
        if (have == o.newSha) continue;
        // Only what the update itself wrote is undone: a file that is neither the update's nor the original
        // (changed since, or another file mapped to the same name) is left alone instead of being deleted.
        if (have != o.oldSha) {
            conflicts++;
            LogLine("tx " + tx + ": " + o.path + " (" + Narrow(target) + ") is not the file the update wrote (" +
                    (have.empty() ? std::string("missing") : have.substr(0, 16)) + "), left as it is");
            continue;
        }
        if (!old[i].backup.empty()) {
            // Restart-time moves consume their source, so restore from a copy and keep the original backup.
            o.staged = JoinPath(dir, L"new\\" + std::to_wstring(i) + L".bin");
            if (!EnsureDirs(o.staged, true) || !CopyFileW(old[i].backup.c_str(), o.staged.c_str(), FALSE) ||
                Sha256File(o.staged) != o.newSha) {
                message = Tr(L"Záloha souboru ", L"The backup of ") + Widen(o.path) + Tr(L" je poškozená nebo chybí.", L" is damaged or missing.");
                return InstallOutcome::Failed;
            }
        }
        if (exists) {
            o.backup = JoinPath(dir, L"backup\\" + std::to_wstring(i) + L".bin");
            if (!EnsureDirs(o.backup, true) || !CopyFileW(target.c_str(), o.backup.c_str(), FALSE)) {
                message = Tr(L"Nelze zálohovat ", L"Cannot back up ") + Widen(o.path) + L": " + LastErrorText(GetLastError());
                return InstallOutcome::Failed;
            }
        }
        ops.push_back(o);
    }
    std::string releaseBefore = conflicts ? "" : src.value("installedBefore", "");
    if (ops.empty()) {
        message = Tr(L"Žádný soubor už nelze bezpečně vrátit; systém zůstal beze změny.",
                     L"No file can be safely undone now; the system was left unchanged.");
        return InstallOutcome::NothingToDo;
    }
    json txj{{"tx", tx}, {"kind", kind}, {"of", of}, {"release", releaseBefore}, {"installedBefore", LoadState().value("installed", "")},
             {"ops", OpsToJson(ops)}, {"state", "prepared"}};
    txj["conflicts"] = conflicts;
    json hist = NewHistory(tx, kind, releaseBefore, ops.size());
    hist["of"] = of;
    InstallOutcome r = RunTx(cfg, tx, txj, ops, hist, releaseBefore, message, progress);
    if (r == InstallOutcome::Installed && conflicts) {
        message += Tr(L" Konfliktní soubory ponechány: ", L" Conflicting files kept: ") + std::to_wstring(conflicts) +
                   Tr(L". Výchozí vydání nelze potvrdit.", L". The previous release cannot be confirmed.");
        FinishHistory(hist, "partial-rollback", message);
    }
    if (r == InstallOutcome::Installed || r == InstallOutcome::PendingRestart) {
        json orig = HistoryFor(of);
        if (!orig.empty()) { orig["rolledBackBy"] = tx; AddHistory(orig); }
    }
    return r;
}

}  // namespace

bool IsTestData() { return !g_dataOverride.empty(); }

std::wstring DataDir() {
    if (!g_dataOverride.empty()) return g_dataOverride;
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path))) return JoinPath(path, L"LiberNTUpdate");
    return L"C:\\ProgramData\\LiberNTUpdate";
}

bool UiCzech() {
    static int lang = -1;  // decided once per process: config.json, then the Windows UI language
    if (lang < 0) {
        json cfg = LoadJson(JoinPath(DataDir(), L"config.json"), json::object());
        std::string want = cfg.contains("language") && cfg["language"].is_string() ? cfg["language"].get<std::string>() : "";
        if (want == "cs") lang = 1;
        else if (want == "en") lang = 0;
        else lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CZECH ? 1 : 0;
    }
    return lang == 1;
}

Config LoadConfig() {
#ifndef LNTU_FEED_URL
#define LNTU_FEED_URL L""
#endif
    Config c{LNTU_FEED_URL, L"C:\\"};
    json j = LoadJson(JoinPath(DataDir(), L"config.json"), json::object());
    try {
        if (j.contains("feedUrl")) c.feedUrl = Widen(j["feedUrl"].get<std::string>());
        if (j.contains("root")) c.root = Widen(j["root"].get<std::string>());
        if (j.contains("bootRoot")) c.bootRoot = Widen(j["bootRoot"].get<std::string>());
        if (j.contains("baseRelease")) c.baseRelease = j["baseRelease"].get<std::string>();
        if (j.contains("auto")) c.autoMode = j["auto"].get<std::string>();
        if (j.contains("checkMinutes")) c.checkMinutes = std::max(1, j["checkMinutes"].get<int>());
        if (j.contains("restartHour")) c.restartHour = j["restartHour"].get<int>();
    } catch (const json::exception& e) {
        LogLine(std::string("config.json: ") + e.what() + "; the remaining settings keep their defaults");
    }
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
        if (e.contains("rolledBackBy")) h.result += Narrow(Tr(L" (vráceno)", L" (undone)"));
        h.files = e.value("files", 0);
        out.push_back(h);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

InstallOutcome InstallUpdate(const Config& cfg, const CheckResult& check, std::wstring& message, const Progress& progress) {
    UpdateLock operation(message);
    if (!operation || !SecureUpdateData(message)) return InstallOutcome::Failed;
    if (InstalledRelease() != check.installed) {
        message = Tr(L"Stav systému se změnil. Zopakujte kontrolu.", L"The system state changed. Check for updates again.");
        return InstallOutcome::Failed;
    }
    if (!PendingTransaction().empty()) { message = Tr(L"Předchozí aktualizace čeká na restart.", L"The previous update waits for a restart."); return InstallOutcome::Failed; }
    if (!SelfUpdateGuardTransaction().empty()) {
        message = Tr(L"Předchozí aktualizaci klienta ještě ověřuje hlídač.", L"The previous client update is still being verified by its watchdog.");
        return InstallOutcome::Failed;
    }
    if (check.changes.empty()) { message = Tr(L"Systém už odpovídá vydání ", L"The system already matches release ") + Widen(check.latest.id) + L"."; return InstallOutcome::NothingToDo; }
    std::string tx = NewTxId("install");
    std::wstring dir = TxDir(tx);
    if (!EnsureDirs(dir, false)) {
        message = Tr(L"Nelze vytvořit složku aktualizace: ", L"Cannot create the update folder: ") + LastErrorText(GetLastError());
        return InstallOutcome::Failed;
    }
    json hist = NewHistory(tx, "install", check.latest.id, check.changes.size());
    bool selfUpdate = false;
    for (auto& change : check.changes)
        if (IsClientFile(change.path) && _stricmp(change.path.substr(change.path.find_last_of('/') + 1).c_str(), "LiberNTUpdate.exe") == 0)
            selfUpdate = true;
    if (selfUpdate && !IsTestData()) SnapshotRunningClient();
    std::vector<Op> ops;
    uint64_t total = 0, done = 0;
    for (auto& c : check.changes) total += c.size;
    for (size_t i = 0; i < check.changes.size(); i++) {
        const Change& c = check.changes[i];
        Op o{c.path, c.newSha, c.oldSha == "unreadable" ? "" : c.oldSha,
             c.newSha.empty() ? L"" : JoinPath(dir, L"new\\" + std::to_wstring(i) + L".bin"), L""};
        if (!c.newSha.empty()) {
            if (progress) progress((int)(60 * done / std::max<uint64_t>(total, 1)), Tr(L"Stahuji ", L"Downloading ") + Widen(c.path));
            std::vector<uint8_t> data;
            std::wstring err;
            if (c.size > 512u * 1024u * 1024u ||
                !Fetch(cfg, "objects/" + c.newSha.substr(0, 2) + "/" + c.newSha, data, err, (size_t)c.size) ||
                data.size() != c.size || Sha256Hex(data.data(), data.size()) != c.newSha) {
                message = Tr(L"Stažení ", L"Download of ") + Widen(c.path) + Tr(L" selhalo: ", L" failed: ") +
                          (err.empty() ? std::wstring(Tr(L"obsah nesouhlasí s manifestem", L"the contents do not match the manifest")) : err);
                FinishHistory(hist, "failed", message);
                return InstallOutcome::Failed;
            }
            if (!WriteFileAtomic(o.staged, data.data(), data.size())) {
                message = Tr(L"Nelze uložit ", L"Cannot save ") + o.staged + L": " + LastErrorText(GetLastError());
                FinishHistory(hist, "failed", message);
                return InstallOutcome::Failed;
            }
        }
        done += c.size;
        std::wstring target = TargetPath(cfg, c.path);
        if (target.empty()) { message = L"Cannot identify update target: " + Widen(c.path); return InstallOutcome::Failed; }
        o.target = target;
        if (FileExists(target)) {
            o.backup = JoinPath(dir, L"backup\\" + std::to_wstring(i) + L".bin");
            if (!EnsureDirs(o.backup, true) || !CopyFileW(target.c_str(), o.backup.c_str(), FALSE)) {
                message = Tr(L"Nelze zálohovat ", L"Cannot back up ") + Widen(c.path) + L": " + LastErrorText(GetLastError());
                FinishHistory(hist, "failed", message);
                return InstallOutcome::Failed;
            }
            o.oldSha = Sha256File(o.backup);
            if (o.oldSha.empty() || o.oldSha != c.oldSha) {
                message = Tr(L"Soubor se od kontroly změnil. Zopakujte kontrolu: ", L"The file changed since the check. Check again: ") + Widen(c.path);
                return InstallOutcome::Failed;
            }
        } else {
            if (!c.oldSha.empty()) { message = L"Update target disappeared after the check: " + Widen(c.path); return InstallOutcome::Failed; }
            o.oldSha.clear();
        }
        ops.push_back(o);
    }
    json txj{{"tx", tx}, {"kind", "install"}, {"release", check.latest.id}, {"commit", check.latest.commit},
             {"installedBefore", InstalledRelease()}, {"ops", OpsToJson(ops)}, {"state", "prepared"}};
    if (!SaveJson(JoinPath(dir, L"tx.json"), txj)) {
        message = Tr(L"Nelze uložit připravenou aktualizaci.", L"The prepared update cannot be saved.");
        FinishHistory(hist, "failed", message);
        return InstallOutcome::Failed;
    }
    if (selfUpdate && !IsTestData() && !RegisterSelfUpdateGuard(tx, message)) return InstallOutcome::Failed;
    InstallOutcome r = RunTx(cfg, tx, txj, ops, hist, check.latest.id, message, progress);
    if (r == InstallOutcome::Failed && selfUpdate && !IsTestData() && PendingTransaction().empty()) RemoveSelfUpdateGuard(tx);
    if (r == InstallOutcome::Installed && selfUpdate && !IsTestData()) {
        g_selfUpdateStarted = StartSelfUpdateWatchdog(tx);
        if (!g_selfUpdateStarted) {
            std::wstring rollback;
            RevertTx(cfg, tx, "auto-rollback", rollback, nullptr);
            message = L"Self-update watchdog could not start. " + rollback;
            return InstallOutcome::Failed;
        }
    }
    if (r == InstallOutcome::Installed) PruneTransactions(5, tx);
    return r;
}

#include "lifecycle.inc"


InstallOutcome RollbackLast(const Config& cfg, std::wstring& message, const Progress& progress) {
    UpdateLock operation(message);
    if (!operation || !SecureUpdateData(message)) return InstallOutcome::Failed;
    if (!PendingTransaction().empty()) { message = Tr(L"Nejdřív je potřeba restartovat kvůli čekající aktualizaci.", L"Restart first: an update is waiting for it."); return InstallOutcome::Failed; }
    auto history = LoadJson(JoinPath(DataDir(), L"history.json"), json::array());
    for (size_t i = history.size(); i-- > 0;) {
        const json& e = history[i];
        if (e.value("kind", "") == "install" && e.value("result", "") == "installed" && !e.contains("rolledBackBy"))
            return RevertTx(cfg, e.value("tx", ""), "rollback", message, progress);
    }
    message = Tr(L"V historii není žádná aktualizace, kterou by šlo vrátit.", L"The history has no update that could be undone.");
    return InstallOutcome::NothingToDo;
}

bool EnsureFinalizeService(std::wstring& error) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring bin = L"\"" + std::wstring(exe) + L"\" /service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) { error = Tr(L"Správce služeb: ", L"Service manager: ") + LastErrorText(GetLastError()); return false; }
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"LiberNT Update", SERVICE_CHANGE_CONFIG, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_CHANGE_CONFIG);
        if (svc)
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, bin.c_str(), nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr);
    }
    bool ok = svc != nullptr;
    if (!ok) error = Tr(L"Služba LiberNTUpdate: ", L"Service LiberNTUpdate: ") + LastErrorText(GetLastError());
    if (svc) {
        SERVICE_DESCRIPTIONW d{const_cast<LPWSTR>(Tr(L"Hledá a instaluje aktualizace LiberNT, po restartu je ověří a při chybě vrátí.",
                                                     L"Finds and installs LiberNT updates, verifies them after the restart and undoes them on failure."))};
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

Status LoadStatus() {
    json j = LoadJson(JoinPath(DataDir(), L"status.json"), json::object());
    Status s;
    s.checked = j.value("checked", "");
    s.latest = j.value("latest", "");
    s.error = j.value("error", "");
    s.changes = j.value("changes", 0);
    return s;
}

#ifndef LNTU_VERSION
#define LNTU_VERSION "dev"
#endif
const char* ClientVersion() { return LNTU_VERSION; }

#include "self_update.inc"


void RunAutomatic(void* stopEvent) {
    std::wstring statusPath = JoinPath(DataDir(), L"status.json");
    LogLine(std::string("service: client ") + ClientVersion() + " running");
    for (;;) {
        Config cfg = LoadConfig();
        json st = LoadJson(statusPath, json::object());
        long long now = (long long)time(nullptr), last = st.value("checkedEpoch", 0LL);
        if (cfg.autoMode != "off" && PendingTransaction().empty() && now - last >= cfg.checkMinutes * 60LL) {
            CheckResult c;
            std::wstring err, msg;
            bool ok = CheckForUpdate(cfg, c, err, nullptr);
            st = json{{"checkedEpoch", now}, {"checked", NowIso()}, {"latest", c.latest.id}, {"changes", ok ? (int)c.changes.size() : 0},
                      {"error", Narrow(err)}};
            // A release that failed or was rolled back on this machine is not retried automatically; a person
            // decides (window or /install), otherwise a broken release would be installed and undone forever.
            bool failedHere = false;
            for (auto& e : LoadJson(JoinPath(DataDir(), L"history.json"), json::array()))
                if (e.value("kind", "") == "install" && e.value("release", "") == c.latest.id &&
                    (e.value("result", "") == "failed" || e.contains("rolledBackBy")))
                    failedHere = true;
            if (ok && !c.changes.empty() && cfg.autoMode == "install" && failedHere) {
                st["error"] = "release " + c.latest.id + " failed or was rolled back here; install it by hand to retry";
                LogLine("auto: " + st["error"].get<std::string>());
            }
            if (ok && !c.changes.empty() && cfg.autoMode == "install" && !failedHere) {
                InstallOutcome r = InstallUpdate(cfg, c, msg, nullptr);
                LogLine("auto install " + c.latest.id + ": " + Narrow(msg));
                if (r == InstallOutcome::Installed) st["changes"] = 0;
                if (r == InstallOutcome::Failed) st["error"] = Narrow(msg);
            }
            SaveJson(statusPath, st);
            if (g_selfUpdateStarted) {
                LogLine("self-update: service stops, the watchdog starts the new client");
                return;
            }
        }
        if (!PendingTransaction().empty() && cfg.restartHour >= 0) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            std::wstring err;
            if (t.wHour == cfg.restartHour) {
                LogLine("auto restart for pending update " + PendingTransaction());
                if (!RestartSystem(err)) LogLine("auto restart: " + Narrow(err));
            }
        }
        if (WaitForSingleObject((HANDLE)stopEvent, 60 * 1000) == WAIT_OBJECT_0) return;
    }
}

bool RegisterAutomatic(std::wstring& error) {
    if (!SecureUpdateData(error)) return false;
    if (!EnsureFinalizeService(error)) return false;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" /tray";
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        error = Tr(L"Nelze otevřít klíč Run v registru.", L"Cannot open the Run key in the registry.");
        return false;
    }
    bool ok = RegSetValueExW(key, L"LiberNTUpdate", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok) error = Tr(L"Nelze zapsat spuštění ikony do klíče Run.", L"Cannot add the tray icon to the Run key.");
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

#include "engine_tests.inc"
