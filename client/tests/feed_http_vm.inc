#include "../src/http.h"
#include <atomic>
#include <mutex>

class FeedHttpServer {
    SOCKET listener_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::map<std::string, std::pair<unsigned, std::string>> replies_;
    std::map<std::string, unsigned> hits_;
    unsigned port_ = 0;

    void Serve() {
        while (!stop_) {
            fd_set ready;
            FD_ZERO(&ready); FD_SET(listener_, &ready);
            timeval timeout{0, 200000};
            if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD milliseconds = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&milliseconds, sizeof(milliseconds));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&milliseconds, sizeof(milliseconds));
            std::string request;
            char buffer[1024];
            while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
                int count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request.append(buffer, count);
            }
            auto first = request.find(' '), last = request.find(' ', first == std::string::npos ? 0 : first + 1);
            std::string path = first != std::string::npos && last != std::string::npos ? request.substr(first + 1, last - first - 1) : "";
            std::pair<unsigned, std::string> reply{404, "missing"};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++hits_[path];
                auto found = replies_.find(path);
                if (found != replies_.end()) reply = found->second;
            }
            std::string response = "HTTP/1.1 " + std::to_string(reply.first) + " Test\r\nContent-Type: application/json\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(reply.second.size()) + "\r\n\r\n" + reply.second;
            size_t sent = 0;
            while (sent < response.size()) {
                int count = send(client, response.data() + sent, (int)(response.size() - sent), 0);
                if (count <= 0) break;
                sent += count;
            }
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }

public:
    ~FeedHttpServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
    }
    bool Start() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, (sockaddr*)&address, sizeof(address)) || listen(listener_, 4)) return false;
        int size = sizeof(address);
        if (getsockname(listener_, (sockaddr*)&address, &size)) return false;
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this]() { Serve(); });
        return true;
    }
    void Reply(const std::string& path, unsigned status, const std::string& body) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[path] = {status, body};
    }
    unsigned Hits(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        return hits_[path];
    }
    std::wstring Url() const { return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/"; }
};

static int RunFeedHttpTest() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return 1;
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(_countof(temporary), temporary)) { WSACleanup(); return 1; }
    const auto base = JoinPath(temporary, L"lntu-http-" + Widen(NowId()) + L"-" + std::to_wstring(GetCurrentProcessId()));
    const auto previousData = g_dataOverride;
    g_dataOverride = JoinPath(base, L"data");
    bool ok = true;
    unsigned passed = 0;
    auto check = [&](bool condition, const char* name) {
        std::cout << (condition ? "PASS " : "FAIL ") << name << std::endl;
        if (condition) ++passed;
        ok = ok && condition;
    };
    auto hex = [](const auto& bytes) {
        std::string text;
        for (auto value : bytes) { text += "0123456789abcdef"[value >> 4]; text += "0123456789abcdef"[value & 15]; }
        return text;
    };
    std::vector<uint8_t> seed, secret(64), publicKey(32), signature(64);
    HexToBytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
    crypto_ed25519_key_pair(secret.data(), publicKey.data(), seed.data());
    SetFeedTestPublicKey(hex(publicKey));
    auto signedReply = [&](FeedHttpServer& server, const std::string& name, const std::string& body) {
        crypto_ed25519_sign(signature.data(), secret.data(), (const uint8_t*)body.data(), body.size());
        server.Reply("/" + name, 200, body);
        server.Reply("/" + name + ".sig", 200, hex(signature));
    };
    json legacyManifest{{"id", "HTTP1"}, {"files", json::array()}};
    auto registryManifest = legacyManifest;
    registryManifest["registry"] = {{"format", 1}, {"size", 1u}, {"sha256", std::string(64, 'a')}};
    auto feed = [&](unsigned format, const json& manifest) {
        auto body = manifest.dump();
        json release{{"id", "HTTP1"}, {"sequence", 1u}, {"manifest", "manifest.json"}, {"manifestSha256", Sha256Hex(body.data(), body.size())}};
        if (format == 2) release["registrySha256"] = manifest["registry"]["sha256"];
        return json{{"format", format}, {"releases", json::array({release})}}.dump();
    };
    auto state = [&]() { return SaveState(json{{"installed", "HTTP1"}, {"pending", ""}}); };
    {
        FeedHttpServer server;
        if (!server.Start()) { std::cout << "FAIL start loopback HTTP fixture" << std::endl; ok = false; }
        else {
            Config cfg{server.Url(), JoinPath(base, L"root")};
            cfg.registryBaseRelease = "HTTP1";
            signedReply(server, "feed.json", feed(1, legacyManifest));
            server.Reply("/manifest.json", 200, legacyManifest.dump());
            CheckResult result;
            std::wstring error;
            check(state() && CheckForUpdate(cfg, result, error, nullptr) && result.latest.id == "HTTP1" && server.Hits("/feed.json") == 1,
                  "HTTP 404 permits a signed legacy feed before v2 adoption");
            unsigned legacyHits = server.Hits("/feed.json");
            for (unsigned status : {403u, 500u}) {
                server.Reply("/feed-v2.json", status, "server error");
                check(state() && !CheckForUpdate(cfg, result, error, nullptr) && error.find(L"HTTP " + std::to_wstring(status)) != std::wstring::npos &&
                          server.Hits("/feed.json") == legacyHits,
                      status == 403 ? "HTTP 403 never falls back to legacy" : "HTTP 500 never falls back to legacy");
            }
            auto registryFeed = feed(2, registryManifest);
            server.Reply("/feed-v2.json", 200, registryFeed);
            check(state() && !CheckForUpdate(cfg, result, error, nullptr) && server.Hits("/feed.json") == legacyHits &&
                      server.Hits("/feed-v2.json.sig") == 1, "missing v2 signature never falls back to legacy");
            signedReply(server, "feed-v2.json", registryFeed);
            server.Reply("/feed-v2.json", 200, registryFeed + " ");
            check(state() && !CheckForUpdate(cfg, result, error, nullptr) && server.Hits("/feed.json") == legacyHits &&
                      LoadState().value("feedProtocol", 1) == 1, "damaged v2 signature rejects the feed without accepting v2");
            signedReply(server, "feed-v2.json", "{");
            check(state() && !CheckForUpdate(cfg, result, error, nullptr) && server.Hits("/feed.json") == legacyHits,
                  "signed malformed v2 JSON never falls back to legacy");
            signedReply(server, "feed-v2.json", registryFeed);
            server.Reply("/manifest.json", 200, registryManifest.dump());
            check(state() && CheckForUpdate(cfg, result, error, nullptr) && result.registryBefore == "HTTP1" &&
                      result.registryAfter == "HTTP1" && LoadState().value("feedProtocol", 1) == 2 && server.Hits("/feed.json") == legacyHits,
                  "valid HTTP v2 feed records protocol adoption");
            server.Reply("/feed-v2.json", 404, "missing");
            check(!CheckForUpdate(cfg, result, error, nullptr) && server.Hits("/feed.json") == legacyHits,
                  "HTTP 404 after v2 adoption refuses a legacy downgrade");
            server.Reply("/large", 200, "12345");
            std::vector<uint8_t> bytes;
            unsigned status = 0;
            check(!HttpGet(server.Url() + L"large", bytes, error, 4, &status) && status == 200 && bytes.empty(),
                  "HTTP response exceeding its byte limit is rejected");
        }
    }
    SetFeedTestPublicKey("");
    g_dataOverride = previousData;
    WSACleanup();
    if (PlainTree(base)) {
        std::wstring from = base + L'\0';
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE; operation.pFrom = from.c_str(); operation.fFlags = FOF_NO_UI;
        if (SHFileOperationW(&operation)) ok = false;
    } else ok = false;
    std::cout << (ok ? "RESULT PASS " : "RESULT FAIL ") << passed << " HTTP checks" << std::endl;
    return ok ? 0 : 1;
}
