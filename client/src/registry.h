#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

struct RegistryItem {
    std::wstring key, name;
    bool keyOnly = false;
    bool present = false;
    DWORD type = REG_NONE;
    std::vector<uint8_t> data;
};

std::vector<uint8_t> EncodeRegistryItem(const RegistryItem& item);
bool DecodeRegistryItem(const std::vector<uint8_t>& raw, RegistryItem& item);

struct RegistryNameLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const;
};
struct RegistrySnapshot {
    std::map<std::wstring, RegistryItem, RegistryNameLess> keys;
    std::map<std::wstring, std::map<std::wstring, RegistryItem, RegistryNameLess>, RegistryNameLess> values;
};
struct RegistryChange { RegistryItem before, after; };

bool ParseRegistrySnapshot(const std::vector<uint8_t>& raw, RegistrySnapshot& out, std::wstring& error);
bool ReadRegistryItem(RegistryItem& item, std::wstring& error);
bool RegistryChildren(const std::wstring& path, std::vector<std::wstring>& keys, std::vector<std::wstring>& values, std::wstring& error);
bool SameRegistryData(const RegistryItem& left, const RegistryItem& right);
bool RegistryUpdatesSupported();
std::wstring RegistryDisplayName(const RegistryItem& item);
bool PlanRegistryChanges(const RegistrySnapshot& before, const RegistrySnapshot& after,
                         std::vector<RegistryChange>& out, size_t& preserved, std::wstring& error);
// Isolated self-tests map the three hives below an owned HKCU key.
void SetRegistryTestRoot(const std::wstring& root);
