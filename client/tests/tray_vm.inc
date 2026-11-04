#include "../src/tray.h"

static const wchar_t* TrayTestDirectory() { return L"C:\\lntu-tray-test"; }

static BOOL CALLBACK FindTestTray(HWND window, LPARAM result) {
    wchar_t name[64]{};
    GetClassNameW(window, name, _countof(name));
    if (wcscmp(name, L"LiberNTUpdateTray")) return TRUE;
    *(HWND*)result = window;
    return FALSE;
}

static HWND TestTrayWindow() {
    HWND window = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindTestTray, (LPARAM)&window);
    return window;
}

static BOOL CALLBACK FindTrayMenu(HWND window, LPARAM result) {
    wchar_t name[64]{};
    GetClassNameW(window, name, _countof(name));
    if (!wcscmp(name, L"#32768")) GetWindowRect(window, (RECT*)result);
    return TRUE;
}

static json OtherTrayPins(bool& ok) {
    json pins = json::object();
    HKEY root = nullptr;
    ok = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) == ERROR_SUCCESS;
    if (!ok) return pins;
    wchar_t own[MAX_PATH]{};
    GetModuleFileNameW(nullptr, own, _countof(own));
    for (DWORD i = 0; ; i++) {
        wchar_t name[256]{};
        DWORD chars = _countof(name);
        LONG status = RegEnumKeyExW(root, i, name, &chars, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        HKEY key = nullptr;
        if (status != ERROR_SUCCESS || RegOpenKeyExW(root, name, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) { ok = false; break; }
        wchar_t executable[MAX_PATH]{};
        DWORD bytes = sizeof(executable), uid = 0, uidBytes = sizeof(uid);
        bool self = RegGetValueW(key, nullptr, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, executable, &bytes) == ERROR_SUCCESS &&
                    RegGetValueW(key, nullptr, L"UID", RRF_RT_REG_DWORD, nullptr, &uid, &uidBytes) == ERROR_SUCCESS &&
                    uid == kTrayIconId && !_wcsicmp(own, executable);
        if (!self) {
            DWORD value = 0;
            bytes = sizeof(value);
            status = RegGetValueW(key, nullptr, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &value, &bytes);
            if (status == ERROR_SUCCESS) pins[Narrow(name)] = value;
            else if (status == ERROR_FILE_NOT_FOUND) pins[Narrow(name)] = nullptr;
            else ok = false;
        }
        RegCloseKey(key);
    }
    RegCloseKey(root);
    return pins;
}

static void TrayTestReport(HWND window) {
    bool pinned = false;
    std::wstring error;
    bool supported = ReadTrayPin(pinned, error);
    NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
    identifier.hWnd = window;
    identifier.uID = kTrayIconId;
    RECT icon{}, taskbar{}, overlap{}, menu{};
    EnumThreadWindows(GetCurrentThreadId(), FindTrayMenu, (LPARAM)&menu);
    using IconRect = HRESULT(WINAPI*)(const NOTIFYICONIDENTIFIER*, RECT*);
    auto query = (IconRect)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "Shell_NotifyIconGetRect");
    HRESULT status = query ? query(&identifier, &icon) : E_NOTIMPL;
    HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    bool onTaskbar = SUCCEEDED(status) && bar && GetWindowRect(bar, &taskbar) && IntersectRect(&overlap, &icon, &taskbar);
    bool otherPinsUnchanged = false;
    json others = OtherTrayPins(otherPinsUnchanged);
    auto baseline = JoinPath(TrayTestDirectory(), L"other-pins.json");
    if (!FileExists(baseline) && otherPinsUnchanged && !WriteTextAtomic(baseline, others.dump())) otherPinsUnchanged = false;
    json previous = LoadJson(baseline, nullptr);
    if (!previous.is_object()) otherPinsUnchanged = false;
    else for (auto item = previous.begin(); item != previous.end(); ++item)
        if (!others.contains(item.key()) || others[item.key()] != item.value()) otherPinsUnchanged = false;
    WriteTextAtomic(JoinPath(TrayTestDirectory(), L"report.json"),
                    json{{"pid", GetCurrentProcessId()}, {"supported", supported}, {"pinned", pinned},
                         {"onTaskbar", onTaskbar}, {"rectStatus", (long)status}, {"error", Narrow(error)},
                         {"otherPinsUnchanged", otherPinsUnchanged},
                         {"rect", {icon.left, icon.top, icon.right, icon.bottom}},
                         {"menu", {menu.left, menu.top, menu.right, menu.bottom}}}.dump(1));
}

static VOID CALLBACK TrayTestTimer(HWND, UINT, UINT_PTR, DWORD) {
    HWND window = TestTrayWindow();
    if (!window) return;
    std::vector<uint8_t> raw;
    auto action = JoinPath(TrayTestDirectory(), L"action.txt");
    if (ReadFileBytes(action, raw, 64)) {
        DeleteFileW(action.c_str());
        std::string command(raw.begin(), raw.end());
        while (!command.empty() && (command.back() == '\r' || command.back() == '\n')) command.pop_back();
        if (command == "quit") { PostMessageW(window, WM_CLOSE, 0, 0); return; }
        bool pinned = false;
        std::wstring error;
        bool supported = ReadTrayPin(pinned, error);
        if (supported && ((command == "pin" && !pinned) || (command == "unpin" && pinned)))
            SendMessageW(window, WM_COMMAND, kTrayPinCommand, 0);
        else if (command == "menu") {
            NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
            identifier.hWnd = window; identifier.uID = kTrayIconId;
            RECT icon{};
            using IconRect = HRESULT(WINAPI*)(const NOTIFYICONIDENTIFIER*, RECT*);
            auto query = (IconRect)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "Shell_NotifyIconGetRect");
            if (query && SUCCEEDED(query(&identifier, &icon))) SetCursorPos((icon.left + icon.right) / 2, (icon.top + icon.bottom) / 2);
            PostMessageW(window, WM_APP + 10, kTrayIconId, WM_RBUTTONUP);
        } else if (command == "close-menu") EndMenu();
        TrayTestReport(window);
    } else if (!FileExists(JoinPath(TrayTestDirectory(), L"report.json"))) TrayTestReport(window);
}

static int RunTrayTest() {
    g_dataOverride = TrayTestDirectory();
    if (!EnsureDirs(g_dataOverride, false) ||
        !WriteTextAtomic(JoinPath(g_dataOverride, L"status.json"), json{{"latest", "UI test"}, {"changes", 1}}.dump()) ||
        !WriteTextAtomic(JoinPath(g_dataOverride, L"config.json"), json{{"language", "en"}, {"auto", "off"}, {"restartHour", -1}}.dump())) return 1;
    DeleteFileW(JoinPath(g_dataOverride, L"report.json").c_str());
    UINT_PTR timer = SetTimer(nullptr, 0, 500, TrayTestTimer);
    int result = RunTray(GetModuleHandleW(nullptr), L"Local\\LiberNTUpdateTrayTest");
    KillTimer(nullptr, timer);
    return result;
}

static int LaunchTrayTest(DWORD explorerPid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, explorerPid);
    if (!process) return 1;
    wchar_t path[MAX_PATH]{}, windows[MAX_PATH]{}, own[MAX_PATH]{};
    DWORD chars = _countof(path), session = 0;
    GetWindowsDirectoryW(windows, _countof(windows));
    bool expected = QueryFullProcessImageNameW(process, 0, path, &chars) &&
                    !_wcsicmp(path, JoinPath(windows, L"explorer.exe").c_str()) &&
                    ProcessIdToSessionId(explorerPid, &session) && session != 0;
    HANDLE source = nullptr, user = nullptr;
    bool copied = expected && OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, &source) &&
                  DuplicateTokenEx(source, TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY, nullptr,
                                   SecurityImpersonation, TokenPrimary, &user);
    if (source) CloseHandle(source);
    CloseHandle(process);
    if (!copied) { std::cout << "Cannot obtain the verified Explorer identity: " << GetLastError() << std::endl; return 1; }
    GetModuleFileNameW(nullptr, own, _countof(own));
    std::wstring command = L"\"" + std::wstring(own) + L"\" tray-test";
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = (LPWSTR)L"winsta0\\default";
    PROCESS_INFORMATION child{};
    bool started = CreateProcessAsUserW(user, own, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &child) != 0;
    DWORD error = started ? 0 : GetLastError();
    CloseHandle(user);
    if (!started) { std::cout << "Interactive tray test failed: " << error << std::endl; return 1; }
    std::cout << "PASS tray test pid=" << child.dwProcessId << " session=" << session << std::endl;
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    return 0;
}
