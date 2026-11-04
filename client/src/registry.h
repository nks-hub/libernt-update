#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct RegistryItem {
    std::wstring key, name;
    bool keyOnly = false;
    bool present = false;
    DWORD type = REG_NONE;
    std::vector<uint8_t> data;
};

std::vector<uint8_t> EncodeRegistryItem(const RegistryItem& item);
