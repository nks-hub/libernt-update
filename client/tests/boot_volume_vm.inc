#include <winioctl.h>

class SavedBootSetting {
    HKEY key_ = nullptr;
    std::wstring name_;
    DWORD type_ = 0;
    std::vector<BYTE> raw_;
public:
    SavedBootSetting(const wchar_t* key, const wchar_t* name) : name_(name) {
        DWORD size = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key_) != ERROR_SUCCESS) return;
        if (RegQueryValueExW(key_, name, nullptr, &type_, nullptr, &size) != ERROR_SUCCESS) return;
        raw_.resize(size);
        if (RegQueryValueExW(key_, name, nullptr, &type_, raw_.data(), &size) != ERROR_SUCCESS) raw_.clear();
    }
    ~SavedBootSetting() { if (key_) { Restore(); RegCloseKey(key_); } }
    bool Valid() const { return key_ && !raw_.empty(); }
    bool Text(const std::wstring& text) {
        return RegSetValueExW(key_, name_.c_str(), 0, REG_SZ, (const BYTE*)text.c_str(),
                             (DWORD)((text.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    bool Remove() { return RegDeleteValueW(key_, name_.c_str()) == ERROR_SUCCESS; }
    bool WrongType() {
        DWORD value = 0;
        return RegSetValueExW(key_, name_.c_str(), 0, REG_DWORD, (const BYTE*)&value, sizeof value) == ERROR_SUCCESS;
    }
    bool Restore() {
        return Valid() && RegSetValueExW(key_, name_.c_str(), 0, type_, raw_.data(), (DWORD)raw_.size()) == ERROR_SUCCESS &&
               RegFlushKey(key_) == ERROR_SUCCESS;
    }
};

static int BootVolumeRegression() {
    g_dataOverride.clear();
    Config cfg;
    cfg.root = L"C:\\";
    SavedBootSetting setup(L"SYSTEM\\Setup", L"SystemPartition");
    SavedBootSetting firmware(L"SYSTEM\\CurrentControlSet\\Control", L"FirmwareBootDevice");
    wchar_t systemArc[512]{}, systemDevice[512]{};
    DWORD bytes = sizeof systemArc;
    if (!setup.Valid() || !firmware.Valid() || !QueryDosDeviceW(L"C:", systemDevice, 512) ||
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SystemBootDevice",
                     RRF_RT_REG_SZ, nullptr, systemArc, &bytes) != ERROR_SUCCESS) return 1;
    bool ok = true;
    auto check = [&](bool result, const char* name) { std::cout << (result ? "PASS " : "FAIL ") << name << "\n"; ok = ok && result; };
    auto target = [&]() { return TargetPath(cfg, "BootPartition/EFI/LiberNT/Boot/bootmgfw.efi"); };
    std::wstring loader = target();
    check(!loader.empty() && FileExists(loader), "active EFI loader identified");
    if (loader.empty()) return 1;
    check(setup.Remove() && target() == loader, "missing Setup value uses verified firmware partition");
    check(setup.Text(systemDevice) && target().empty(), "conflicting Setup partition is refused");
    check(setup.WrongType() && target().empty(), "malformed Setup value is refused");
    check(setup.Restore(), "original Setup value restored");
    check(firmware.Text(systemArc) && target().empty(), "firmware fallback to the OS partition is refused");
    check(firmware.Text(L"..\\Device\\HarddiskVolume1") && target().empty(), "invalid ARC path is refused");
    check(firmware.Restore() && target() == loader, "original firmware value restored");
    return ok ? 0 : 1;
}

static int BootVolumeProbe(bool expectAmbiguous = false) {
    g_dataOverride.clear();
    Config cfg;
    cfg.root = L"C:\\";
    using FirmwareQuery = BOOL(WINAPI*)(DWORD*);
    auto query = (FirmwareQuery)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareType");
    DWORD firmware = 0;
    BOOL firmwareOk = query && query(&firmware);
    std::cout << "firmware_ok=" << firmwareOk << " firmware=" << firmware << "\n";
    auto loader = TargetPath(cfg, "BootPartition/EFI/LiberNT/Boot/bootmgfw.efi");
    std::cout << "loader=" << Narrow(loader) << "\n";
    wchar_t volume[MAX_PATH]{};
    HANDLE find = FindFirstVolumeW(volume, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return 1;
    do {
        std::wstring name(volume + 4);
        name.pop_back();
        wchar_t device[512]{}, fs[32]{};
        QueryDosDeviceW(name.c_str(), device, 512);
        GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, fs, 32);
        std::wstring raw = volume;
        raw.pop_back();
        HANDLE handle = CreateFileW(raw.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        PARTITION_INFORMATION_EX partition{};
        STORAGE_DEVICE_NUMBER number{};
        DWORD bytes = 0;
        bool partOk = handle != INVALID_HANDLE_VALUE && DeviceIoControl(handle, IOCTL_DISK_GET_PARTITION_INFO_EX,
            nullptr, 0, &partition, sizeof partition, &bytes, nullptr);
        DWORD partError = partOk ? 0 : GetLastError();
        bool numberOk = handle != INVALID_HANDLE_VALUE && DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr, 0, &number, sizeof number, &bytes, nullptr);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        std::cout << "volume=" << Narrow(volume) << " device=" << Narrow(device) << " fs=" << Narrow(fs)
                  << " part_ok=" << partOk << " part_error=" << partError << " style=" << partition.PartitionStyle
                  << " type=" << (unsigned)partition.Mbr.PartitionType << " number_ok=" << numberOk
                  << " disk=" << number.DeviceNumber << " partition=" << number.PartitionNumber << "\n";
    } while (FindNextVolumeW(find, volume, MAX_PATH));
    FindVolumeClose(find);
    if (expectAmbiguous) {
        std::cout << (loader.empty() ? "PASS" : "FAIL") << " automatic selection with a cloned ESP is refused\n";
        return loader.empty() ? 0 : 1;
    }
    return 0;
}
