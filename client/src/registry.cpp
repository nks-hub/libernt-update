#include "registry.h"
#include "../boot/regplan.h"
#include "runtime.h"
#include "util.h"
#include <algorithm>
#include <cstring>
#include <set>

extern "C" {
NTSYSAPI LONG NTAPI RtlCompareUnicodeString(PCUNICODE_STRING, PCUNICODE_STRING, BOOLEAN);
NTSYSAPI NTSTATUS NTAPI NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
}

namespace {
std::wstring testRoot;

bool OpenRegistryKey(const std::wstring& key, HKEY& handle, std::wstring& error) {
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    RtlInitUnicodeString(&name, key.c_str());
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    NTSTATUS status = NtOpenKey((PHANDLE)&handle, KEY_READ, &attributes);
    if (status == (NTSTATUS)0xc0000034 || status == (NTSTATUS)0xc000003a) { handle = nullptr; return true; }
    if (NT_SUCCESS(status)) return true;
    error = L"Cannot read registry key: " + key + L" (NTSTATUS " + std::to_wstring((ULONG)status) + L")";
    return false;
}

void RegistryPaths(const RegistrySnapshot& snapshot, RegistrySnapshot& out, DWORD control) {
    auto path = [&](const std::wstring& key) {
        if (!testRoot.empty()) return testRoot + L"\\" + key;
        const std::wstring system = L"SYSTEM\\CurrentControlSet\\";
        if (key.rfind(system, 0) == 0) {
            wchar_t selected[32];
            swprintf_s(selected, L"ControlSet%03u", control);
            return L"\\Registry\\Machine\\SYSTEM\\" + std::wstring(selected) + L"\\" + key.substr(system.size());
        }
        if (key.rfind(L"SOFTWARE\\", 0) == 0) return L"\\Registry\\Machine\\" + key;
        return L"\\Registry\\User\\.DEFAULT\\" + key.substr(8);
    };
    out = {};
    for (auto& [key, item] : snapshot.keys) {
        RegistryItem copy = item;
        copy.key = path(key);
        out.keys.emplace(copy.key, copy);
    }
    for (auto& [key, values] : snapshot.values) for (auto& [name, item] : values) {
        RegistryItem copy = item;
        copy.key = path(key);
        out.values[copy.key].emplace(name, copy);
    }
}
}

bool RegistryNameLess::operator()(const std::wstring& left, const std::wstring& right) const {
    UNICODE_STRING a, b;
    RtlInitUnicodeString(&a, left.c_str());
    RtlInitUnicodeString(&b, right.c_str());
    return RtlCompareUnicodeString(&a, &b, TRUE) < 0;
}

void SetRegistryTestRoot(const std::wstring& root) {
    if (IsTestData()) testRoot = root;
}

bool SameRegistryData(const RegistryItem& left, const RegistryItem& right) {
    return left.present == right.present && (!left.present || (left.type == right.type && left.data == right.data));
}

bool RegistryUpdatesSupported() {
    DWORD version = 0, size = sizeof(version);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\LiberNTUpdate", L"Version",
                         RRF_RT_REG_DWORD, nullptr, &version, &size) == ERROR_SUCCESS && version == 1;
}

std::wstring RegistryDisplayName(const RegistryItem& item) {
    std::wstring key = item.key;
    if (key.rfind(L"\\Registry\\Machine\\", 0) == 0) key = L"HKLM\\" + key.substr(18);
    else if (key.rfind(L"\\Registry\\User\\", 0) == 0) key = L"HKU\\" + key.substr(15);
    return key + (item.keyOnly ? L"" : L"\\" + (item.name.empty() ? std::wstring(Tr(L"(výchozí)", L"(default)")) : item.name));
}

bool ReadRegistryItem(RegistryItem& item, std::wstring& error) {
    item.present = false;
    item.type = REG_NONE;
    item.data.clear();
    HKEY key = nullptr;
    if (!OpenRegistryKey(item.key, key, error)) return false;
    if (!key) return true;
    if (item.keyOnly) { item.present = true; RegCloseKey(key); return true; }
    DWORD size = 0;
    LONG status = RegQueryValueExW(key, item.name.c_str(), nullptr, &item.type, nullptr, &size);
    if (status == ERROR_SUCCESS && size <= LUUP_REG_MAX_DATA) {
        item.data.resize(size);
        status = RegQueryValueExW(key, item.name.c_str(), nullptr, &item.type, item.data.data(), &size);
        if (status == ERROR_SUCCESS) { item.data.resize(size); item.present = true; }
    } else if (status == ERROR_SUCCESS) status = ERROR_FILE_TOO_LARGE;
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) { item.type = REG_NONE; item.data.clear(); return true; }
    if (status == ERROR_SUCCESS) return true;
    error = L"Cannot read registry value: " + item.key + L"\\" + item.name + L": " + LastErrorText(status);
    return false;
}

bool RegistryChildren(const std::wstring& path, std::vector<std::wstring>& keys, std::vector<std::wstring>& values, std::wstring& error) {
    keys.clear(); values.clear();
    HKEY key = nullptr;
    if (!OpenRegistryKey(path, key, error)) return false;
    if (!key) return true;
    LONG status = ERROR_SUCCESS;
    wchar_t name[16384];
    for (DWORD i = 0; ; i++) {
        DWORD chars = _countof(name);
        status = RegEnumValueW(key, i, name, &chars, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS) break;
        values.emplace_back(name, chars);
    }
    if (status == ERROR_NO_MORE_ITEMS) for (DWORD i = 0; ; i++) {
        DWORD chars = _countof(name);
        status = RegEnumKeyExW(key, i, name, &chars, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS) break;
        keys.emplace_back(name, chars);
    }
    RegCloseKey(key);
    if (status == ERROR_NO_MORE_ITEMS) return true;
    error = L"Cannot enumerate registry key: " + path + L": " + LastErrorText(status);
    return false;
}

bool PlanRegistryChanges(const RegistrySnapshot& before, const RegistrySnapshot& after,
                         std::vector<RegistryChange>& out, size_t& preserved, std::wstring& error) {
    RegistrySnapshot old, next;
    out.clear();
    preserved = 0;
    DWORD control = 0, size = sizeof(control), type = 0;
    if (testRoot.empty()) {
        HKEY select = nullptr;
        LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Select", 0, KEY_QUERY_VALUE, &select);
        if (status == ERROR_SUCCESS) {
            status = RegQueryValueExW(select, L"Current", nullptr, &type, (BYTE*)&control, &size);
            RegCloseKey(select);
        }
        if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(control) || !control || control > 999) {
            error = L"Cannot identify the selected SYSTEM control set.";
            return false;
        }
    }
    RegistryPaths(before, old, control);
    RegistryPaths(after, next, control);
    std::vector<RegistryChange> changes;
    std::set<std::wstring, RegistryNameLess> created, removed;
    std::map<std::wstring, std::set<std::wstring, RegistryNameLess>, RegistryNameLess> removedValues;
    std::vector<RegistryItem> keys;
    for (auto& [key, item] : next.keys) if (!old.keys.count(key)) keys.push_back(item);
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.key.size() < b.key.size(); });
    for (auto item : keys) {
        if (!ReadRegistryItem(item, error)) return false;
        if (item.present) continue;
        RegistryItem parent{item.key.substr(0, item.key.find_last_of(L'\\')), L"", true};
        if (!ReadRegistryItem(parent, error)) return false;
        if (!parent.present && !created.count(parent.key)) { preserved++; continue; }
        auto desired = item;
        desired.present = true;
        changes.push_back({item, desired});
        created.insert(item.key);
    }
    auto offer = [&](const RegistryItem* original, const RegistryItem* desired) {
        RegistryItem absent = original ? *original : *desired;
        absent.present = false; absent.type = REG_NONE; absent.data.clear();
        const RegistryItem& a = original ? *original : absent;
        const RegistryItem& b = desired ? *desired : absent;
        if (SameRegistryData(a, b)) return true;
        RegistryItem current = a;
        if (!ReadRegistryItem(current, error)) return false;
        if (SameRegistryData(current, b)) return true;
        if (!SameRegistryData(current, a)) { preserved++; return true; }
        if (b.present) {
            RegistryItem parent{b.key, L"", true};
            if (!ReadRegistryItem(parent, error)) return false;
            if (!parent.present && !created.count(parent.key)) { preserved++; return true; }
        }
        changes.push_back({current, b});
        if (!b.present) removedValues[b.key].insert(b.name);
        return true;
    };
    for (auto& [key, values] : next.values) for (auto& [name, item] : values) {
        auto oldKey = old.values.find(key);
        const RegistryItem* previous = nullptr;
        if (oldKey != old.values.end()) {
            auto found = oldKey->second.find(name);
            if (found != oldKey->second.end()) previous = &found->second;
        }
        if (!offer(previous, &item)) return false;
    }
    for (auto& [key, values] : old.values) for (auto& [name, item] : values) {
        auto newKey = next.values.find(key);
        if ((newKey == next.values.end() || !newKey->second.count(name)) && !offer(&item, nullptr)) return false;
    }
    keys.clear();
    for (auto& [key, item] : old.keys) if (!next.keys.count(key)) keys.push_back(item);
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.key.size() > b.key.size(); });
    for (auto item : keys) {
        if (!ReadRegistryItem(item, error)) return false;
        if (!item.present) continue;
        std::vector<std::wstring> children, values;
        if (!RegistryChildren(item.key, children, values, error)) return false;
        bool empty = std::all_of(values.begin(), values.end(), [&](const auto& name) { return removedValues[item.key].count(name); }) &&
                     std::all_of(children.begin(), children.end(), [&](const auto& name) { return removed.count(item.key + L"\\" + name); });
        if (!empty) { preserved++; continue; }
        RegistryItem absent = item;
        absent.present = false;
        changes.push_back({item, absent});
        removed.insert(item.key);
    }
    out.swap(changes);
    return true;
}

std::vector<uint8_t> EncodeRegistryItem(const RegistryItem& item) {
    if (item.key.empty() || item.key.front() != L'\\' || item.key.back() == L'\\' ||
        item.key.size() > 32760 || item.name.size() > 16383 || item.data.size() > LUUP_REG_MAX_DATA ||
        item.type > REG_QWORD || item.type == REG_LINK ||
        item.key.find(L'\0') != std::wstring::npos || item.name.find(L'\0') != std::wstring::npos ||
        (!item.present && (item.type || !item.data.empty())) ||
        (item.keyOnly && (!item.name.empty() || item.type || !item.data.empty()))) return {};
    LUUP_REG_RECORD record{LUUP_REG_MAGIC, item.keyOnly ? LUUP_REG_KEY : LUUP_REG_VALUE,
                           (ULONG)item.key.size(), (ULONG)item.name.size(), item.present ? 1UL : 0UL,
                           item.type, (ULONG)item.data.size()};
    size_t keyBytes = (item.key.size() + 1) * sizeof(wchar_t);
    size_t nameBytes = (item.name.size() + 1) * sizeof(wchar_t);
    std::vector<uint8_t> raw(sizeof(record) + keyBytes + nameBytes + item.data.size());
    memcpy(raw.data(), &record, sizeof(record));
    memcpy(raw.data() + sizeof(record), item.key.c_str(), keyBytes);
    memcpy(raw.data() + sizeof(record) + keyBytes, item.name.c_str(), nameBytes);
    if (!item.data.empty()) memcpy(raw.data() + sizeof(record) + keyBytes + nameBytes, item.data.data(), item.data.size());
    return raw;
}

bool DecodeRegistryItem(const std::vector<uint8_t>& raw, RegistryItem& item) {
    if (raw.size() < sizeof(LUUP_REG_RECORD)) return false;
    LUUP_REG_RECORD header;
    memcpy(&header, raw.data(), sizeof(header));
    uint64_t bytes = sizeof(header) + ((uint64_t)header.KeyChars + header.NameChars + 2) * sizeof(wchar_t) + header.DataBytes;
    if (header.Magic != LUUP_REG_MAGIC || (header.Kind != LUUP_REG_KEY && header.Kind != LUUP_REG_VALUE) ||
        header.Present > 1 || bytes != raw.size() || header.KeyChars > 32760 || header.NameChars > 16383 ||
        header.DataBytes > LUUP_REG_MAX_DATA) return false;
    auto key = reinterpret_cast<const wchar_t*>(raw.data() + sizeof(header));
    auto name = key + header.KeyChars + 1;
    auto data = reinterpret_cast<const uint8_t*>(name + header.NameChars + 1);
    item = {std::wstring(key, header.KeyChars), std::wstring(name, header.NameChars), header.Kind == LUUP_REG_KEY,
            header.Present != 0, header.Type, std::vector<uint8_t>(data, data + header.DataBytes)};
    return EncodeRegistryItem(item) == raw;
}
