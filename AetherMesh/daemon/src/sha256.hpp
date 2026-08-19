#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aether::hash {

using Digest = std::array<std::uint8_t, 32>;

Digest sha256(const std::uint8_t *data, std::size_t len);
Digest hmac_sha256(const std::uint8_t *key, std::size_t key_len, const std::uint8_t *data, std::size_t data_len);
Digest sha256_string(const std::string &text);
std::string hex_encode(const std::uint8_t *data, std::size_t len);
template <std::size_t N>
inline std::string hex_encode(const std::array<std::uint8_t, N> &data) {
	return hex_encode(data.data(), N);
}

template <std::size_t N>
inline Digest hmac_sha256(const std::array<std::uint8_t, N> &key, const std::vector<std::uint8_t> &data) {
	return hmac_sha256(key.data(), key.size(), data.data(), data.size());
}

std::array<std::uint8_t, 16> first16(const Digest &digest);

} // namespace aether::hash
