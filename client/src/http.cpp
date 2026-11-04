#include "http.h"

#include <windows.h>
#include <wininet.h>

#include "util.h"

bool HttpGet(const std::wstring& url, std::vector<uint8_t>& out, std::wstring& error, size_t maxBytes) {
    out.clear();
    HINTERNET net = InternetOpenW(L"LiberNTUpdate/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) { error = L"InternetOpen: " + LastErrorText(GetLastError()); return false; }
    DWORD timeout = 30000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
    HINTERNET req = InternetOpenUrlW(net, url.c_str(), nullptr, 0,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE |
                                         INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI,
                                     0);
    if (!req) {
        error = url + L": " + LastErrorText(GetLastError());
        InternetCloseHandle(net);
        return false;
    }
    DWORD status = 0, len = sizeof status;
    if (HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr) && status != 200) {
        error = url + L": HTTP " + std::to_wstring(status);
        InternetCloseHandle(req);
        InternetCloseHandle(net);
        return false;
    }
    bool ok = true;
    uint8_t buf[65536];
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(req, buf, sizeof buf, &got)) {
            error = url + L": " + LastErrorText(GetLastError());
            ok = false;
            break;
        }
        if (!got) break;
        if (out.size() > maxBytes || got > maxBytes - out.size()) {
            error = L"HTTP response exceeds the download limit.";
            ok = false;
            out.clear();
            break;
        }
        out.insert(out.end(), buf, buf + got);
    }
    InternetCloseHandle(req);
    InternetCloseHandle(net);
    return ok;
}
