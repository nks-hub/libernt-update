std::wstring g_watchdogExe;  // snapshot of the running client, see SnapshotRunningClient

std::wstring WatchdogExe() { return g_watchdogExe; }

std::string SelfUpdateGuardTransaction() {
    if (IsTestData()) return {};
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"LiberNTUpdateGuard", SERVICE_QUERY_CONFIG) : nullptr;
    DWORD code = svc ? ERROR_SUCCESS : GetLastError();
    if (!svc) {
        if (scm) CloseServiceHandle(scm);
        return code == ERROR_SERVICE_DOES_NOT_EXIST || code == ERROR_SERVICE_MARKED_FOR_DELETE ? "" : "?";
    }
    DWORD size = 0;
    QueryServiceConfigW(svc, nullptr, 0, &size);
    std::vector<uint8_t> buffer(size);
    std::string tx = "?";
    if (size && QueryServiceConfigW(svc, (QUERY_SERVICE_CONFIGW*)buffer.data(), size, &size)) {
        std::wstring command = ((QUERY_SERVICE_CONFIGW*)buffer.data())->lpBinaryPathName;
        std::wstring separator = L" /guard-service ";
        size_t start = command.rfind(separator);
        if (start != std::wstring::npos) tx = Narrow(command.substr(start + separator.size()));
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return tx;
}

void SnapshotRunningClient() {
    std::wstring error;
    UpdateLock operation(error);
    if (!operation) { LogLine(Narrow(error)); return; }
    // Called first thing at service start, while the exe on disk is still the image this process runs. The name
    // carries the hash, so a watchdog still running from an older snapshot never blocks the copy.
    std::wstring dir = JoinPath(DataDir(), L"watchdog");
    std::string sha = Sha256File(OwnExe());
    std::wstring exe = JoinPath(dir, Widen(sha.substr(0, 16)) + L".exe");
    EnsureDirs(exe, true);
    if (Sha256File(exe) != sha && !CopyFileW(OwnExe().c_str(), exe.c_str(), FALSE)) {
        LogLine("service: cannot copy the running client to " + Narrow(exe) + ": " + Narrow(LastErrorText(GetLastError())));
        return;
    }
    g_watchdogExe = exe;
    if (!SelfUpdateGuardTransaction().empty()) return;
    WIN32_FIND_DATAW item;
    HANDLE find = FindFirstFileW(JoinPath(dir, L"*.exe").c_str(), &item);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring path = JoinPath(dir, item.cFileName);
        if (_wcsicmp(path.c_str(), exe.c_str()) != 0) DeleteFileW(path.c_str());
    } while (FindNextFileW(find, &item));
    FindClose(find);
}

void RemoveSelfUpdateGuard(const std::string& tx) {
    if (IsTestData()) return;
    std::wstring error;
    UpdateLock operation(error);
    if (!operation || SelfUpdateGuardTransaction() != tx) return;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"LiberNTUpdateGuard", DELETE) : nullptr;
    if (svc) { DeleteService(svc); CloseServiceHandle(svc); }
    if (scm) CloseServiceHandle(scm);
}


bool RegisterSelfUpdateGuard(const std::string& tx, std::wstring& error) {
    std::wstring exe = WatchdogExe();
    if (exe.empty() || !FileExists(exe)) { error = L"The previous client snapshot is missing."; return false; }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    std::wstring command = L"\"" + exe + L"\" /guard-service " + Widen(tx);
    SC_HANDLE svc = scm ? CreateServiceW(scm, L"LiberNTUpdateGuard", L"LiberNT Update recovery", SERVICE_CHANGE_CONFIG,
                                         SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                         command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr) : nullptr;
    if (!svc && scm && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, L"LiberNTUpdateGuard", SERVICE_CHANGE_CONFIG);
        if (svc && !ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, command.c_str(),
                                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            CloseServiceHandle(svc);
            svc = nullptr;
        }
    }
    bool ok = svc != nullptr;
    if (!ok) error = L"Cannot register the previous client's recovery service: " + LastErrorText(GetLastError());
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    return ok;
}

void ServiceHousekeeping() {
    std::wstring lockError;
    UpdateLock operation(lockError);
    if (!operation) { LogLine(Narrow(lockError)); return; }
    std::wstring exe = OwnExe(), dir = exe.substr(0, exe.find_last_of(L'\\'));
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(JoinPath(dir, L"*.old-*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring p = JoinPath(dir, fd.cFileName);
            LogLine("housekeeping: " + Narrow(p) + (DeleteFileW(p.c_str()) ? " removed" : " still in use"));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::wstring err;
    if (!EnsureBootApplier(err)) LogLine("housekeeping: luupboot: " + Narrow(err));
}

/// Stops the service if it runs, starts it again and requires it to stay running for 30 s.
bool RestartService(std::wstring& error, const wchar_t* serviceName) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP) : nullptr;
    bool ok = false;
    if (!svc) {
        error = Tr(L"Službu LiberNTUpdate nelze otevřít: ", L"Cannot open service LiberNTUpdate: ") + LastErrorText(GetLastError());
    } else {
        SERVICE_STATUS s{};
        bool stopRequested = false;
        for (int i = 0; i < 120 && QueryServiceStatus(svc, &s) && s.dwCurrentState != SERVICE_STOPPED; i++) {
            if (s.dwCurrentState == SERVICE_RUNNING && !stopRequested)
                stopRequested = ControlService(svc, SERVICE_CONTROL_STOP, &s) != FALSE;
            Sleep(500);
        }
        bool started = StartServiceW(svc, 0, nullptr) != FALSE;
        DWORD startError = started ? ERROR_SUCCESS : GetLastError();
        if (!started && startError != ERROR_SERVICE_ALREADY_RUNNING) {
            error = Tr(L"Službu LiberNTUpdate nelze spustit: ", L"Cannot start service LiberNTUpdate: ") + LastErrorText(GetLastError());
        } else {
            for (int i = 0; i < 60 && QueryServiceStatus(svc, &s) && s.dwCurrentState == SERVICE_START_PENDING; i++) Sleep(500);
            ok = QueryServiceStatus(svc, &s) && s.dwCurrentState == SERVICE_RUNNING;
            for (int i = 0; ok && i < 30; i++) {
                Sleep(1000);
                ok = QueryServiceStatus(svc, &s) && s.dwCurrentState == SERVICE_RUNNING;
            }
            if (!ok) error = Tr(L"Služba LiberNTUpdate po spuštění skončila (stav ", L"Service LiberNTUpdate stopped after the start (state ") +
                             std::to_wstring(s.dwCurrentState) + Tr(L", kód ", L", code ") +
                             std::to_wstring(s.dwWin32ExitCode) + L").";
        }
    }
    LogLine(ok ? "service restarted and running for 30 s" : "service restart failed: " + Narrow(error));
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    return ok;
}

/// Runs from the previous client's exe (moved aside by a self-update): restarts the service with the new client and
/// rolls the update back when the new client does not keep the service running.
bool WatchSelfUpdate(const std::string& tx) {
    LogLine(std::string("self-update watchdog (client ") + ClientVersion() + ") for tx " + tx);
    std::wstring err;
    if (RestartService(err)) {
        if (PendingTransaction() != tx) RemoveSelfUpdateGuard(tx);
        return true;
    }
    std::wstring msg;
    InstallOutcome r;
    {
        UpdateLock operation(msg);
        if (!operation) return false;
        if (PendingTransaction() == tx) {
            json journal = LoadJson(JoinPath(TxDir(tx), L"tx.json"), json::object());
            if (journal.value("mode", "") == "direct") {
                FinalizePending(LoadConfig(), msg);
                r = PendingTransaction().empty() ? InstallOutcome::Installed : InstallOutcome::Failed;
            } else {
                if (!FileExists(BootFile(L"rollback.txt")) || !WriteTextAtomic(BootFile(L"trial"), "2")) return false;
                LogLine("new client failed after boot; rollback requested at the next boot");
                RestartSystem(err);
                return false;
            }
        } else {
            r = RevertTx(LoadConfig(), tx, "auto-rollback", msg, nullptr);
        }
    }
    LogLine("self-update failed, rollback of " + tx + ": " + Narrow(msg));
    bool healthy = RestartService(err);
    if (healthy && r == InstallOutcome::Installed && PendingTransaction().empty()) RemoveSelfUpdateGuard(tx);
    return healthy && r == InstallOutcome::Installed;
}

/// After a self-update in the service: the copy of the code that is running now (made at service start, see
/// SnapshotRunningClient) restarts the service with the new client and undoes the update if that fails.
bool StartSelfUpdateWatchdog(const std::string& tx) {
    std::wstring exe = WatchdogExe();
    std::wstring cmd = L"\"" + exe + L"\" /watch-self-update " + Widen(tx);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (exe.empty() || !FileExists(exe) ||
        !CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        LogLine("self-update: cannot start the watchdog " + Narrow(exe) + ": " + Narrow(LastErrorText(GetLastError())));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
