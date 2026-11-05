// VM-only regression probe. It calls the real engine against an isolated journal; /kernel requires a VM snapshot.
#include <winsock2.h>
#include "../src/engine.cpp"
#include <iostream>

static std::wstring ProbeBase() { return L"C:\\lntu-review"; }

static bool PlainTree(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
    WIN32_FIND_DATAW item;
    HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &item);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool safe = true;
    do {
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L"..")) continue;
        if (!PlainTree(JoinPath(path, item.cFileName))) { safe = false; break; }
    } while (FindNextFileW(find, &item));
    FindClose(find);
    return safe;
}

static void PutObject(const Config& cfg, const std::string& text) {
    std::string sha = Sha256Hex(text.data(), text.size());
    std::wstring feed = ProbeBase() + L"\\feed\\objects\\" + Widen(sha.substr(0, 2)) + L"\\" + Widen(sha);
    if (!WriteTextAtomic(feed, text)) throw std::runtime_error("cannot write probe object");
}

#include "registry_boot.inc"
#include "tray_vm.inc"
#include "feed_http_vm.inc"
#include "boot_volume_vm.inc"
#include "uefi_boot_vm.inc"
#include "offline_vm.inc"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    std::wstring mode = argv[1];
    g_dataOverride = ProbeBase() + L"\\data";
    Config cfg{L"file:///C:/lntu-review/feed/", ProbeBase() + L"\\root"};
    std::wstring error;
    if (mode == L"selftest-engine") {
        std::wstring report;
        bool ok = SelfTest(report);
        std::cout << Narrow(report);
        return ok ? 0 : 1;
    }
    if (mode == L"feed-http") return RunFeedHttpTest();
    if (mode == L"boot-volume") return BootVolumeProbe();
    if (mode == L"boot-volume-clone") return BootVolumeProbe(true);
    if (mode == L"boot-volume-test") return BootVolumeRegression();
    if (mode.rfind(L"uefi-", 0) == 0) return UefiBootProbe(mode);
    if (mode.rfind(L"offline-", 0) == 0) return OfflineVmProbe(mode);
    if (mode == L"tray-test") return RunTrayTest();
    if (mode == L"tray-launch" && argc == 3) return LaunchTrayTest(wcstoul(argv[2], nullptr, 10));
    if (mode == L"registry-snapshot" && argc == 3) {
        LARGE_INTEGER frequency, started, parsed, compared;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        std::vector<uint8_t> raw;
        RegistrySnapshot snapshot;
        if (!ReadFileBytes(argv[2], raw, 16 * 1024 * 1024) || !ParseRegistrySnapshot(raw, snapshot, error)) {
            std::cout << "FAIL " << Narrow(error) << std::endl;
            return 1;
        }
        QueryPerformanceCounter(&parsed);
        std::vector<RegistryChange> changes;
        size_t preserved = 0, values = 0;
        bool ok = PlanRegistryChanges(snapshot, snapshot, changes, preserved, error) && changes.empty();
        QueryPerformanceCounter(&compared);
        for (const auto& key : snapshot.values) values += key.second.size();
        std::cout << (ok ? "PASS" : "FAIL") << " bytes=" << raw.size() << " keys=" << snapshot.keys.size() << " values=" << values
                  << " read_parse_ms=" << 1000.0 * (parsed.QuadPart - started.QuadPart) / frequency.QuadPart
                  << " same_baseline_compare_ms=" << 1000.0 * (compared.QuadPart - parsed.QuadPart) / frequency.QuadPart << std::endl;
        return ok ? 0 : 1;
    }
    if (mode == L"registry-plan" && argc == 4) {
        // Read-only: plans a baseline change against the live registry without applying it.
        std::vector<uint8_t> beforeRaw, afterRaw;
        RegistrySnapshot before, after;
        std::vector<RegistryChange> changes;
        size_t preserved = 0;
        bool ok = ReadFileBytes(argv[2], beforeRaw, 16 * 1024 * 1024) && ReadFileBytes(argv[3], afterRaw, 16 * 1024 * 1024) &&
                  ParseRegistrySnapshot(beforeRaw, before, error) && ParseRegistrySnapshot(afterRaw, after, error) &&
                  PlanRegistryChanges(before, after, changes, preserved, error);
        std::cout << (ok ? "PASS" : "FAIL") << " before=" << Sha256Hex(beforeRaw.data(), beforeRaw.size())
                  << " after=" << Sha256Hex(afterRaw.data(), afterRaw.size()) << " changes=" << changes.size()
                  << " preserved=" << preserved << " " << Narrow(error) << "\n";
        for (const auto& change : changes)
            std::cout << (change.after.present ? "SET " : "DEL ") << Narrow(RegistryDisplayName(change.after)) << "\n";
        return ok ? 0 : 1;
    }
    if (mode == L"registry-check" && argc == 5) {
        // Full signed check against a local feed copy with a new data folder; the system's updater data stays untouched.
        std::wstring feedDir = argv[2];
        g_dataOverride = feedDir + L"-data";
        if (!CreateDirectoryW(g_dataOverride.c_str(), nullptr)) { std::cout << "FAIL data folder must not exist\n"; return 1; }
        Config check{L"file:///" + feedDir + L"\\", L"C:\\"};
        check.baseRelease = Narrow(argv[3]);
        check.registryBaseRelease = Narrow(argv[4]);
        CheckResult result;
        bool ok = CheckForUpdate(check, result, error, nullptr);
        std::cout << (ok ? "PASS" : "FAIL") << " latest=" << result.latest.id << " files=" << result.changes.size()
                  << " registry=" << result.registryChanges.size() << " preserved=" << result.registryPreserved
                  << " registryBefore=" << result.registryBefore << " registryAfter=" << result.registryAfter
                  << " smssSupportsRegistry=" << RegistryUpdatesSupported() << " " << Narrow(error) << "\n";
        for (const auto& change : result.registryChanges)
            std::cout << (change.after.present ? "SET " : "DEL ") << Narrow(RegistryDisplayName(change.after)) << "\n";
        return ok ? 0 : 1;
    }
    if (mode.rfind(L"registry-", 0) == 0 || mode.rfind(L"smss-", 0) == 0) return RegistryBootProbe(mode, cfg);
    if (mode == L"selftest-client") {
        SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, TRUE};
        HANDLE output = CreateFileW(L"C:\\lntu-selftest.txt", GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS, 0, nullptr);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
        if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) return 1;
        STARTUPINFOW startup{sizeof startup};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = startup.hStdError = output;
        PROCESS_INFORMATION process{};
        wchar_t command[] = L"C:\\lntu-test.exe /selftest";
        bool started = CreateProcessW(nullptr, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) != 0;
        CloseHandle(input);
        CloseHandle(output);
        if (!started) return 1;
        DWORD result = 1;
        if (WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return (int)result;
    }
    if (mode == L"cleanup") {
        if (!PendingTransaction().empty()) return 1;
        wchar_t label[64]{};
        if (!GetVolumeInformationW(L"F:\\", label, 64, nullptr, nullptr, nullptr, nullptr, 0) || wcscmp(label, L"LNTUTEST")) return 1;
        for (std::wstring path : {ProbeBase(), std::wstring(L"F:\\lntu-boot-review")}) {
            if (!PlainTree(path)) return 1;
            std::wstring from = path + L'\0';
            SHFILEOPSTRUCTW operation{};
            operation.wFunc = FO_DELETE;
            operation.pFrom = from.c_str();
            operation.fFlags = FOF_NO_UI;
            if (SHFileOperationW(&operation) || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
        }
        std::cout << "PASS cleanup" << std::endl;
        return 0;
    }
    if (mode == L"hash" && argc == 3) {
        std::string hash = Sha256File(argv[2]);
        std::cout << hash << std::endl;
        return hash.empty() ? 1 : 0;
    }
    if (mode == L"permissions") {
        g_dataOverride.clear();
        bool ok = SecureUpdateData(error);
        std::cout << (ok ? "PASS permissions" : Narrow(error)) << std::endl;
        return ok ? 0 : 1;
    }
    if (mode == L"service-ready" || mode == L"service-hung") {
        bool healthy = RestartService(error, mode == L"service-ready" ? L"LntuReviewSlow" : L"LntuReviewHung");
        bool ok = mode == L"service-ready" ? healthy : !healthy;
        std::cout << Narrow(error) << "\n" << (ok ? "PASS" : "FAIL") << std::endl;
        return ok ? 0 : 1;
    }
    std::wstring target = TargetPath(cfg, "Windows/system32/probe.dll");
    if (mode == L"prepare" || mode == L"interrupt") {
        SaveState(json{{"installed", "ProbeBefore"}, {"pending", ""}});
        WriteTextAtomic(target, "before");
        PutObject(cfg, "after");
        CheckResult update;
        update.installed = "ProbeBefore";
        update.latest.id = "ProbeAfter";
        update.changes = {{"Windows/system32/probe.dll", Sha256Hex("after", 5), 5, Sha256Hex("before", 6)}};
        if (argc >= 3) {
            cfg.bootRoot = argv[2];
            std::wstring efi = TargetPath(cfg, "BootPartition/efi-probe.bin");
            WriteTextAtomic(efi, "efi-before");
            PutObject(cfg, "efi-after");
            update.changes.push_back({"BootPartition/efi-probe.bin", Sha256Hex("efi-after", 9), 9, Sha256Hex("efi-before", 10)});
        }
        HANDLE locked = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        InstallOutcome outcome = InstallUpdate(cfg, update, error, nullptr);
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        std::cout << Narrow(error) << std::endl;
        if (outcome != InstallOutcome::PendingRestart) return 1;
        if (mode == L"interrupt") {
            std::string tx = PendingTransaction();
            json journal = LoadJson(JoinPath(TxDir(tx), L"tx.json"), json::object());
            auto operations = OpsFromJson(journal["ops"]);
            for (auto& operation : operations)
                if (!CopyFileW(operation.staged.c_str(), Target(cfg, operation).c_str(), FALSE)) return 1;
            if (!WriteTextAtomic(BootFile(L"applying"), "applying\n") ||
                !MoveFileExW(BootFile(L"plan.txt").c_str(), BootFile(L"plan.done").c_str(), MOVEFILE_WRITE_THROUGH)) return 1;
        }
        return 0;
    }
    bool finalized = PendingTransaction().empty() || FinalizePending(cfg, error);
    std::cout << Narrow(error) << std::endl;
    std::cout << "installed=" << InstalledRelease() << " pending=" << PendingTransaction() << " boot=" << CurrentBootId() << std::endl;
    bool trial = FileExists(BootFile(L"trial"));
    // luupboot restarts right after applying a plan, so the first boot the probe sees confirms the update.
    bool ok = finalized && (mode == L"first" || mode == L"second" ? !trial && PendingTransaction().empty() && InstalledRelease() == "ProbeAfter"
                                : mode == L"recovered" ? !trial && PendingTransaction().empty() && InstalledRelease() == "ProbeBefore"
                                : false);
    std::cout << (ok ? "PASS" : "FAIL") << std::endl;
    return ok ? 0 : 1;
}
