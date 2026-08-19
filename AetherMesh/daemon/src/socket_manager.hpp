#pragma once

#include "crypto_ffi.hpp"
#include "ipc.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace aether::net {

struct PeerEndpoint {
	std::string host;
	std::uint16_t port = 0;
};

class CryptoBridge {
public:
	using EncryptFn = int (*)(const std::uint8_t *, const std::uint8_t *, std::size_t, std::uint8_t *, std::uint8_t *, std::uint8_t *);
	using DecryptFn = int (*)(const std::uint8_t *, const std::uint8_t *, const std::uint8_t *, const std::uint8_t *, std::size_t, std::uint8_t *);
	using SecureZeroFn = void (*)(void *, std::size_t);

	bool load(const std::vector<std::string> &candidate_paths);
	bool loaded() const;
	int encrypt(const std::uint8_t *key, const std::uint8_t *plaintext, std::size_t len, std::uint8_t *out_nonce, std::uint8_t *out_tag, std::uint8_t *out_ciphertext) const;
	int decrypt(const std::uint8_t *key, const std::uint8_t *nonce, const std::uint8_t *tag, const std::uint8_t *ciphertext, std::size_t len, std::uint8_t *out_plaintext) const;
	void secureZero(void *ptr, std::size_t len) const;

private:
	void *handle_ = nullptr;
	EncryptFn encrypt_fn_ = nullptr;
	DecryptFn decrypt_fn_ = nullptr;
	SecureZeroFn secure_zero_fn_ = nullptr;
};

class SocketManager {
public:
	using DeliverHandler = std::function<void(const ipc::ChatRecord &)>;

	SocketManager(std::string node_name, std::array<std::uint8_t, 32> key, CryptoBridge *crypto);
	~SocketManager();

	SocketManager(const SocketManager &) = delete;
	SocketManager &operator=(const SocketManager &) = delete;

	bool start(std::uint16_t listen_port, DeliverHandler handler);
	void stop();
	bool connectPeer(const PeerEndpoint &endpoint);
	bool sendLocalMessage(const ipc::ChatRecord &record);
	std::size_t peerCount() const;

private:
	struct PeerConnection {
		int fd = -1;
		std::string peer_id;
		std::thread reader;
	};

	struct WireMessage {
		std::array<std::uint8_t, 16> uuid{};
		std::uint64_t timestamp_ms = 0;
		std::array<std::uint8_t, 32> sender_id{};
		std::array<std::uint8_t, 12> nonce{};
		std::array<std::uint8_t, 16> tag{};
		std::vector<std::uint8_t> ciphertext;
	};

	void acceptLoop();
	void peerReaderLoop(std::shared_ptr<PeerConnection> peer);
	void handleWireMessage(std::shared_ptr<PeerConnection> origin, const WireMessage &message);
	std::optional<WireMessage> readWireMessage(int fd);
	bool sendWireMessage(int fd, const WireMessage &message);
	bool writeAll(int fd, const std::uint8_t *data, std::size_t len);
	bool readAll(int fd, std::uint8_t *data, std::size_t len);
	WireMessage encryptForNetwork(const ipc::ChatRecord &record) const;
	bool decryptFromNetwork(const WireMessage &message, std::vector<std::uint8_t> &plaintext) const;
	void broadcast(const WireMessage &message, const std::string &exclude_peer_id);
	void addPeer(std::shared_ptr<PeerConnection> peer);
	std::string uuidToHex(const std::array<std::uint8_t, 16> &uuid) const;
	std::array<std::uint8_t, 32> senderDigestFor(const std::string &sender) const;

	std::string node_name_;
	std::array<std::uint8_t, 32> key_{};
	CryptoBridge *crypto_ = nullptr;
	DeliverHandler deliver_handler_;
	int listen_fd_ = -1;
	std::uint16_t listen_port_ = 0;
	std::atomic_bool running_{false};
	mutable std::mutex peers_mutex_;
	std::vector<std::shared_ptr<PeerConnection>> peers_;
	std::unordered_set<std::string> seen_uuids_;
	mutable std::mutex seen_mutex_;
	std::thread accept_thread_;
};

} // namespace aether::net
