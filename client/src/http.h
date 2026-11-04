#pragma once
#include <cstdint>
#include <string>
#include <vector>

/// Downloads url into out through WinINet; on failure returns false and sets error.
bool HttpGet(const std::wstring& url, std::vector<uint8_t>& out, std::wstring& error,
             size_t maxBytes = 16u * 1024u * 1024u, unsigned* statusCode = nullptr);
