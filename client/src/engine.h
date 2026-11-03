#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/// %ProgramData%\LiberNTUpdate: config, state, history, transactions and the log.
std::wstring DataDir();

struct Config {
    std::wstring feedUrl;  ///< ends with '/'
    std::wstring root;     ///< system drive the manifest paths are relative to, e.g. C:\ .
    std::string autoMode = "install";  ///< service: "install" (check and install), "notify" (check only), "off"
    int checkMinutes = 360;            ///< how often the service checks the feed
    int restartHour = 3;               ///< local hour for the automatic restart of a pending update; -1 = never
};
Config LoadConfig();

/// Service loop: checks the feed every checkMinutes, installs when autoMode is "install", restarts a pending update
/// at restartHour, and writes %ProgramData%\LiberNTUpdate\status.json for the tray icon. Returns when stop is set.
void RunAutomatic(void* stopEvent);

/// What the last automatic check found (read by the tray icon).
struct Status {
    std::string checked, latest, error;
    int changes = 0;
};
Status LoadStatus();

/// Registers the boot-time service and the tray icon (HKLM Run) for automatic updates.
bool RegisterAutomatic(std::wstring& error);

/// Client version, set at build time (LNTU_VERSION), "dev" for local builds.
const char* ClientVersion();

/// Service start: removes the client files an earlier self-update moved aside and installs the current luupboot.exe.
void ServiceHousekeeping();

/// Stops the LiberNTUpdate service if it runs, starts it and requires it to stay running for 30 s.
bool RestartService(std::wstring& error);

/// Run from the previous client's exe after a self-update: restarts the service with the new client and rolls the
/// transaction back when the new client does not keep running.
bool WatchSelfUpdate(const std::string& tx);

struct Change {
    std::string path;    ///< manifest path, forward slashes, relative to the root
    std::string newSha;
    uint64_t size = 0;
    std::string oldSha;  ///< empty when the file does not exist yet
};

struct Release {
    std::string id, commit, date;
    std::vector<std::string> notes;
};

struct CheckResult {
    Release latest;
    std::string installed;  ///< id of the release installed through this tool, empty if none
    std::string pendingTx;  ///< transaction waiting for a restart, if any
    std::vector<Change> changes;
};

struct HistoryEntry {
    std::string tx, kind, release, started, finished, result, message;
    int files = 0;
};

/// Progress callback: percent 0-100 (or -1 when unknown) and a line of text.
using Progress = std::function<void(int, const std::wstring&)>;

/// Verifies the feed signature, the manifest hash, and compares the newest release with the disk.
bool CheckForUpdate(const Config& cfg, CheckResult& out, std::wstring& error, const Progress& progress);

enum class InstallOutcome { Installed, PendingRestart, Failed, NothingToDo };
/// Downloads and verifies the changed files, backs up the old ones and replaces them: right away when
/// none is in use, otherwise all together at the next restart. Any failure restores the backup.
InstallOutcome InstallUpdate(const Config& cfg, const CheckResult& check, std::wstring& message, const Progress& progress);

/// After a restart: confirms that a pending transaction landed and rolls it back if it did not.
/// Returns false when there was nothing to finalize.
bool FinalizePending(const Config& cfg, std::wstring& message);

/// Withdraws an update that waits for a restart: removes its scheduled renames, keeps the system as it is.
bool CancelPending(std::wstring& message);

/// Restores the files of the most recent installed transaction (directly or at the next restart).
InstallOutcome RollbackLast(const Config& cfg, std::wstring& message, const Progress& progress);

std::vector<HistoryEntry> LoadHistory();
std::string InstalledRelease();
std::string PendingTransaction();

/// Registers LiberNTUpdate.exe /service as an auto-start LocalSystem service that finalizes updates at boot.
bool EnsureFinalizeService(std::wstring& error);
/// Requests a system restart (needs SeShutdownPrivilege).
bool RestartSystem(std::wstring& error);

/// Built-in checks of the cryptography and of the transaction logic on a scratch directory.
bool SelfTest(std::wstring& report);
