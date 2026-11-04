#pragma once
#include <windows.h>
#include <string>

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayPinCommand = 204;
bool ReadTrayPin(bool& pinned, std::wstring& error);
bool SetTrayPin(bool pinned, std::wstring& error);
int RunTray(HINSTANCE instance, const wchar_t* mutexName = L"Local\\LiberNTUpdateTray");
