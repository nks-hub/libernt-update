// LiberNT Update: checks a signed feed, updates changed system files, verifies them after a restart
// and rolls back on failure. Without arguments it opens the window; see Usage() for the command line.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "engine.h"
#include "util.h"

int RunGui(HINSTANCE inst);
int RunTray(HINSTANCE inst);

namespace {

SERVICE_STATUS_HANDLE g_status;
SERVICE_STATUS g_svc{SERVICE_WIN32_OWN_PROCESS};
HANDLE g_stop;

/// Writes to the console or pipe the program was started from, and to %ProgramData%\LiberNTUpdate\cli-output.txt
/// (a GUI-subsystem program started by cmd.exe gets no pipe, so the file is what scripts read).
void Out(const std::wstring& text) {
    static bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    (void)attached;
    std::string s = Narrow(text + L"\r\n");
    DWORD wrote;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s.data(), (DWORD)s.size(), &wrote, nullptr);
    std::wstring path = JoinPath(DataDir(), L"cli-output.txt");
    EnsureDirs(path, true);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, s.data(), (DWORD)s.size(), &wrote, nullptr);
        CloseHandle(f);
    }
}

void ReportService(DWORD state, DWORD exitCode = NO_ERROR) {
    g_svc.dwCurrentState = state;
    g_svc.dwWin32ExitCode = exitCode;
    g_svc.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_status, &g_svc);
}

DWORD WINAPI ServiceControl(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        ReportService(SERVICE_STOP_PENDING);
        SetEvent(g_stop);
    }
    return NO_ERROR;
}

/// At boot: finish or roll back a pending update; then check and install updates until the service stops.
void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_status = RegisterServiceCtrlHandlerExW(L"LiberNTUpdate", ServiceControl, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ReportService(SERVICE_RUNNING);
    std::wstring msg;
    if (FinalizePending(LoadConfig(), msg)) LogLine("service: " + Narrow(msg));
    ServiceHousekeeping();
    RunAutomatic(g_stop);
    ReportService(SERVICE_STOPPED);
}

std::wstring Usage() {
    return L"LiberNTUpdate.exe                 open the window\r\n"
           L"LiberNTUpdate.exe /check          compare the newest release with this system\r\n"
           L"LiberNTUpdate.exe /install        install it (restart may be required)\r\n"
           L"LiberNTUpdate.exe /rollback       undo the most recent update\r\n"
           L"LiberNTUpdate.exe /cancel         withdraw an update that waits for a restart\r\n"
           L"LiberNTUpdate.exe /finalize       finish a pending update (the service does this at boot)\r\n"
           L"LiberNTUpdate.exe /history        list past updates\r\n"
           L"LiberNTUpdate.exe /restart        restart the computer\r\n"
           L"LiberNTUpdate.exe /register       enable automatic updates (service and tray icon)\r\n"
           L"LiberNTUpdate.exe /status         result of the last automatic check\r\n"
           L"LiberNTUpdate.exe /tray           tray icon that announces updates (started at logon)\r\n"
           L"LiberNTUpdate.exe /selftest       built-in checks";
}

const wchar_t* OutcomeName(InstallOutcome r) {
    switch (r) {
        case InstallOutcome::Installed: return L"installed";
        case InstallOutcome::PendingRestart: return L"pending-restart";
        case InstallOutcome::NothingToDo: return L"nothing-to-do";
        default: return L"failed";
    }
}

int Cli(const std::wstring& cmd) {
    DeleteFileW(JoinPath(DataDir(), L"cli-output.txt").c_str());
    Config cfg = LoadConfig();
    Progress quiet;
    if (cmd == L"/selftest") {
        std::wstring report;
        bool ok = SelfTest(report);
        Out(report);
        return ok ? 0 : 1;
    }
    if (cmd == L"/check" || cmd == L"/install") {
        CheckResult c;
        std::wstring err;
        if (!CheckForUpdate(cfg, c, err, quiet)) { Out(L"ERROR " + err); return 2; }
        Out(L"installed: " + (c.installed.empty() ? std::wstring(L"(none recorded)") : Widen(c.installed)));
        Out(L"latest:    " + Widen(c.latest.id) + L" commit " + Widen(c.latest.commit));
        if (!c.pendingTx.empty()) Out(L"pending:   " + Widen(c.pendingTx) + L" (restart needed)");
        Out(L"changed:   " + std::to_wstring(c.changes.size()) + L" files");
        for (auto& ch : c.changes)
            Out((ch.oldSha.empty() ? L"  new     " : L"  changed ") + Widen(ch.path) + L"  " + Widen(ch.oldSha.substr(0, 12)) + L" -> " +
                Widen(ch.newSha.substr(0, 12)));
        if (cmd == L"/check") return 0;
        std::wstring msg, svcErr;
        if (!EnsureFinalizeService(svcErr)) Out(L"warning: " + svcErr);
        InstallOutcome r = InstallUpdate(cfg, c, msg, quiet);
        Out(std::wstring(OutcomeName(r)) + L": " + msg);
        return r == InstallOutcome::Failed ? 3 : 0;
    }
    if (cmd == L"/rollback") {
        std::wstring msg;
        InstallOutcome r = RollbackLast(cfg, msg, quiet);
        Out(std::wstring(OutcomeName(r)) + L": " + msg);
        return r == InstallOutcome::Failed ? 3 : 0;
    }
    if (cmd == L"/cancel") {
        std::wstring msg;
        bool ok = CancelPending(msg);
        Out(msg);
        return ok ? 0 : 1;
    }
    if (cmd == L"/finalize") {
        std::wstring msg;
        Out(FinalizePending(cfg, msg) ? msg : L"nothing pending");
        return 0;
    }
    if (cmd == L"/history") {
        for (auto& h : LoadHistory())
            Out(Widen(h.started + "  " + h.kind + "  " + h.release + "  " + h.result + "  " + std::to_string(h.files) + " files  " + h.message));
        return 0;
    }
    if (cmd == L"/register") {
        std::wstring err;
        if (!RegisterAutomatic(err)) { Out(L"ERROR " + err); return 5; }
        Out(L"automatic updates: service LiberNTUpdate and the tray icon are registered (auto=" + Widen(cfg.autoMode) +
            L", every " + std::to_wstring(cfg.checkMinutes) + L" min, restart hour " + std::to_wstring(cfg.restartHour) + L")");
        return 0;
    }
    if (cmd == L"/version") {
        Out(Widen(ClientVersion()));
        return 0;
    }
    if (cmd == L"/restart-service") {
        std::wstring err;
        return RestartService(err) ? 0 : 6;
    }
    if (cmd == L"/watch-self-update") {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::string tx = argc > 2 ? Narrow(argv[2]) : "";
        LocalFree(argv);
        return !tx.empty() && WatchSelfUpdate(tx) ? 0 : 7;
    }
    if (cmd == L"/status") {
        Status s = LoadStatus();
        Out(L"checked: " + Widen(s.checked) + L"\r\nlatest:  " + Widen(s.latest) + L"\r\nchanges: " + std::to_wstring(s.changes) +
            L"\r\npending: " + Widen(PendingTransaction()) + (s.error.empty() ? L"" : L"\r\nerror:   " + Widen(s.error)));
        return 0;
    }
    if (cmd == L"/restart") {
        std::wstring err;
        if (RestartSystem(err)) return 0;
        Out(L"ERROR " + err);
        return 4;
    }
    Out(Usage());
    return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    LocalFree(argv);
    if (cmd == L"/service") {
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(L"LiberNTUpdate"), ServiceMain}, {nullptr, nullptr}};
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }
    if (cmd == L"/tray") return RunTray(inst);
    if (!cmd.empty()) return Cli(cmd);
    return RunGui(inst);
}
