#include <windows.h>
#include <string>

static SERVICE_STATUS_HANDLE statusHandle;
static HANDLE stopped;
static SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS};
static std::wstring serviceName;

static DWORD WINAPI Control(DWORD code, DWORD, void*, void*) {
    if (code == SERVICE_CONTROL_STOP) {
        status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(statusHandle, &status);
        SetEvent(stopped);
    }
    return NO_ERROR;
}

static void WINAPI ServiceMain(DWORD, wchar_t**) {
    stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    statusHandle = RegisterServiceCtrlHandlerExW(serviceName.c_str(), Control, nullptr);
    status.dwCurrentState = SERVICE_START_PENDING;
    status.dwWaitHint = 10000;
    int delay = serviceName == L"LntuReviewHung" ? 45 : 5;
    for (int i = 0; i < delay; i++) {
        status.dwCheckPoint++;
        SetServiceStatus(statusHandle, &status);
        Sleep(1000);
    }
    if (serviceName != L"LntuReviewHung") {
        status.dwCurrentState = SERVICE_RUNNING;
        status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
        SetServiceStatus(statusHandle, &status);
        WaitForSingleObject(stopped, INFINITE);
    }
    status.dwCurrentState = SERVICE_STOPPED;
    status.dwControlsAccepted = 0;
    SetServiceStatus(statusHandle, &status);
    CloseHandle(stopped);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    serviceName = argv[1];
    if (serviceName != L"LntuReviewSlow" && serviceName != L"LntuReviewHung") return 2;
    SERVICE_TABLE_ENTRYW table[] = {{&serviceName[0], ServiceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
