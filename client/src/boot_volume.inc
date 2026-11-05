namespace {

LONG ReadBootString(const wchar_t* key, const wchar_t* name, std::wstring& value) {
    wchar_t data[512]{};
    DWORD size = sizeof data;
    LONG status = RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, data, &size);
    if (status != ERROR_SUCCESS) return status;
    value = data;
    return value.empty() ? ERROR_INVALID_DATA : ERROR_SUCCESS;
}

bool DiskNumber(const std::wstring& path, STORAGE_DEVICE_NUMBER& number) {
    HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD bytes = 0;
    bool ok = file != INVALID_HANDLE_VALUE && DeviceIoControl(file, IOCTL_STORAGE_GET_DEVICE_NUMBER,
        nullptr, 0, &number, sizeof number, &bytes, nullptr) && bytes >= sizeof number && number.DeviceType == FILE_DEVICE_DISK;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

bool EfiPartition(const PARTITION_INFORMATION_EX& part) {
    static const GUID esp = {0xc12a7328, 0xf81f, 0x11d2, {0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b}};
    return part.PartitionStyle == PARTITION_STYLE_MBR ? part.Mbr.PartitionType == 0xef
         : part.PartitionStyle == PARTITION_STYLE_GPT && IsEqualGUID(part.Gpt.PartitionType, esp);
}

bool SoleEfiPartition(const STORAGE_DEVICE_NUMBER& target) {
    std::vector<wchar_t> names(32768);
    while (!QueryDosDeviceW(nullptr, names.data(), (DWORD)names.size())) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || names.size() >= 1024 * 1024) return false;
        names.resize(names.size() * 2);
    }
    size_t count = 0;
    bool matched = false;
    for (const wchar_t* name = names.data(); *name; name += wcslen(name) + 1) {
        if (_wcsnicmp(name, L"PhysicalDrive", 13) || !name[13]) continue;
        const wchar_t* suffix = name + 13;
        while (*suffix >= L'0' && *suffix <= L'9') suffix++;
        if (*suffix) continue;
        // QueryDosDevice includes global names; open the global disk even if a local alias shadows its name.
        std::wstring path = L"\\\\?\\GLOBALROOT\\GLOBAL??\\" + std::wstring(name);
        HANDLE disk = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (disk == INVALID_HANDLE_VALUE) return false;
        std::vector<BYTE> raw(4096);
        DWORD bytes = 0, error = ERROR_SUCCESS;
        while (!DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, raw.data(), (DWORD)raw.size(), &bytes, nullptr)) {
            error = GetLastError();
            if ((error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA) || raw.size() >= 1024 * 1024) break;
            raw.resize(raw.size() * 2);
            error = ERROR_SUCCESS;
        }
        STORAGE_DEVICE_NUMBER number{};
        DWORD numberBytes = 0;
        bool numberOk = DeviceIoControl(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                        &number, sizeof number, &numberBytes, nullptr) != 0;
        CloseHandle(disk);
        if (error == ERROR_NOT_READY) continue;
        size_t header = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
        if (error || !numberOk || numberBytes < sizeof number || number.DeviceType != FILE_DEVICE_DISK || bytes < header) return false;
        auto layout = (const DRIVE_LAYOUT_INFORMATION_EX*)raw.data();
        if (layout->PartitionCount > (bytes - header) / sizeof(PARTITION_INFORMATION_EX)) return false;
        for (DWORD i = 0; i < layout->PartitionCount; i++) {
            const auto& part = layout->PartitionEntry[i];
            if (!EfiPartition(part)) continue;
            count++;
            if (number.DeviceNumber == target.DeviceNumber && part.PartitionNumber == target.PartitionNumber) matched = true;
        }
    }
    return count == 1 && matched;
}

std::wstring FirmwareBootDevice() {
    std::wstring arc;
    if (ReadBootString(L"SYSTEM\\CurrentControlSet\\Control", L"FirmwareBootDevice", arc) != ERROR_SUCCESS) return {};
    for (wchar_t c : arc)
        if (!(c >= L'a' && c <= L'z') && !(c >= L'A' && c <= L'Z') && !(c >= L'0' && c <= L'9') && c != L'(' && c != L')') return {};
    std::wstring path = L"\\ArcName\\" + arc;
    UNICODE_STRING name{};
    RtlInitUnicodeString(&name, path.c_str());
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof attributes;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    using OpenLink = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using QueryLink = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto open = (OpenLink)GetProcAddress(nt, "NtOpenSymbolicLinkObject");
    auto query = (QueryLink)GetProcAddress(nt, "NtQuerySymbolicLinkObject");
    HANDLE link = nullptr;
    if (!open || !query || open(&link, 1, &attributes) < 0) return {};
    wchar_t buffer[512]{};
    UNICODE_STRING result{0, sizeof buffer, buffer};
    NTSTATUS status = query(link, &result, nullptr);
    CloseHandle(link);
    if (status < 0 || !result.Length || result.Length % sizeof(wchar_t) || result.Length > sizeof buffer) return {};
    return std::wstring(buffer, result.Length / sizeof(wchar_t));
}

bool PlainEfiLoader(const std::wstring& volume) {
    for (const wchar_t* path : {L"EFI", L"EFI\\LiberNT", L"EFI\\LiberNT\\Boot"}) {
        DWORD attrs = GetFileAttributesW(JoinPath(volume, path).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    }
    DWORD attrs = GetFileAttributesW(JoinPath(volume, L"EFI\\LiberNT\\Boot\\bootmgfw.efi").c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

std::wstring DetectEfiPartition(const Config& cfg) {
    std::wstring bootDevice = FirmwareBootDevice(), setupDevice;
    if (bootDevice.empty()) return {};
    STORAGE_DEVICE_NUMBER boot{}, system{};
    wchar_t systemRoot[MAX_PATH]{}, systemVolume[MAX_PATH]{};
    if (!GetVolumePathNameW(cfg.root.c_str(), systemRoot, MAX_PATH) ||
        !GetVolumeNameForVolumeMountPointW(systemRoot, systemVolume, MAX_PATH)) return {};
    std::wstring systemPath(systemVolume);
    systemPath.pop_back();
    if (!DiskNumber(L"\\\\?\\GLOBALROOT" + bootDevice, boot) || !DiskNumber(systemPath, system)) return {};
    if (boot.DeviceNumber == system.DeviceNumber && boot.PartitionNumber == system.PartitionNumber) return {};
    if (!SoleEfiPartition(boot)) return {};
    LONG setupStatus = ReadBootString(L"SYSTEM\\Setup", L"SystemPartition", setupDevice);
    if (setupStatus != ERROR_SUCCESS && setupStatus != ERROR_FILE_NOT_FOUND && setupStatus != ERROR_PATH_NOT_FOUND) return {};
    STORAGE_DEVICE_NUMBER setup{};
    if (setupStatus == ERROR_SUCCESS && (!DiskNumber(L"\\\\?\\GLOBALROOT" + setupDevice, setup) ||
        setup.DeviceNumber != boot.DeviceNumber || setup.PartitionNumber != boot.PartitionNumber)) return {};
    wchar_t volume[MAX_PATH]{};
    HANDLE find = FindFirstVolumeW(volume, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return {};
    std::wstring chosen;
    size_t matches = 0;
    do {
        std::wstring raw(volume);
        raw.pop_back();
        STORAGE_DEVICE_NUMBER number{};
        if (!DiskNumber(raw, number) || number.DeviceNumber != boot.DeviceNumber || number.PartitionNumber != boot.PartitionNumber) continue;
        HANDLE file = CreateFileW(raw.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        PARTITION_INFORMATION_EX part{};
        DWORD bytes = 0;
        bool valid = file != INVALID_HANDLE_VALUE && DeviceIoControl(file, IOCTL_DISK_GET_PARTITION_INFO_EX,
            nullptr, 0, &part, sizeof part, &bytes, nullptr) && bytes >= sizeof part && EfiPartition(part);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        wchar_t fs[32]{};
        if (!valid || !GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, fs, 32) ||
            (_wcsicmp(fs, L"FAT") && _wcsicmp(fs, L"FAT32")) || !PlainEfiLoader(volume)) continue;
        matches++;
        chosen = volume;
    } while (FindNextVolumeW(find, volume, MAX_PATH));
    DWORD enumerationError = GetLastError();
    FindVolumeClose(find);
    return enumerationError == ERROR_NO_MORE_FILES && matches == 1 ? chosen : std::wstring();
}

}
