#include "registry.h"
#include "util.h"
#include "../third_party/nlohmann/json.hpp"
#include <algorithm>
#include <stdexcept>

using json = nlohmann::json;

namespace {
std::wstring RegistryText(const json& value, bool key) {
    const auto text = value.get<std::string>();
    auto wide = Widen(text);
    if (Narrow(wide) != text || wide.find(L'\0') != std::wstring::npos ||
        wide.size() > (key ? 32000 : 16383) || (key && (wide.empty() || wide.front() == L'\\' ||
        wide.back() == L'\\' || wide.find(L"\\\\") != std::wstring::npos)))
        throw std::invalid_argument("invalid registry name");
    return wide;
}

unsigned Nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::invalid_argument("invalid registry data");
}
}

bool ParseRegistrySnapshot(const std::vector<uint8_t>& raw, RegistrySnapshot& out, std::wstring& error) {
    out = {};
    try {
        if (raw.size() > 16 * 1024 * 1024) throw std::invalid_argument("registry snapshot exceeds 16 MiB");
        auto snapshot = json::parse(raw.begin(), raw.end());
        if (snapshot.at("format") != 1 || !snapshot.at("hives").is_object() || snapshot["hives"].size() != 3)
            throw std::invalid_argument("invalid registry snapshot format");
        for (const char* hive : {"SYSTEM", "SOFTWARE", "DEFAULT"}) {
            auto& contents = snapshot.at("hives").at(hive);
            if (!contents.at("keys").is_array() || !contents.at("values").is_array())
                throw std::invalid_argument("invalid registry hive");
            auto fullKey = [&](const json& value) {
                auto key = RegistryText(value, true);
                if (std::string(hive) == "SYSTEM" && key != L"CurrentControlSet\\Control" &&
                    key != L"CurrentControlSet\\Services" && key.rfind(L"CurrentControlSet\\Control\\", 0) != 0 &&
                    key.rfind(L"CurrentControlSet\\Services\\", 0) != 0)
                    throw std::invalid_argument("SYSTEM snapshot must contain only Control and Services");
                return Widen(hive) + L"\\" + key;
            };
            for (auto& key : contents["keys"]) {
                auto name = fullKey(key);
                if (!out.keys.emplace(name, RegistryItem{name, L"", true, true}).second)
                    throw std::invalid_argument("duplicate registry key");
            }
            for (auto& value : contents["values"]) {
                RegistryItem item{fullKey(value.at("key")), RegistryText(value.at("name"), false), false, true};
                if (!out.keys.count(item.key) || !value.at("type").is_number_unsigned())
                    throw std::invalid_argument("invalid registry value key or type");
                auto type = value["type"].get<uint64_t>();
                if (type > REG_QWORD || type == REG_LINK) throw std::invalid_argument("unsupported registry value type");
                item.type = (DWORD)type;
                auto hex = value.at("data").get<std::string>();
                if (hex.size() % 2 || hex.size() > 2 * 1024 * 1024) throw std::invalid_argument("invalid registry value size");
                item.data.reserve(hex.size() / 2);
                for (size_t i = 0; i < hex.size(); i += 2) item.data.push_back((uint8_t)(16 * Nibble(hex[i]) + Nibble(hex[i + 1])));
                if (!out.values[item.key].emplace(item.name, item).second) throw std::invalid_argument("duplicate registry value");
            }
        }
        for (auto& [name, item] : out.keys) {
            auto parent = name.substr(0, name.find_last_of(L'\\'));
            if (parent != L"SYSTEM\\CurrentControlSet" && parent != L"SOFTWARE" && parent != L"DEFAULT" && !out.keys.count(parent))
                throw std::invalid_argument("registry snapshot omits a parent key");
        }
        return true;
    } catch (const json::exception&) {
        error = L"Invalid registry snapshot JSON.";
    } catch (const std::invalid_argument& failure) {
        error = Widen(failure.what());
    }
    out = {};
    return false;
}
