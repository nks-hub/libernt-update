static const wchar_t kOfflineVictim[] = L"LUUP OFFLINE RECOVERY TEST VICTIM";

static bool IsOfflineVictim(const VolumeIdentity& volume) {
    auto device = L"\\\\?\\GLOBALROOT\\GLOBAL??\\PhysicalDrive" + std::to_wstring(volume.disk);
    HANDLE file = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    alignas(DRIVE_LAYOUT_INFORMATION_EX) BYTE buffer[4096]{};
    DWORD bytes = 0;
    bool ok = file != INVALID_HANDLE_VALUE && DeviceIoControl(file, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        nullptr, 0, buffer, sizeof buffer, &bytes, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    auto layout = (const DRIVE_LAYOUT_INFORMATION_EX*)buffer;
    static const GUID gptCopy = {0x4c555550, 0x0000, 0x4000, {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
    return ok && bytes >= FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) &&
           ((layout->PartitionStyle == PARTITION_STYLE_MBR && layout->Mbr.Signature == 0x4c555550) ||
            (layout->PartitionStyle == PARTITION_STYLE_GPT && IsEqualGUID(layout->Gpt.DiskId, gptCopy)));
}

static bool WriteMappedValue(RegistryItem item, OfflineHives& hives, std::wstring& error) {
    if (!hives.Map(item, error)) return false;
    auto keyPath = item.key.substr(wcslen(L"\\Registry\\Machine\\"));
    HKEY key = nullptr;
    LONG status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) status = RegSetValueExW(key, item.name.c_str(), 0, item.type, item.data.data(), (DWORD)item.data.size());
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    if (key) RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static int OfflineVmProbe(const std::wstring& mode) {
    std::wstring error;
    VolumeIdentity current, target;
    if (!ReadVolumeIdentity(L"C:\\", current)) return 1;
    std::cout << "running_disk=" << current.disk << " partition=" << current.partition << "\n";
    if (mode == L"offline-victim-setup") {
        if (current.disk != 0 || IsOfflineVictim(current) || !ReadVolumeIdentity(L"E:\\", target) ||
            target.disk != 1 || target.partition != 2 || !IsOfflineVictim(target)) return 1;
        RecoveryPrivileges privileges(error);
        OfflineHives hives(target.root);
        if (!privileges) return 1;
        RegistryItem drive{L"\\Registry\\Machine\\SYSTEM\\MountedDevices", L"\\DosDevices\\C:", false, true, REG_BINARY,
                           {0x50, 0x55, 0x55, 0x4c, 0x00, 0x00, 0x10, 0x04, 0x00, 0x00, 0x00, 0x00}};
        if (!WriteMappedValue(drive, hives, error) || !hives.Unload(error)) { std::cout << Narrow(error); return 1; }
        if (!WriteTextAtomic(L"E:\\lntu-offline-victim.txt", Narrow(kOfflineVictim))) return 1;
        g_dataOverride.clear();
        g_migrationData = L"E:\\ProgramData\\LiberNTUpdate";
        if (GetFileAttributesW(DataDir().c_str()) == INVALID_FILE_ATTRIBUTES && !CreateUpdateDataDirectory(DataDir(), error)) return 1;
        std::cout << "PASS victim drive mapping and protected updater directory prepared\n";
        return 0;
    }
    if (mode == L"offline-gpt-setup") {
        // Maps C: to the GPT copy of the victim disk; refused for any other disk GUID.
        static const GUID diskId = {0x4c555550, 0x0000, 0x4000, {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
        std::wstring root = L"F:\\";
        if (!ReadVolumeIdentity(root, target) || target.disk == current.disk) return 1;
        std::wstring raw = target.root;
        raw.pop_back();
        HANDLE volume = CreateFileW(raw.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        PARTITION_INFORMATION_EX part{};
        DWORD bytes = 0;
        bool ok = volume != INVALID_HANDLE_VALUE &&
                  DeviceIoControl(volume, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &part, sizeof part, &bytes, nullptr);
        if (volume != INVALID_HANDLE_VALUE) CloseHandle(volume);
        auto device = L"\\\\?\\GLOBALROOT\\GLOBAL??\\PhysicalDrive" + std::to_wstring(target.disk);
        HANDLE disk = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        alignas(DRIVE_LAYOUT_INFORMATION_EX) BYTE layoutBuffer[4096]{};
        ok = ok && disk != INVALID_HANDLE_VALUE && DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                                                                   layoutBuffer, sizeof layoutBuffer, &bytes, nullptr);
        if (disk != INVALID_HANDLE_VALUE) CloseHandle(disk);
        auto layout = (const DRIVE_LAYOUT_INFORMATION_EX*)layoutBuffer;
        std::vector<uint8_t> marker;
        if (!ok || part.PartitionStyle != PARTITION_STYLE_GPT || layout->PartitionStyle != PARTITION_STYLE_GPT ||
            !IsEqualGUID(layout->Gpt.DiskId, diskId) || !ReadFileBytes(root + L"lntu-offline-victim.txt", marker) ||
            std::string(marker.begin(), marker.end()) != Narrow(kOfflineVictim)) {
            std::cout << "FAIL not the GPT test copy\n";
            return 1;
        }
        RecoveryPrivileges privileges(error);
        OfflineHives hives(target.root);
        if (!privileges) return 1;
        RegistryItem drive{L"\\Registry\\Machine\\SYSTEM\\MountedDevices", L"\\DosDevices\\C:", false, true, REG_BINARY,
                           {'D', 'M', 'I', 'O', ':', 'I', 'D', ':'}};
        drive.data.insert(drive.data.end(), (const uint8_t*)&part.Gpt.PartitionId, (const uint8_t*)&part.Gpt.PartitionId + sizeof(GUID));
        if (!WriteMappedValue(drive, hives, error) || !hives.Unload(error)) { std::cout << Narrow(error); return 1; }
        std::cout << "PASS C: of the GPT copy mapped to its partition GUID\n";
        return 0;
    }
    if (mode == L"offline-log-test" || mode == L"offline-orphan-mount" || mode == L"offline-orphan-retry" || mode == L"offline-orphan-cleared") {
        if (IsOfflineVictim(current) || !ReadVolumeIdentity(L"E:\\", target) || !IsOfflineVictim(target)) return 1;
        RecoveryPrivileges privileges(error);
        OfflineHives hives(target.root);
        if (!privileges) return 1;
        if (mode == L"offline-log-test") {
            std::wstring log = L"E:\\Windows\\System32\\config\\SOFTWARE.LOG1";
            std::wstring saved = log + L".lntu-log-test", outside = L"C:\\lntu-offline-hive-log-target.txt";
            if (FileExists(saved) || FileExists(outside)) return 1;
            bool hadLog = FileExists(log);
            if (hadLog && !MoveFileExW(log.c_str(), saved.c_str(), MOVEFILE_WRITE_THROUGH)) return 1;
            bool linked = WriteTextAtomic(outside, "outside") && CreateSymbolicLinkW(log.c_str(), outside.c_str(), 0);
            auto mount = linked ? hives.Mount(L"SOFTWARE", error) : std::wstring();
            bool ok = linked && mount.empty() && error.find(L"hive log") != std::wstring::npos && Sha256File(outside) == Sha256Hex("outside", 7);
            if (!mount.empty()) hives.Unload(error);
            if (linked) ok = DeleteFileW(log.c_str()) && ok;
            if (hadLog) ok = MoveFileExW(saved.c_str(), log.c_str(), MOVEFILE_WRITE_THROUGH) && ok;
            if (Sha256File(outside) == Sha256Hex("outside", 7)) DeleteFileW(outside.c_str());
            std::cout << (ok ? "PASS " : "FAIL ") << "offline hive log symlink preserves the other volume: " << Narrow(error) << "\n";
            return ok ? 0 : 1;
        }
        auto mount = hives.Mount(L"SYSTEM", error);
        if (mode == L"offline-orphan-mount" && !mount.empty()) {
            std::cout << "MOUNT " << Narrow(mount) << "; intentionally exiting before destructors" << std::endl;
            ExitProcess(77);
        }
        bool ok = mode == L"offline-orphan-retry" ? mount.empty() && error.find(L"restart the recovery OS") != std::wstring::npos
                                                  : !mount.empty() && hives.Unload(error);
        std::cout << (ok ? "PASS " : "FAIL ") << Narrow(mode) << " " << Narrow(error) << "\n";
        return ok ? 0 : 1;
    }
    std::vector<uint8_t> marker;
    if (!IsOfflineVictim(current) || !ReadFileBytes(L"C:\\lntu-offline-victim.txt", marker) ||
        std::string(marker.begin(), marker.end()) != Narrow(kOfflineVictim)) { std::cout << "FAIL victim identity\n"; return 1; }
    g_dataOverride.clear();
    g_migrationData = L"C:\\ProgramData\\LiberNTUpdate";
    Config cfg{L"file:///C:/ProgramData/LiberNTUpdate/feed/", L"C:\\"};
    if (!SecureUpdateData(error)) { std::cout << Narrow(error); return 1; }
    if (mode == L"offline-loader-prepare") {
        if (!PendingTransaction().empty() || InstalledRelease() != "RecoveryBefore") { std::cout << "FAIL state\n"; return 1; }
        const std::string firstPath = "BootPartition/EFI/BOOT/bootx64.efi";
        const std::string secondPath = "BootPartition/EFI/LiberNT/Boot/bootmgfw.efi";
        auto first = TargetPath(cfg, firstPath), second = TargetPath(cfg, secondPath);
        wchar_t volumeRoot[MAX_PATH]{};
        if (first.empty() || second.empty() || !GetVolumePathNameW(first.c_str(), volumeRoot, MAX_PATH)) {
            std::cout << "FAIL loader paths first=" << Narrow(first) << " error=" << GetLastError() << "\n";
            return 1;
        }
        std::wstring bootRoot(volumeRoot);
        auto bcd = JoinPath(bootRoot, L"EFI\\LiberNT\\Boot\\BCD");
        std::string firstBefore = Sha256File(first), secondBefore = Sha256File(second), bcdBefore = Sha256File(bcd);
        // The running OS keeps BCD open as a hive; then its hash is checked offline instead.
        if (firstBefore.empty() || secondBefore.empty()) {
            std::cout << "FAIL hashes first=" << Narrow(first) << " second=" << Narrow(second) << " bcd=" << Narrow(bcd)
                      << " missing=" << firstBefore.empty() << secondBefore.empty() << bcdBefore.empty() << "\n";
            return 1;
        }
        std::vector<uint8_t> damaged(4096, 0xa5);
        const char label[] = "LIBERNT UPDATE INTENTIONAL UEFI FAILURE";
        memcpy(damaged.data(), label, sizeof label - 1);
        std::string damagedHash = Sha256Hex(damaged.data(), damaged.size());
        if (!WriteFileAtomic(JoinPath(DataDir(), L"feed\\objects\\" + Widen(damagedHash.substr(0, 2)) + L"\\" + Widen(damagedHash)),
                             damaged.data(), damaged.size())) return 1;
        CheckResult update;
        update.installed = "RecoveryBefore";
        update.latest.id = "RecoveryBadLoader";
        update.changes = {{firstPath, damagedHash, damaged.size(), firstBefore},
                          {secondPath, damagedHash, damaged.size(), secondBefore}};
        auto outcome = InstallUpdate(cfg, update, error, nullptr);
        std::string tx;
        auto history = LoadJson(JoinPath(DataDir(), L"history.json"), json::array());
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->value("release", "") == "RecoveryBadLoader" && it->value("result", "") == "installed") {
                tx = it->value("tx", "");
                break;
            }
        }
        auto saved = LoadJson(JoinPath(DataDir(), L"victim.json"), json::object());
        saved["loaderTx"] = tx;
        saved["loaderFirstBefore"] = firstBefore;
        saved["loaderSecondBefore"] = secondBefore;
        saved["loaderBcdBefore"] = bcdBefore;
        saved["loaderDamaged"] = damagedHash;
        bool ok = outcome == InstallOutcome::Installed && RecoveryTransactionId(tx) &&
                  Sha256File(first) == damagedHash && Sha256File(second) == damagedHash &&
                  InstalledRelease() == "RecoveryBadLoader" && PendingTransaction().empty() &&
                  SaveJson(JoinPath(DataDir(), L"victim.json"), saved);
        std::cout << (ok ? "PASS " : "FAIL ") << "UEFI loaders damaged by direct transaction; tx=" << tx
                  << " boot=" << Narrow(bootRoot) << " bcd=" << (bcdBefore.empty() ? "locked" : bcdBefore) << " " << Narrow(error) << "\n";
        return ok ? 0 : 1;
    }
    if (mode == L"offline-loader-recovered") {
        auto saved = LoadJson(JoinPath(DataDir(), L"victim.json"), json::object());
        auto first = TargetPath(cfg, "BootPartition/EFI/BOOT/bootx64.efi");
        auto second = TargetPath(cfg, "BootPartition/EFI/LiberNT/Boot/bootmgfw.efi");
        wchar_t volumeRoot[MAX_PATH]{};
        if (first.empty() || second.empty() || saved.empty() || !GetVolumePathNameW(first.c_str(), volumeRoot, MAX_PATH)) return 1;
        std::wstring bootRoot(volumeRoot);
        auto bcd = JoinPath(bootRoot, L"EFI\\LiberNT\\Boot\\BCD");
        bool ok = Sha256File(first) == saved.value("loaderFirstBefore", "") &&
                  Sha256File(second) == saved.value("loaderSecondBefore", "") &&
                  Sha256File(bcd) == saved.value("loaderBcdBefore", "") &&  // both empty while BCD is a loaded hive
                  InstalledRelease() == "RecoveryBefore" && PendingTransaction().empty();
        std::cout << (ok ? "PASS " : "FAIL ") << "UEFI loaders and BCD restored; installed="
                  << InstalledRelease() << " pending=" << PendingTransaction() << "\n";
        return ok ? 0 : 1;
    }
    // Installs the files listed in C:\lntu-files.json ({"release", "files": [{"path", "file"}]}) as one update.
    if (mode == L"offline-files-install" || mode == L"offline-files-rollback") {
        InstallOutcome outcome;
        if (mode == L"offline-files-rollback") {
            outcome = RollbackLast(cfg, error, nullptr);
        } else {
            auto plan = LoadJson(L"C:\\lntu-files.json", json::object());
            CheckResult update;
            update.installed = InstalledRelease();
            update.latest.id = plan.value("release", "");
            for (const auto& item : plan.value("files", json::array())) {
                std::vector<uint8_t> data;
                std::string path = item.value("path", "");
                if (!ReadFileBytes(Widen(item.value("file", "")), data)) return 1;
                std::string hash = Sha256Hex(data.data(), data.size());
                if (!WriteFileAtomic(JoinPath(DataDir(), L"feed\\objects\\" + Widen(hash.substr(0, 2)) + L"\\" + Widen(hash)), data.data(), data.size()))
                    return 1;
                update.changes.push_back({path, hash, data.size(), Sha256File(TargetPath(cfg, path))});
            }
            if (update.latest.id.empty() || update.changes.empty() || !PendingTransaction().empty()) return 1;
            outcome = InstallUpdate(cfg, update, error, nullptr);
        }
        std::cout << (outcome == InstallOutcome::Failed ? "FAIL " : "PASS ") << Narrow(error) << " installed=" << InstalledRelease()
                  << " pending=" << PendingTransaction() << "\n";
        return outcome == InstallOutcome::Failed ? 1 : 0;
    }
    // A protected, loaded but non-critical driver: the native helper replaces and restores it across real restarts.
    if (mode.rfind(L"offline-protected-", 0) == 0) {
        const std::wstring beep = L"C:\\Windows\\System32\\drivers\\beep.sys";
        auto sddl = [&]() {
            std::vector<BYTE> sd(4096);
            DWORD need = 0;
            LPWSTR text = nullptr;
            std::string result;
            const SECURITY_INFORMATION what = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
            if (GetFileSecurityW(beep.c_str(), what, sd.data(), (DWORD)sd.size(), &need) &&
                ConvertSecurityDescriptorToStringSecurityDescriptorW(sd.data(), SDDL_REVISION_1, what, &text, nullptr)) result = Narrow(text);
            if (text) LocalFree(text);
            return result;
        };
        auto metadata = JoinPath(DataDir(), L"protected.json");
        if (mode == L"offline-protected-prepare") {
            std::vector<uint8_t> data;
            if (!PendingTransaction().empty() || InstalledRelease() != "RecoveryBefore" || !ReadFileBytes(beep, data)) return 1;
            std::string before = Sha256Hex(data.data(), data.size()), acl = sddl();
            if (acl.find("O:S-1-5-80-956008885") != 0 || !ChangeLoaderFixture(data)) { std::cout << "FAIL fixture " << acl << "\n"; return 1; }
            std::string after = Sha256Hex(data.data(), data.size());
            if (!WriteFileAtomic(JoinPath(DataDir(), L"feed\\objects\\" + Widen(after.substr(0, 2)) + L"\\" + Widen(after)), data.data(), data.size()) ||
                !SaveJson(metadata, json{{"before", before}, {"after", after}, {"acl", acl}})) return 1;
            CheckResult update;
            update.installed = "RecoveryBefore";
            update.latest.id = "RecoveryProtected";
            update.changes = {{"Windows/system32/drivers/beep.sys", after, data.size(), before}};
            auto outcome = InstallUpdate(cfg, update, error, nullptr);
            std::cout << (outcome == InstallOutcome::PendingRestart ? "PASS " : "FAIL ") << Narrow(error) << " acl=" << acl << "\n";
            return outcome == InstallOutcome::PendingRestart ? 0 : 1;
        }
        auto saved = LoadJson(metadata, json::object());
        if (saved.empty()) return 1;
        if (mode == L"offline-protected-rollback") {
            auto outcome = RollbackLast(cfg, error, nullptr);
            std::cout << (outcome == InstallOutcome::PendingRestart ? "PASS " : "FAIL ") << Narrow(error) << "\n";
            return outcome == InstallOutcome::PendingRestart ? 0 : 1;
        }
        bool rollback = mode.find(L"rollback") != std::wstring::npos, first = mode.find(L"first") != std::wstring::npos;
        bool finalized = PendingTransaction().empty() || FinalizePending(cfg, error);
        std::string hash = Sha256File(beep), acl = sddl();
        bool ok = finalized && hash == saved.value(rollback ? "before" : "after", "") && acl == saved.value("acl", "") &&
                  (first ? !PendingTransaction().empty()
                         : PendingTransaction().empty() && InstalledRelease() == (rollback ? "RecoveryBefore" : "RecoveryProtected"));
        std::cout << (ok ? "PASS " : "FAIL ") << Narrow(mode) << " beep.sys=" << hash << " aclKept=" << (acl == saved.value("acl", ""))
                  << " installed=" << InstalledRelease() << " pending=" << PendingTransaction() << " " << Narrow(error) << "\n";
        return ok ? 0 : 1;
    }
    // LiberNT does not start a newly registered Start=0 driver, so this replaces disk.sys, which the loader always loads.
    if (mode == L"offline-bootdriver-prepare") {
        const std::wstring disk = L"C:\\Windows\\System32\\drivers\\disk.sys";
        std::vector<uint8_t> bad;
        if (!PendingTransaction().empty() || InstalledRelease() != "RecoveryBefore" ||
            !ReadFileBytes(L"C:\\lntu_recovery_crash.sys", bad) || bad.size() < 512 || bad[0] != 'M' || bad[1] != 'Z') {
            std::cout << "FAIL state or crash driver\n";
            return 1;
        }
        std::string before = Sha256File(disk), hash = Sha256Hex(bad.data(), bad.size());
        if (before.empty() || before == hash ||
            !WriteFileAtomic(JoinPath(DataDir(), L"feed\\objects\\" + Widen(hash.substr(0, 2)) + L"\\" + Widen(hash)), bad.data(), bad.size()) ||
            !SaveJson(JoinPath(DataDir(), L"victim.json"), json{{"diskBefore", before}, {"diskAfter", hash}})) return 1;
        CheckResult update;
        update.installed = "RecoveryBefore";
        update.latest.id = "RecoveryBadBootDriver";
        update.changes = {{"Windows/system32/drivers/disk.sys", hash, bad.size(), before}};
        auto outcome = InstallUpdate(cfg, update, error, nullptr);
        std::cout << (outcome == InstallOutcome::PendingRestart ? "PASS " : "FAIL ") << Narrow(error)
                  << " tx=" << PendingTransaction() << " before=" << before << "\n";
        return outcome == InstallOutcome::PendingRestart ? 0 : 1;
    }
    if (mode == L"offline-bootdriver-first" || mode == L"offline-bootdriver-recovered") {
        bool first = mode == L"offline-bootdriver-first";
        bool finalized = first ? FinalizePending(cfg, error) : true;
        auto saved = LoadJson(JoinPath(DataDir(), L"victim.json"), json::object());
        std::string have = Sha256File(L"C:\\Windows\\System32\\drivers\\disk.sys");
        bool ok = finalized && !saved.empty() &&
                  (first ? have == saved.value("diskAfter", "") && !PendingTransaction().empty() && FileExists(BootFile(L"trial"))
                         : have == saved.value("diskBefore", "") && PendingTransaction().empty() && InstalledRelease() == "RecoveryBefore");
        std::cout << (ok ? "PASS " : "FAIL ") << Narrow(mode) << " disk.sys=" << have << " installed=" << InstalledRelease()
                  << " pending=" << PendingTransaction() << " " << Narrow(error) << "\n";
        return ok ? 0 : 1;
    }
    if (mode == L"offline-driver-prepare" || mode == L"offline-driver-retry") {
        if (!PendingTransaction().empty() || (!InstalledRelease().empty() && InstalledRelease() != "RecoveryBefore") ||
            FileExists(L"C:\\Windows\\System32\\drivers\\lntu_recovery_crash.sys")) return 1;
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LiberNTUpdateRecoveryProbe", 0, nullptr, 0,
                           KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return 1;
        DWORD value = 1, keep = 73;
        bool seeded = RegSetValueExW(key, L"Value", 0, REG_DWORD, (const BYTE*)&value, sizeof value) == ERROR_SUCCESS &&
                      RegSetValueExW(key, L"Keep", 0, REG_DWORD, (const BYTE*)&keep, sizeof keep) == ERROR_SUCCESS && RegFlushKey(key) == ERROR_SUCCESS;
        RegCloseKey(key);
        if (!seeded || !SaveState(json{{"installed", "RecoveryBefore"}, {"registryRelease", "RecoveryBefore"}, {"pending", ""}})) return 1;
        std::vector<uint8_t> bad;
        if (!ReadFileBytes(L"C:\\lntu_recovery_crash.sys", bad) || bad.size() < 512 || bad[0] != 'M' || bad[1] != 'Z') return 1;
        std::string hash = Sha256Hex(bad.data(), bad.size());
        if (!WriteFileAtomic(JoinPath(DataDir(), L"feed\\objects\\" + Widen(hash.substr(0, 2)) + L"\\" + Widen(hash)), bad.data(), bad.size())) return 1;
        CheckResult update;
        update.installed = update.registryBefore = "RecoveryBefore";
        update.latest.id = update.registryAfter = "RecoveryBadDriver";
        update.changes = {{"Windows/system32/drivers/lntu_recovery_crash.sys", hash, bad.size(), ""}};
        RegistryItem before{L"\\Registry\\Machine\\SOFTWARE\\LiberNTUpdateRecoveryProbe", L"Value", false, true, REG_DWORD, {1, 0, 0, 0}};
        auto after = before;
        after.data[0] = 2;
        update.registryChanges = {{before, after}};
        DWORD selected = 0, bytes = sizeof selected;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Select", L"Current", RRF_RT_REG_DWORD, nullptr, &selected, &bytes) != ERROR_SUCCESS ||
            !selected || selected > 999) return 1;
        wchar_t service[160];
        // The LiberNT kernel derives a boot driver's service name from its image file name.
        swprintf(service, 160, L"\\Registry\\Machine\\SYSTEM\\ControlSet%03lu\\Services\\lntu_recovery_crash", selected);
        RegistryItem missing{service, L"", true, false};
        auto present = missing;
        present.present = true;
        RegistryItem existing = missing;
        if (!ReadRegistryItem(existing, error) || existing.present) return 1;
        update.registryChanges.push_back({missing, present});
        auto dword = [&](const wchar_t* name, DWORD value) {
            RegistryItem absent{service, name, false, false}, desired{service, name, false, true, REG_DWORD, std::vector<uint8_t>(4)};
            memcpy(desired.data.data(), &value, sizeof value);
            update.registryChanges.push_back({absent, desired});
        };
        auto text = [&](const wchar_t* name, const std::wstring& value) {
            RegistryItem absent{service, name, false, false}, desired{service, name, false, true, REG_SZ, std::vector<uint8_t>((value.size() + 1) * sizeof(wchar_t))};
            memcpy(desired.data.data(), value.c_str(), desired.data.size());
            update.registryChanges.push_back({absent, desired});
        };
        dword(L"Type", 1);
        dword(L"Start", 0);
        dword(L"ErrorControl", 3);
        text(L"Group", L"Boot Bus Extender");
        text(L"ImagePath", L"system32\\drivers\\lntu_recovery_crash.sys");
        if (!SaveJson(JoinPath(DataDir(), L"victim.json"), json{{"driverAfter", hash}, {"service", Narrow(service)}})) return 1;
        auto outcome = InstallUpdate(cfg, update, error, nullptr);
        std::cout << Narrow(error) << " tx=" << PendingTransaction() << "\n";
        return outcome == InstallOutcome::PendingRestart ? 0 : 1;
    }
    if (mode == L"offline-driver-first") {
        bool finalized = FinalizePending(cfg, error);
        DWORD value = 0, keep = 0, size = sizeof(DWORD);
        bool values = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LiberNTUpdateRecoveryProbe", L"Value", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
        size = sizeof(DWORD);
        values = values && RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LiberNTUpdateRecoveryProbe", L"Keep", RRF_RT_REG_DWORD, nullptr, &keep, &size) == ERROR_SUCCESS;
        auto saved = LoadJson(JoinPath(DataDir(), L"victim.json"), json::object());
        bool ok = finalized && !PendingTransaction().empty() && FileExists(BootFile(L"trial")) && values && value == 2 && keep == 73 &&
                  Sha256File(L"C:\\Windows\\System32\\drivers\\lntu_recovery_crash.sys") == saved.value("driverAfter", "");
        std::cout << (ok ? "PASS " : "FAIL ") << "bad driver applied, registry=" << value << " keep=" << keep << " " << Narrow(error) << "\n";
        return ok ? 0 : 1;
    }
    if (mode == L"offline-driver-recovered") {
        auto saved = LoadJson(JoinPath(DataDir(), L"victim.json"), json::object());
        DWORD value = 0, keep = 0, size = sizeof(DWORD);
        bool values = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LiberNTUpdateRecoveryProbe", L"Value", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
        size = sizeof(DWORD);
        values = values && RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LiberNTUpdateRecoveryProbe", L"Keep", RRF_RT_REG_DWORD, nullptr, &keep, &size) == ERROR_SUCCESS;
        RegistryItem service{Widen(saved.value("service", "")), L"", true, false};
        bool absent = ReadRegistryItem(service, error) && !service.present;
        bool ok = values && value == 1 && keep == 73 && PendingTransaction().empty() && InstalledRelease() == "RecoveryBefore" &&
                  !FileExists(L"C:\\Windows\\System32\\drivers\\lntu_recovery_crash.sys") && absent;
        std::cout << (ok ? "PASS " : "FAIL ") << "test driver removed=" << absent
                  << " registry=" << value << " keep=" << keep << " installed=" << InstalledRelease() << " pending=" << PendingTransaction() << "\n";
        return ok ? 0 : 1;
    }
    return 2;
}
