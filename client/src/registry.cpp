#include "registry.h"
#include "../boot/regplan.h"
#include <cstring>

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
