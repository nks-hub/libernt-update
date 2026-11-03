#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/// Incremental SHA-256 (FIPS 180-4); kept local so hashing does not depend on the OS crypto stack.
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    /// Finishes the hash and returns it as 64 lowercase hex characters.
    std::string hex();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t total_ = 0;
    size_t used_ = 0;
};

std::string Sha256Hex(const void* data, size_t len);
