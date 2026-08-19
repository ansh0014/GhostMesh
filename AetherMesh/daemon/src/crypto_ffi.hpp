#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
int aether_encrypt(
    const std::uint8_t *key,
    const std::uint8_t *plaintext,
    std::size_t len,
    std::uint8_t *out_nonce,
    std::uint8_t *out_tag,
    std::uint8_t *out_ciphertext);

int aether_decrypt(
    const std::uint8_t *key,
    const std::uint8_t *nonce,
    const std::uint8_t *tag,
    const std::uint8_t *ciphertext,
    std::size_t len,
    std::uint8_t *out_plaintext);

void aether_secure_zero(void *ptr, std::size_t len);
}
