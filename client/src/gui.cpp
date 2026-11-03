// Main window: current and offered release, the files that would change, progress and the update history.
#include <windows.h>
#include <commctrl.h>

#include <memory>
#include <thread>

#include "engine.h"
#include "util.h"

namespace {

enum : int { IdCheck = 101, IdInstall, IdRollback, IdRestart, IdChanges, IdHistory, IdNotes };
enum : UINT { WmProgress = WM_APP + 1, WmDone };
enum class Job { Check, Install, Rollback };

struct DoneInfo {
    Job job;
    bool ok;
    InstallOutcome outcome;
    std::wstring message;
    std::unique_ptr<CheckResult> check;
};

HWND g_wnd, g_info, g_status, g_progress, g_changes, g_history, g_notes, g_btn[4], g_lblChanges, g_lblHistory, g_lblNotes;
HFONT g_font, g_bold;
CheckResult g_check;
bool g_haveCheck = false, g_busy = false;

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
    if (r.rfind("installed", 0) == 0) return L"nainstalováno" + Widen(r.substr(9));
    if (r == "pending-restart") return L"čeká na restart";
    if (r == "failed") return L"selhalo";
    if (r == "running") return L"přerušeno";
    return Widen(r);
}

std::wstring KindText(const std::string& k) {
    if (k == "install") return L"aktualizace";
    if (k == "rollback") return L"vrácení";
    if (k == "auto-rollback") return L"automatické vrácení";
    return Widen(k);
}

void RefreshHistory() {
    ListView_DeleteAllItems(g_history);
    int row = 0;
    for (auto& h : LoadHistory()) {
        SetCell(g_history, row, 0, Widen(h.started));
        SetCell(g_history, row, 1, KindText(h.kind));
        SetCell(g_history, row, 2, Widen(h.release));
        SetCell(g_history, row, 3, ResultText(h.result));
        SetCell(g_history, row, 4, std::to_wstring(h.files));
        SetCell(g_history, row, 5, Widen(h.message));
        row++;
    }
}

void RefreshInfo() {
    std::string inst = InstalledRelease(), pend = PendingTransaction();
    std::wstring t = L"Nainstalované vydání: " + (inst.empty() ? std::wstring(L"zatím neevidováno") : Widen(inst));
    if (g_haveCheck) {
        t += L"\r\nNejnovější vydání: " + Widen(g_check.latest.id) + L" (commit " + Widen(g_check.latest.commit.substr(0, 10)) + L", " +
             Widen(g_check.latest.date) + L")";
        t += g_check.changes.empty() ? L"\r\nSystém je aktuální." : L"\r\nK aktualizaci: " + std::to_wstring(g_check.changes.size()) + L" souborů";
    }
    if (!pend.empty()) t += L"\r\nAktualizace " + Widen(pend) + L" čeká na restart.";
    SetWindowTextW(g_info, t.c_str());
    EnableWindow(g_btn[0], !g_busy);
    EnableWindow(g_btn[1], !g_busy && g_haveCheck && !g_check.changes.empty() && pend.empty());
    EnableWindow(g_btn[2], !g_busy && pend.empty());
    EnableWindow(g_btn[3], !g_busy);
}

void ShowCheck() {
    ListView_DeleteAllItems(g_changes);
    int row = 0;
    for (auto& c : g_check.changes) {
        SetCell(g_changes, row, 0, Widen(c.path));
        SetCell(g_changes, row, 1, Size(c.size));
        SetCell(g_changes, row, 2, c.oldSha.empty() ? L"nový" : L"změněný");
        row++;
    }
    std::wstring notes;
    for (auto& n : g_check.latest.notes) notes += Widen(n) + L"\r\n";
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

void Layout(int w, int h) {
    const int m = 10, bw = 150, bh = 28;
    MoveWindow(g_info, m, m, w - 2 * m, 58, TRUE);
    for (int i = 0; i < 4; i++) MoveWindow(g_btn[i], m + i * (bw + 8), 74, bw, bh, TRUE);
    MoveWindow(g_progress, m, 110, w - 2 * m, 16, TRUE);
    MoveWindow(g_status, m, 130, w - 2 * m, 20, TRUE);
    int top = 156, rest = h - top - m;
    int changesH = rest * 40 / 100, historyH = rest * 35 / 100, notesH = rest - changesH - historyH - 3 * 20;
    MoveWindow(g_lblChanges, m, top, w - 2 * m, 18, TRUE);
    MoveWindow(g_changes, m, top + 20, w - 2 * m, changesH, TRUE);
    top += 20 + changesH;
    MoveWindow(g_lblHistory, m, top + 2, w - 2 * m, 18, TRUE);
    MoveWindow(g_history, m, top + 22, w - 2 * m, historyH, TRUE);
    top += 22 + historyH;
    MoveWindow(g_lblNotes, m, top + 2, w - 2 * m, 18, TRUE);
    MoveWindow(g_notes, m, top + 22, w - 2 * m, std::max(40, notesH), TRUE);
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
            g_info = Make(L"STATIC", L"", SS_LEFT, 0);
            SendMessageW(g_info, WM_SETFONT, (WPARAM)g_bold, TRUE);
            const wchar_t* labels[4] = {L"Zkontrolovat", L"Nainstalovat", L"Vrátit poslední", L"Restartovat"};
            for (int i = 0; i < 4; i++) g_btn[i] = Make(L"BUTTON", labels[i], BS_PUSHBUTTON | WS_TABSTOP, IdCheck + i);
            g_progress = Make(PROGRESS_CLASSW, L"", 0, 0);
            SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            g_status = Make(L"STATIC", L"Připraveno.", SS_LEFT | SS_ENDELLIPSIS, 0);
            g_lblChanges = Make(L"STATIC", L"Soubory, které aktualizace změní", SS_LEFT, 0);
            g_changes = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER, IdChanges);
            ListView_SetExtendedListViewStyle(g_changes, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            AddColumn(g_changes, 0, L"Soubor", 480);
            AddColumn(g_changes, 1, L"Velikost", 90);
            AddColumn(g_changes, 2, L"Typ", 90);
            g_lblHistory = Make(L"STATIC", L"Historie aktualizací", SS_LEFT, 0);
            g_history = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER, IdHistory);
            ListView_SetExtendedListViewStyle(g_history, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            AddColumn(g_history, 0, L"Začátek (UTC)", 140);
            AddColumn(g_history, 1, L"Akce", 120);
            AddColumn(g_history, 2, L"Vydání", 190);
            AddColumn(g_history, 3, L"Výsledek", 120);
            AddColumn(g_history, 4, L"Souborů", 60);
            AddColumn(g_history, 5, L"Zpráva", 400);
            g_lblNotes = Make(L"STATIC", L"Poznámky k vydání (změny ve zdrojích)", SS_LEFT, 0);
            g_notes = Make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, IdNotes);
            RefreshHistory();
            RefreshInfo();
            std::string pend = PendingTransaction();
            if (!pend.empty()) SetWindowTextW(g_status, L"Aktualizace čeká na restart; po něm ji služba ověří.");
            Start(Job::Check);
            return 0;
        }
        case WM_SIZE:
            Layout(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)lp)->ptMinTrackSize = {720, 560};
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IdCheck: Start(Job::Check); break;
                case IdInstall:
                    if (MessageBoxW(w, (L"Nainstalovat vydání " + Widen(g_check.latest.id) + L"?\r\n\r\nPůvodní soubory se zálohují; když se něco "
                                         L"nepovede, vrátí se automaticky.").c_str(),
                                    L"LiberNT Update", MB_YESNO | MB_ICONQUESTION) == IDYES)
                        Start(Job::Install);
                    break;
                case IdRollback:
                    if (MessageBoxW(w, L"Vrátit poslední nainstalovanou aktualizaci?", L"LiberNT Update", MB_YESNO | MB_ICONQUESTION) == IDYES)
                        Start(Job::Rollback);
                    break;
                case IdRestart: {
                    std::wstring err;
                    if (MessageBoxW(w, L"Restartovat počítač nyní?", L"LiberNT Update", MB_YESNO | MB_ICONQUESTION) == IDYES && !RestartSystem(err))
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
                SetWindowTextW(g_status, (g_check.changes.empty() ? L"Systém odpovídá nejnovějšímu vydání."
                                                                  : std::to_wstring(g_check.changes.size()) + L" souborů k aktualizaci.").c_str());
            } else {
                SetWindowTextW(g_status, d->message.c_str());
            }
            if (!d->ok) MessageBoxW(w, d->message.c_str(), L"LiberNT Update", MB_ICONERROR);
            if (d->outcome == InstallOutcome::PendingRestart &&
                MessageBoxW(w, (d->message + L"\r\n\r\nRestartovat nyní?").c_str(), L"LiberNT Update", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::wstring err;
                if (!RestartSystem(err)) MessageBoxW(w, err.c_str(), L"LiberNT Update", MB_ICONERROR);
            }
            if (d->job != Job::Check && d->outcome == InstallOutcome::Installed) Start(Job::Check);
            RefreshHistory();
            RefreshInfo();
            SendMessageW(g_progress, PBM_SETPOS, 100, 0);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int RunGui(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"LiberNTUpdateWindow";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"LiberNT Update", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 680,
                             nullptr, nullptr, inst, nullptr);
    ShowWindow(w, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
