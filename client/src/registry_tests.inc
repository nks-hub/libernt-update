#include "registry.h"
#include "../boot/regplan.h"
#include <sddl.h>
#include <cstring>

static bool RegistrySelfTest(const std::wstring& base, std::wstring& report) {
    bool ok = true;
    auto check = [&](bool result, const wchar_t* name) {
        report += (result ? L"PASS " : L"FAIL ") + std::wstring(name) + L"\r\n";
        ok = ok && result;
    };
    HANDLE token = nullptr;
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> user(size);
    LPWSTR sid = nullptr;
    bool haveSid = size && GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
                   ConvertSidToStringSidW(((TOKEN_USER*)user.data())->User.Sid, &sid);
    CloseHandle(token);
    if (!haveSid) { check(false, L"registry sandbox user SID"); return false; }
    std::wstring subkey = L"Software\\LiberNTUpdateSelfTest-" + Widen(NowId()) + L"-" + std::to_wstring(GetCurrentProcessId());
    std::wstring native = L"\\Registry\\User\\" + std::wstring(sid) + L"\\" + subkey;
    LocalFree(sid);
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key);
    if (key) RegCloseKey(key);
    if (status != ERROR_FILE_NOT_FOUND) { check(false, L"registry sandbox must be absent"); return false; }
    std::wstring desiredFile = JoinPath(base, L"registry-new.bin"), expectedFile = JoinPath(base, L"registry-old.bin");
    std::wstring plan = JoinPath(base, L"registry-plan.txt");
    auto run = [&](const RegistryItem& desired, const RegistryItem& expected, bool damage = false) {
        auto next = EncodeRegistryItem(desired), before = EncodeRegistryItem(expected);
        if (next.empty() || before.empty()) return false;
        if (damage) next[0] ^= 1;
        if (!WriteFileAtomic(desiredFile, next.data(), next.size()) || !WriteFileAtomic(expectedFile, before.data(), before.size()) ||
            !WriteUtf16(plan, L"LUUP 1\nG\t" + NtPath(desiredFile) + L"\t" + NtPath(expectedFile) + L"\n")) return false;
        LUUP_RESULT result{};
        return NT_SUCCESS(LuupApplyPlan(NtPath(plan).c_str(), &result, nullptr)) && result.Done == 1 && !result.Failed;
    };
    auto readNumber = [&]() {
        DWORD value = 0, kind = 0, bytes = sizeof(value);
        HKEY opened = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &opened) != ERROR_SUCCESS) return 0xffffffffUL;
        LONG result = RegQueryValueExW(opened, L"Setting", nullptr, &kind, (BYTE*)&value, &bytes);
        RegCloseKey(opened);
        return result == ERROR_SUCCESS && kind == REG_DWORD && bytes == sizeof(value) ? value : 0xffffffffUL;
    };
    auto flushRetry = [&](const RegistryItem& desired, const RegistryItem& expected) {
        LuupSetRegistryFlushFailures(2);
        bool first = run(desired, expected), second = run(desired, expected), third = run(desired, expected);
        LuupSetRegistryFlushFailures(0);
        return !first && !second && third;
    };
    RegistryItem absentKey{native, L"", true, false}, presentKey{native, L"", true, true};
    RegistryItem absentValue{native, L"Setting", false, false};
    RegistryItem value{native, L"Setting", false, true, REG_DWORD, {1, 0, 0, 0}};
    check(flushRetry(presentKey, absentKey), L"native registry retries a failed key flush before confirming creation");
    check(flushRetry(value, absentValue) && readNumber() == 1, L"native registry retries a failed value flush before confirming its data");
    check(!run(absentKey, presentKey) && readNumber() == 1, L"native registry plan preserves a nonempty key");
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        DWORD custom = 77;
        RegSetValueExW(key, L"Setting", 0, REG_DWORD, (BYTE*)&custom, sizeof(custom));
        RegCloseKey(key);
        key = nullptr;
    }
    check(!run(absentValue, value) && readNumber() == 77, L"native registry rollback preserves a later local change");
    RegistryItem custom{native, L"Setting", false, true, REG_DWORD, {77, 0, 0, 0}};
    check(run(value, custom) && !run(custom, value, true) && readNumber() == 1, L"native registry rejects a malformed record before writing");
    check(run(absentValue, value) && readNumber() == 0xffffffffUL, L"native registry rollback removes its new value");
    check(flushRetry(absentKey, presentKey), L"native registry rollback retries a failed deletion flush");
    RegistryItem nested{native + L"\\Nested\\Leaf", L"Setting", false, true, REG_DWORD, {1, 0, 0, 0}};
    RegistryItem noNested{nested.key, nested.name, false, false};
    check(!run(nested, noNested), L"native registry refuses a value write with unplanned parent keys");
    RegistryItem nestedKey{native + L"\\Nested", L"", true, true}, noNestedKey{nestedKey.key, L"", true, false};
    RegistryItem leafKey{nested.key, L"", true, true}, noLeafKey{nested.key, L"", true, false};
    check(run(presentKey, absentKey) && run(nestedKey, noNestedKey) && run(leafKey, noLeafKey) && run(nested, noNested) &&
          run(noNested, nested) && run(noLeafKey, leafKey) && run(noNestedKey, nestedKey) && run(absentKey, presentKey),
          L"native registry rolls back every explicitly planned parent key");
    RegistryItem unicode{native + L"\\Parametry-\u017elu\u0165ou\u010dk\u00fd", L"N\u00e1zev", false, true, REG_SZ};
    RegistryItem unicodeKey{unicode.key, L"", true, true}, noUnicodeKey{unicode.key, L"", true, false};
    std::wstring text = L"Uk\u00e1zka";
    unicode.data.resize((text.size() + 1) * sizeof(wchar_t));
    memcpy(unicode.data.data(), text.c_str(), unicode.data.size());
    RegistryItem noUnicode{unicode.key, unicode.name, false, false};
    wchar_t actualText[32]{};
    DWORD actualSize = sizeof(actualText);
    check(run(presentKey, absentKey) && run(unicodeKey, noUnicodeKey) && run(unicode, noUnicode) &&
          RegGetValueW(HKEY_CURRENT_USER, (subkey + L"\\Parametry-\u017elu\u0165ou\u010dk\u00fd").c_str(),
          unicode.name.c_str(), RRF_RT_REG_SZ, nullptr, actualText, &actualSize) == ERROR_SUCCESS && text == actualText,
          L"native registry preserves Unicode key names and string data");
    RegistryItem defaultValue{unicode.key, L"", false, true, REG_SZ, unicode.data};
    RegistryItem noDefault{unicode.key, L"", false, false};
    actualText[0] = 0;
    actualSize = sizeof(actualText);
    check(run(defaultValue, noDefault) && RegGetValueW(HKEY_CURRENT_USER, (subkey + L"\\Parametry-\u017elu\u0165ou\u010dk\u00fd").c_str(),
          nullptr, RRF_RT_REG_SZ, nullptr, actualText, &actualSize) == ERROR_SUCCESS && text == actualText,
          L"native registry supports an unnamed default value");
    RegistryItem large{native, L"Large", false, true, REG_BINARY, std::vector<uint8_t>(128 * 1024, 0x5a)};
    RegistryItem noLarge{native, large.name, false, false};
    std::vector<uint8_t> actualBytes(large.data.size());
    DWORD actualType = 0;
    actualSize = (DWORD)actualBytes.size();
    check(run(large, noLarge) && RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), large.name.c_str(), RRF_RT_REG_BINARY,
          &actualType, actualBytes.data(), &actualSize) == ERROR_SUCCESS && actualSize == large.data.size() && actualBytes == large.data,
          L"native registry preserves a value spanning multiple hive cells");
    RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
    return ok;
}
