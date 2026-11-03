// Main window: current and offered release, the files that would change, progress and the update history.
// Tray icon (/tray): announces an update the service found or installed and offers the restart.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <memory>
#include <thread>

#include "engine.h"
#include "util.h"

namespace {

enum : int { IdCheck = 101, IdInstall, IdRollback, IdRestart, IdTabs, IdChanges, IdHistory, IdNotes };
enum : UINT { WmProgress = WM_APP + 1, WmDone };
enum class Job { Check, Install, Rollback };

struct DoneInfo {
    Job job;
    bool ok;
    InstallOutcome outcome;
    std::wstring message;
    std::unique_ptr<CheckResult> check;
};

HWND g_wnd, g_heading, g_detail, g_status, g_progress, g_tabs, g_changes, g_history, g_notes, g_btn[4], g_lblNotes;
HFONT g_font, g_bold, g_titleFont;
CheckResult g_check;
bool g_haveCheck = false, g_busy = false;
int g_page = 0;

void AddColumn(HWND lv, int i, const wchar_t* title, int width) {
    LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH};
    c.pszText = const_cast<LPWSTR>(title);
    c.cx = width;
    ListView_InsertColumn(lv, i, &c);
}

void SetCell(HWND lv, int row, int col, const std::wstring& text) {
    if (col == 0) {
        LVITEMW it{LVIF_TEXT};
        it.iItem = row;
        it.pszText = const_cast<LPWSTR>(text.c_str());
        ListView_InsertItem(lv, &it);
    } else {
        ListView_SetItemText(lv, row, col, const_cast<LPWSTR>(text.c_str()));
    }
}

std::wstring Size(uint64_t b) {
    wchar_t buf[32];
    if (b >= 1 << 20) swprintf(buf, 32, L"%.1f MB", b / 1048576.0);
    else swprintf(buf, 32, L"%.1f kB", b / 1024.0);
    return buf;
}

std::wstring ResultText(const std::string& r) {
    if (r.rfind("installed", 0) == 0) return Tr(L"nainstalováno", L"installed") + Widen(r.substr(9));
    if (r == "pending-restart") return Tr(L"čeká na restart", L"waits for a restart");
    if (r == "failed") return Tr(L"selhalo", L"failed");
    if (r == "running") return Tr(L"přerušeno", L"interrupted");
    return Widen(r);
}

std::wstring KindText(const std::string& k) {
    if (k == "install") return Tr(L"aktualizace", L"update");
    if (k == "rollback") return Tr(L"vrácení", L"undo");
    if (k == "auto-rollback") return Tr(L"automatické vrácení", L"automatic undo");
    return Widen(k);
}

std::wstring CompactDate(const std::string& value) {
    if (value.size() < 16) return Widen(value);
    return Widen(value.substr(0, 10) + " " + value.substr(11, 5));
}

std::wstring CompactRelease(const std::string& value) {
    if (value.size() >= 26 && value[8] == '-' && value[15] == '-')
        return Widen(value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2) + " " + value.substr(9, 2) + ":" +
                     value.substr(11, 2) + " ") +
               L"\x00B7 " + Widen(value.substr(16));
    return Widen(value);
}

void RefreshHistory() {
    ListView_DeleteAllItems(g_history);
    int row = 0;
    for (auto& h : LoadHistory()) {
        SetCell(g_history, row, 0, CompactDate(h.started));
        SetCell(g_history, row, 1, KindText(h.kind));
        SetCell(g_history, row, 2, CompactRelease(h.release));
        SetCell(g_history, row, 3, ResultText(h.result));
        SetCell(g_history, row, 4, std::to_wstring(h.files));
        SetCell(g_history, row, 5, Widen(h.message));
        row++;
    }
}

void RefreshInfo() {
    std::string inst = InstalledRelease(), pend = PendingTransaction();
    std::wstring heading;
    if (g_busy) heading = Tr(L"Pracuji na aktualizaci", L"Working on the update");
    else if (!pend.empty()) heading = Tr(L"Aktualizace čeká na restart", L"The update needs a restart");
    else if (!g_haveCheck) heading = Tr(L"Kontroluji aktualizace", L"Checking for updates");
    else if (g_check.changes.empty()) heading = Tr(L"Systém je aktuální", L"The system is up to date");
    else heading = Tr(L"Je k dispozici aktualizace", L"An update is available");
    SetWindowTextW(g_heading, heading.c_str());

    std::wstring detail = Tr(L"Nainstalováno: ", L"Installed: ") +
                          (inst.empty() ? std::wstring(Tr(L"zatím neevidováno", L"not recorded yet")) : CompactRelease(inst));
    if (g_haveCheck) {
        detail += Tr(L"\r\nNejnovější: ", L"\r\nNewest: ") + CompactRelease(g_check.latest.id);
        if (!g_check.changes.empty())
            detail += Tr(L"  ·  souborů ke změně: ", L"  ·  files to change: ") + std::to_wstring(g_check.changes.size());
    }
    SetWindowTextW(g_detail, detail.c_str());
    EnableWindow(g_btn[0], !g_busy);
    EnableWindow(g_btn[1], !g_busy && g_haveCheck && !g_check.changes.empty() && pend.empty());
    EnableWindow(g_btn[2], !g_busy && pend.empty());
    EnableWindow(g_btn[3], !g_busy && !pend.empty());
}

/// Keyboard focus on the first enabled button when nothing in the window has it (at start, after a job disabled the
/// buttons), so Tab, Enter and the Alt shortcuts work.
void FocusFirstButton() {
    HWND f = GetFocus();
    if (f && f != g_wnd && IsWindowEnabled(f)) return;
    for (HWND b : g_btn)
        if (IsWindowEnabled(b)) { SetFocus(b); return; }
}

void ShowCheck() {
    ListView_DeleteAllItems(g_changes);
    int row = 0;
    for (auto& c : g_check.changes) {
        SetCell(g_changes, row, 0, Widen(c.path));
        SetCell(g_changes, row, 1, Size(c.size));
        SetCell(g_changes, row, 2, c.newSha.empty() ? Tr(L"odstraněný", L"removed")
                                                    : c.oldSha.empty() ? Tr(L"nový", L"new") : Tr(L"změněný", L"changed"));
        row++;
    }
    if (!row) SetCell(g_changes, 0, 0, Tr(L"Žádné soubory není potřeba měnit.", L"No files need to be changed."));
    std::wstring notes;
    for (auto& n : g_check.latest.notes) notes += Widen(n) + L"\r\n";
    if (notes.empty()) notes = Tr(L"K tomuto vydání nejsou poznámky.", L"This release has no notes.");
    SetWindowTextW(g_notes, notes.c_str());
}

void Start(Job job) {
    if (g_busy) return;
    g_busy = true;
    RefreshInfo();
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    CheckResult snapshot = g_check;
    std::thread([job, snapshot]() {
        auto done = new DoneInfo{job, true, InstallOutcome::NothingToDo, L"", nullptr};
        Progress progress = [](int pct, const std::wstring& text) {
            PostMessageW(g_wnd, WmProgress, (WPARAM)pct, (LPARAM) new std::wstring(text));
        };
        Config cfg = LoadConfig();
        if (job == Job::Check) {
            done->check = std::make_unique<CheckResult>();
            done->ok = CheckForUpdate(cfg, *done->check, done->message, progress);
        } else if (job == Job::Install) {
            std::wstring err;
            if (!EnsureFinalizeService(err)) LogLine("service: " + Narrow(err));
            done->outcome = InstallUpdate(cfg, snapshot, done->message, progress);
            done->ok = done->outcome != InstallOutcome::Failed;
        } else {
            done->outcome = RollbackLast(cfg, done->message, progress);
            done->ok = done->outcome != InstallOutcome::Failed;
        }
        PostMessageW(g_wnd, WmDone, 0, (LPARAM)done);
    }).detach();
}

void ShowPage(int page) {
    g_page = page;
    ShowWindow(g_lblNotes, page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_notes, page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_changes, page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_history, page == 2 ? SW_SHOW : SW_HIDE);
}

void Layout(int w, int h) {
    const int m = 24, gap = 8, bh = 30;
    MoveWindow(g_heading, m, 18, w - 2 * m, 32, TRUE);
    MoveWindow(g_detail, m, 52, w - 2 * m, 42, TRUE);
    int actionY = 101;
    MoveWindow(g_btn[0], m, actionY, 118, bh, TRUE);
    MoveWindow(g_btn[2], m + 118 + gap, actionY, 166, bh, TRUE);
    MoveWindow(g_btn[3], w - m - 132, actionY, 132, bh, TRUE);
    MoveWindow(g_btn[1], w - m - 132 - gap - 150, actionY, 150, bh, TRUE);
    MoveWindow(g_progress, m, 143, w - 2 * m, 8, TRUE);
    MoveWindow(g_status, m, 157, w - 2 * m, 20, TRUE);
    int tabTop = 184;
    MoveWindow(g_tabs, m, tabTop, w - 2 * m, h - tabTop - m, TRUE);
    int x = m + 12, y = tabTop + 36, cw = w - 2 * (m + 12), ch = h - y - m - 12;
    MoveWindow(g_lblNotes, x, y, cw, 20, TRUE);
    MoveWindow(g_notes, x, y + 24, cw, std::max(40, ch - 24), TRUE);
    MoveWindow(g_changes, x, y, cw, ch, TRUE);
    MoveWindow(g_history, x, y, cw, ch, TRUE);
    ListView_SetColumnWidth(g_changes, 0, std::max(220, cw - 190));
    ListView_SetColumnWidth(g_changes, 1, 85);
    ListView_SetColumnWidth(g_changes, 2, 95);
    int message = std::max(130, cw - 531);
    ListView_SetColumnWidth(g_history, 0, 116);
    ListView_SetColumnWidth(g_history, 1, 90);
    ListView_SetColumnWidth(g_history, 2, 170);
    ListView_SetColumnWidth(g_history, 3, 95);
    ListView_SetColumnWidth(g_history, 4, 60);
    ListView_SetColumnWidth(g_history, 5, message);
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_wnd = w;
            g_heading = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
            SendMessageW(g_heading, WM_SETFONT, (WPARAM)g_titleFont, TRUE);
            g_detail = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
            const wchar_t* labels[4] = {Tr(L"&Zkontrolovat znovu", L"&Check again"), Tr(L"&Nainstalovat", L"&Install update"),
                                        Tr(L"&Vrátit poslední", L"&Undo last update"), Tr(L"&Restartovat", L"&Restart")};
            for (int i : {0, 2, 1, 3})
                g_btn[i] = Make(L"BUTTON", labels[i], (i == 1 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON) | WS_TABSTOP, IdCheck + i);
            g_progress = Make(PROGRESS_CLASSW, L"", 0, 0);
            SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            g_status = Make(L"STATIC", Tr(L"Připraveno.", L"Ready."), SS_LEFT | SS_ENDELLIPSIS, 0);
            g_tabs = Make(WC_TABCONTROLW, L"", WS_TABSTOP, IdTabs);
            TCITEMW tab{TCIF_TEXT};
            for (const wchar_t* label : {Tr(L"Přehled", L"Overview"), Tr(L"Změny", L"Changes"), Tr(L"Historie", L"History")}) {
                tab.pszText = const_cast<LPWSTR>(label);
                TabCtrl_InsertItem(g_tabs, TabCtrl_GetItemCount(g_tabs), &tab);
            }
            g_changes = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, IdChanges);
            ListView_SetExtendedListViewStyle(g_changes, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            AddColumn(g_changes, 0, Tr(L"Soubor", L"File"), 480);
            AddColumn(g_changes, 1, Tr(L"Velikost", L"Size"), 90);
            AddColumn(g_changes, 2, Tr(L"Typ", L"Kind"), 90);
            g_history = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, IdHistory);
            ListView_SetExtendedListViewStyle(g_history, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            AddColumn(g_history, 0, Tr(L"Začátek (UTC)", L"Started (UTC)"), 140);
            AddColumn(g_history, 1, Tr(L"Akce", L"Action"), 120);
            AddColumn(g_history, 2, Tr(L"Vydání", L"Release"), 190);
            AddColumn(g_history, 3, Tr(L"Výsledek", L"Result"), 120);
            AddColumn(g_history, 4, Tr(L"Počet", L"Files"), 60);
            AddColumn(g_history, 5, Tr(L"Zpráva", L"Message"), 400);
            g_lblNotes = Make(L"STATIC", Tr(L"Poznámky k nejnovějšímu vydání", L"Notes for the newest release"), SS_LEFT, 0);
            SendMessageW(g_lblNotes, WM_SETFONT, (WPARAM)g_bold, TRUE);
            g_notes = Make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, IdNotes);
            ShowPage(0);
            RefreshHistory();
            RefreshInfo();
            std::string pend = PendingTransaction();
            if (!pend.empty()) SetWindowTextW(g_status, Tr(L"Aktualizace čeká na restart; po něm ji služba ověří.", L"An update waits for a restart; the service verifies it afterwards."));
            Start(Job::Check);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) FocusFirstButton();
            return 0;
        case WM_SIZE:
            Layout(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)lp)->ptMinTrackSize = {720, 520};
            return 0;
        case WM_NOTIFY:
            if (((NMHDR*)lp)->idFrom == IdTabs && ((NMHDR*)lp)->code == TCN_SELCHANGE) {
                ShowPage(TabCtrl_GetCurSel(g_tabs));
                RECT r;
                GetClientRect(w, &r);
                Layout(r.right, r.bottom);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IdCheck: Start(Job::Check); break;
                case IdInstall:
                    if (MessageBoxW(w, (Tr(L"Nainstalovat vydání ", L"Install release ") + Widen(g_check.latest.id) +
                                     Tr(L"?\r\n\r\nPůvodní soubory se zálohují; když se něco nepovede, vrátí se automaticky.",
                                        L"?\r\n\r\nThe current files are backed up; if anything fails they come back automatically."))
                                        .c_str(),
                                    L"LiberNT Update", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                        Start(Job::Install);
                    break;
                case IdRollback:
                    if (MessageBoxW(w, Tr(L"Vrátit poslední nainstalovanou aktualizaci?", L"Undo the last installed update?"), L"LiberNT Update", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                        Start(Job::Rollback);
                    break;
                case IdRestart: {
                    std::wstring err;
                    if (MessageBoxW(w, Tr(L"Restartovat počítač nyní?", L"Restart the computer now?"), L"LiberNT Update", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES && !RestartSystem(err))
                        MessageBoxW(w, err.c_str(), L"LiberNT Update", MB_ICONERROR);
                    break;
                }
            }
            return 0;
        case WmProgress: {
            std::unique_ptr<std::wstring> text((std::wstring*)lp);
            if ((int)wp >= 0) SendMessageW(g_progress, PBM_SETPOS, wp, 0);
            SetWindowTextW(g_status, text->c_str());
            return 0;
        }
        case WmDone: {
            std::unique_ptr<DoneInfo> d((DoneInfo*)lp);
            g_busy = false;
            if (d->job == Job::Check && d->ok) {
                g_check = *d->check;
                g_haveCheck = true;
                ShowCheck();
                SetWindowTextW(g_status, (g_check.changes.empty()
                                              ? std::wstring(Tr(L"Systém odpovídá nejnovějšímu vydání.", L"The system matches the newest release."))
                                              : Tr(L"Souborů k aktualizaci: ", L"Files to update: ") + std::to_wstring(g_check.changes.size()))
                                             .c_str());
            } else {
                SetWindowTextW(g_status, d->message.c_str());
            }
            if (!d->ok) MessageBoxW(w, d->message.c_str(), L"LiberNT Update", MB_ICONERROR);
            if (d->outcome == InstallOutcome::PendingRestart &&
                MessageBoxW(w, (d->message + Tr(L"\r\n\r\nRestartovat nyní?", L"\r\n\r\nRestart now?")).c_str(), L"LiberNT Update", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                std::wstring err;
                if (!RestartSystem(err)) MessageBoxW(w, err.c_str(), L"LiberNT Update", MB_ICONERROR);
            }
            if (d->job != Job::Check && d->outcome == InstallOutcome::Installed) Start(Job::Check);
            RefreshHistory();
            RefreshInfo();
            FocusFirstButton();
            SendMessageW(g_progress, PBM_SETPOS, d->ok ? 100 : 0, 0);
            return 0;
        }
        case WM_CLOSE:
            if (g_busy) {
                MessageBoxW(w, Tr(L"Probíhající operaci je potřeba nechat dokončit.", L"Let the current operation finish before closing the window."),
                            L"LiberNT Update", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int RunGui(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);
    LOGFONTW title = bold;
    title.lfHeight = -22;
    g_titleFont = CreateFontIndirectW(&title);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"LiberNTUpdateWindow";
    RegisterClassExW(&wc);
    std::wstring windowTitle = L"LiberNT Update " + Widen(ClientVersion());
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int ww = 840, wh = 620;
    int x = work.left + std::max(0L, (work.right - work.left - ww) / 2);
    int y = work.top + std::max(0L, (work.bottom - work.top - wh) / 2);
    HWND w = CreateWindowExW(0, wc.lpszClassName, windowTitle.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, ww, wh,
                             nullptr, nullptr, inst, nullptr);
    ShowWindow(w, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        HWND focused = GetFocus();
        int id = focused ? GetDlgCtrlID(focused) : 0;
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && id >= IdCheck && id <= IdRestart && IsWindowEnabled(focused)) {
            SendMessageW(focused, BM_CLICK, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}

namespace {

enum : UINT { WmTray = WM_APP + 10 };
enum : int { IdOpen = 201, IdRestartNow, IdHide };
UINT g_taskbarCreated;
NOTIFYICONDATAW g_nid;
bool g_iconShown = false;
std::string g_announced, g_hiddenFor;  // what the last balloon was about; what the user hid the icon for

void OpenMainWindow() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOWNORMAL);
}

/// Shows, updates or removes the icon according to the pending update and the last automatic check.
void RefreshTray(HWND w) {
    std::string pending = PendingTransaction();
    Status s = LoadStatus();
    std::string about = !pending.empty() ? "pending:" + pending : s.changes > 0 ? "available:" + s.latest : "";
    if (about.empty() || about == g_hiddenFor) {
        if (g_iconShown) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_iconShown = false;
        return;
    }
    Config cfg = LoadConfig();
    std::wstring tip, title, text;
    if (!pending.empty()) {
        tip = Tr(L"LiberNT Update: aktualizace čeká na restart", L"LiberNT Update: an update waits for a restart");
        title = Tr(L"Aktualizace je připravená", L"The update is ready");
        text = Tr(L"Dokončí se při příštím restartu", L"It finishes at the next restart");
        text += cfg.restartHour >= 0 ? Tr(L" (automaticky v ", L" (automatically at ") + std::to_wstring(cfg.restartHour) + L":00)." : std::wstring(L".");
    } else {
        tip = Tr(L"LiberNT Update: k dispozici je ", L"LiberNT Update: available ") + Widen(s.latest);
        title = Tr(L"Je k dispozici aktualizace LiberNT", L"A LiberNT update is available");
        text = Tr(L"Vydání ", L"Release ") + Widen(s.latest) + Tr(L", souborů ke změně: ", L", files to change: ") + std::to_wstring(s.changes) +
               Tr(L". Klepnutím otevřete LiberNT Update.", L". Click to open LiberNT Update.");
    }
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    if (about != g_announced) {
        g_nid.uFlags |= NIF_INFO;
        wcsncpy_s(g_nid.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
        g_nid.dwInfoFlags = NIIF_INFO;
        g_announced = about;
    }
    if (!Shell_NotifyIconW(g_iconShown ? NIM_MODIFY : NIM_ADD, &g_nid)) {
        LogLine("tray: Shell_NotifyIcon failed: " + Narrow(LastErrorText(GetLastError())));
        return;
    }
    if (!g_iconShown) LogLine("tray: icon shown for " + about);
    g_iconShown = true;
}

LRESULT CALLBACK TrayProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated) {  // Explorer restarted: the icon has to be added again.
        g_iconShown = false;
        g_announced.clear();
        RefreshTray(w);
        return 0;
    }
    switch (msg) {
        case WM_TIMER:
            RefreshTray(w);
            return 0;
        case WmTray:
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK || lp == NIN_BALLOONUSERCLICK) {
                OpenMainWindow();
            } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, IdOpen, Tr(L"Otevřít LiberNT Update", L"Open LiberNT Update"));
                if (!PendingTransaction().empty()) AppendMenuW(m, MF_STRING, IdRestartNow, Tr(L"Restartovat a dokončit aktualizaci", L"Restart and finish the update"));
                AppendMenuW(m, MF_STRING, IdHide, Tr(L"Skrýt ikonu", L"Hide the icon"));
                POINT p;
                GetCursorPos(&p);
                SetForegroundWindow(w);
                TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, w, nullptr);
                DestroyMenu(m);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IdOpen) {
                OpenMainWindow();
            } else if (LOWORD(wp) == IdRestartNow) {
                std::wstring err;
                if (!RestartSystem(err)) MessageBoxW(nullptr, err.c_str(), L"LiberNT Update", MB_ICONERROR);
            } else if (LOWORD(wp) == IdHide) {
                g_hiddenFor = g_announced;
                RefreshTray(w);
            }
            return 0;
        case WM_DESTROY:
            if (g_iconShown) Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int RunTray(HINSTANCE inst) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LiberNTUpdateTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"LiberNTUpdateTray";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"LiberNT Update", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    // The V2 size works with shells that predate the Vista fields of NOTIFYICONDATAW.
    g_nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    g_nid.hWnd = w;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WmTray;
    g_nid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RefreshTray(w);
    SetTimer(w, 1, 30 * 1000, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CloseHandle(single);
    return 0;
}
