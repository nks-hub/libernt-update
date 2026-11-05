#pragma once

#include <string>

// Self-test only: use a temporary signing key instead of the production feed key.
void SetFeedTestPublicKey(const std::string& publicKeyHex);
bool SafeManifestPath(const std::string& path);
