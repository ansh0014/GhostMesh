#include "socket_manager.hpp"

#include "sha256.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <random>

namespace aether::net {
namespace {

constexpr std::uint32_t kWireMagic = 0x54454841U; // AHET in little-endian bytes.
constexpr std::size_t kWireHeaderSize = 4 + 16 + 8 + 32 + 12 + 16 + 4;
constexpr std::size_t kMaxCiphertext = 1024 * 1024;

template <typename T>
void appendLE(std::vector<std::uint8_t> &buffer, T value) {
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		buffer.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
	}
}

template <typename T>
T readLE(const std::uint8_t *data) {
	T value = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		value |= static_cast<T>(data[i]) << (8U * i);
	}
	return value;
}

std::uint64_t nowMs() {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
										  std::chrono::system_clock::now().time_since_epoch())
										  .count());
}

std::array<std::uint8_t, 16> randomUuid() {
	std::array<std::uint8_t, 16> uuid{};
	std::random_device rd;
	for (auto &byte : uuid) {
		byte = static_cast<std::uint8_t>(rd() & 0xffU);
	}
	return uuid;
}

std::string endpointKey(const sockaddr_in &address) {
	char host[INET_ADDRSTRLEN] = {};
	if (::inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host)) == nullptr) {
		return {};
	}
	return std::string(host) + ":" + std::to_string(ntohs(address.sin_port));
}

} // namespace

bool CryptoBridge::load(const std::vector<std::string> &candidate_paths) {
	if (handle_ != nullptr) {
		return true;
	}

	for (const auto &path : candidate_paths) {
		if (path.empty()) {
			continue;
		}

		handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (handle_ == nullptr) {
			continue;
		}

		encrypt_fn_ = reinterpret_cast<EncryptFn>(::dlsym(handle_, "aether_encrypt"));
		decrypt_fn_ = reinterpret_cast<DecryptFn>(::dlsym(handle_, "aether_decrypt"));
		secure_zero_fn_ = reinterpret_cast<SecureZeroFn>(::dlsym(handle_, "aether_secure_zero"));
		if (encrypt_fn_ && decrypt_fn_ && secure_zero_fn_) {
			return true;
		}

		::dlclose(handle_);
		handle_ = nullptr;
		encrypt_fn_ = nullptr;
		decrypt_fn_ = nullptr;
		secure_zero_fn_ = nullptr;
	}

	return false;
}

bool CryptoBridge::loaded() const {
	return handle_ != nullptr && encrypt_fn_ != nullptr && decrypt_fn_ != nullptr && secure_zero_fn_ != nullptr;
}

int CryptoBridge::encrypt(const std::uint8_t *key, const std::uint8_t *plaintext, std::size_t len, std::uint8_t *out_nonce, std::uint8_t *out_tag, std::uint8_t *out_ciphertext) const {
	return encrypt_fn_ ? encrypt_fn_(key, plaintext, len, out_nonce, out_tag, out_ciphertext) : -1;
}

int CryptoBridge::decrypt(const std::uint8_t *key, const std::uint8_t *nonce, const std::uint8_t *tag, const std::uint8_t *ciphertext, std::size_t len, std::uint8_t *out_plaintext) const {
	return decrypt_fn_ ? decrypt_fn_(key, nonce, tag, ciphertext, len, out_plaintext) : -1;
}

void CryptoBridge::secureZero(void *ptr, std::size_t len) const {
	if (secure_zero_fn_) {
		secure_zero_fn_(ptr, len);
		return;
	}

	if (ptr != nullptr) {
		auto *bytes = static_cast<std::uint8_t *>(ptr);
		for (std::size_t i = 0; i < len; ++i) {
			bytes[i] = 0;
		}
	}
}

SocketManager::SocketManager(std::string node_name, std::array<std::uint8_t, 32> key, CryptoBridge *crypto)
	: node_name_(std::move(node_name)), key_(key), crypto_(crypto) {}

SocketManager::~SocketManager() {
	stop();
}

bool SocketManager::start(std::uint16_t listen_port, DeliverHandler handler) {
	if (running_) {
		return true;
	}

	deliver_handler_ = std::move(handler);
	listen_port_ = listen_port;
	listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd_ < 0) {
		return false;
	}

	int reuse = 1;
	::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(listen_port_);
	if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		::close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}

	if (::listen(listen_fd_, 64) < 0) {
		::close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}

	running_ = true;
	accept_thread_ = std::thread(&SocketManager::acceptLoop, this);
	return true;
}

void SocketManager::stop() {
	running_ = false;

	if (listen_fd_ >= 0) {
		::shutdown(listen_fd_, SHUT_RDWR);
		::close(listen_fd_);
		listen_fd_ = -1;
	}

	if (accept_thread_.joinable()) {
		accept_thread_.join();
	}

	std::vector<std::shared_ptr<PeerConnection>> peers_snapshot;
	{
		std::lock_guard lock(peers_mutex_);
		peers_snapshot = peers_;
		peers_.clear();
	}

	for (auto &peer : peers_snapshot) {
		if (!peer) {
			continue;
		}
		if (peer->fd >= 0) {
			::shutdown(peer->fd, SHUT_RDWR);
			::close(peer->fd);
			peer->fd = -1;
		}
		if (peer->reader.joinable()) {
			peer->reader.join();
		}
	}
}

bool SocketManager::connectPeer(const PeerEndpoint &endpoint) {
	if (!running_) {
		return false;
	}

	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(endpoint.port);
	if (::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
		::close(fd);
		return false;
	}

	if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		::close(fd);
		return false;
	}

	auto peer = std::make_shared<PeerConnection>();
	peer->fd = fd;
	peer->peer_id = endpoint.host + ":" + std::to_string(endpoint.port);
	peer->reader = std::thread(&SocketManager::peerReaderLoop, this, peer);
	addPeer(peer);
	return true;
}

bool SocketManager::sendLocalMessage(const ipc::ChatRecord &record) {
	if (!running_ || !crypto_ || !crypto_->loaded()) {
		return false;
	}

	const auto wire_message = encryptForNetwork(record);
	if (wire_message.ciphertext.empty() && !record.plaintext.empty()) {
		return false;
	}

	{
		std::lock_guard lock(seen_mutex_);
		seen_uuids_.insert(uuidToHex(wire_message.uuid));
	}

	broadcast(wire_message, std::string{});
	return true;
}

std::size_t SocketManager::peerCount() const {
	std::lock_guard lock(peers_mutex_);
	return peers_.size();
}

void SocketManager::acceptLoop() {
	while (running_) {
		const int accepted_fd = ::accept(listen_fd_, nullptr, nullptr);
		if (accepted_fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (!running_) {
				break;
			}
			continue;
		}

		sockaddr_in address{};
		socklen_t address_len = sizeof(address);
		if (::getpeername(accepted_fd, reinterpret_cast<sockaddr *>(&address), &address_len) < 0) {
			::close(accepted_fd);
			continue;
		}

		auto peer = std::make_shared<PeerConnection>();
		peer->fd = accepted_fd;
		peer->peer_id = endpointKey(address);
		peer->reader = std::thread(&SocketManager::peerReaderLoop, this, peer);
		addPeer(peer);
	}
}

void SocketManager::peerReaderLoop(std::shared_ptr<PeerConnection> peer) {
	while (running_ && peer && peer->fd >= 0) {
		const auto message = readWireMessage(peer->fd);
		if (!message) {
			break;
		}
		handleWireMessage(peer, *message);
	}

	if (peer && peer->fd >= 0) {
		::shutdown(peer->fd, SHUT_RDWR);
		::close(peer->fd);
		peer->fd = -1;
	}
}

void SocketManager::handleWireMessage(std::shared_ptr<PeerConnection> origin, const WireMessage &message) {
	const auto uuid_key = uuidToHex(message.uuid);
	{
		std::lock_guard lock(seen_mutex_);
		if (seen_uuids_.count(uuid_key) != 0) {
			return;
		}
		seen_uuids_.insert(uuid_key);
	}

	std::vector<std::uint8_t> plaintext;
	if (!decryptFromNetwork(message, plaintext)) {
		return;
	}

	ipc::ChatRecord record;
	record.uuid = message.uuid;
	record.timestamp_ms = message.timestamp_ms;
	record.sender = aether::hash::hex_encode(message.sender_id);
	record.plaintext.assign(reinterpret_cast<const char *>(plaintext.data()), plaintext.size());

	if (deliver_handler_) {
		deliver_handler_(record);
	}

	broadcast(message, origin ? origin->peer_id : std::string{});
}

std::optional<SocketManager::WireMessage> SocketManager::readWireMessage(int fd) {
	std::array<std::uint8_t, 4> magic{};
	if (!readAll(fd, magic.data(), magic.size())) {
		return std::nullopt;
	}

	if (readLE<std::uint32_t>(magic.data()) != kWireMagic) {
		return std::nullopt;
	}

	WireMessage message;
	if (!readAll(fd, message.uuid.data(), message.uuid.size())) {
		return std::nullopt;
	}
	if (!readAll(fd, reinterpret_cast<std::uint8_t *>(&message.timestamp_ms), sizeof(message.timestamp_ms))) {
		return std::nullopt;
	}
	if (!readAll(fd, message.sender_id.data(), message.sender_id.size())) {
		return std::nullopt;
	}
	if (!readAll(fd, message.nonce.data(), message.nonce.size())) {
		return std::nullopt;
	}
	if (!readAll(fd, message.tag.data(), message.tag.size())) {
		return std::nullopt;
	}

	std::array<std::uint8_t, 4> ciphertext_len_bytes{};
	if (!readAll(fd, ciphertext_len_bytes.data(), ciphertext_len_bytes.size())) {
		return std::nullopt;
	}
	const auto ciphertext_len = readLE<std::uint32_t>(ciphertext_len_bytes.data());
	if (ciphertext_len > kMaxCiphertext) {
		return std::nullopt;
	}

	message.ciphertext.resize(ciphertext_len);
	if (ciphertext_len > 0 && !readAll(fd, message.ciphertext.data(), message.ciphertext.size())) {
		return std::nullopt;
	}

	return message;
}

bool SocketManager::sendWireMessage(int fd, const WireMessage &message) {
	std::vector<std::uint8_t> buffer;
	buffer.reserve(kWireHeaderSize + message.ciphertext.size());

	appendLE<std::uint32_t>(buffer, kWireMagic);
	buffer.insert(buffer.end(), message.uuid.begin(), message.uuid.end());
	appendLE<std::uint64_t>(buffer, message.timestamp_ms);
	buffer.insert(buffer.end(), message.sender_id.begin(), message.sender_id.end());
	buffer.insert(buffer.end(), message.nonce.begin(), message.nonce.end());
	buffer.insert(buffer.end(), message.tag.begin(), message.tag.end());
	appendLE<std::uint32_t>(buffer, static_cast<std::uint32_t>(message.ciphertext.size()));
	buffer.insert(buffer.end(), message.ciphertext.begin(), message.ciphertext.end());

	return writeAll(fd, buffer.data(), buffer.size());
}

bool SocketManager::writeAll(int fd, const std::uint8_t *data, std::size_t len) {
	std::size_t written = 0;
	while (written < len) {
		const ssize_t result = ::send(fd, data + written, len - written, MSG_NOSIGNAL);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		written += static_cast<std::size_t>(result);
	}
	return true;
}

bool SocketManager::readAll(int fd, std::uint8_t *data, std::size_t len) {
	std::size_t total = 0;
	while (total < len) {
		const ssize_t result = ::recv(fd, data + total, len - total, 0);
		if (result == 0) {
			return false;
		}
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		total += static_cast<std::size_t>(result);
	}
	return true;
}

SocketManager::WireMessage SocketManager::encryptForNetwork(const ipc::ChatRecord &record) const {
	WireMessage message;
	message.uuid = record.uuid;
	if (message.uuid == std::array<std::uint8_t, 16>{}) {
		message.uuid = randomUuid();
	}
	message.timestamp_ms = record.timestamp_ms == 0 ? nowMs() : record.timestamp_ms;
	message.sender_id = senderDigestFor(record.sender);
	message.ciphertext.resize(record.plaintext.size());

	if (!crypto_ || !crypto_->loaded()) {
		message.ciphertext.clear();
		return message;
	}

	if (crypto_->encrypt(key_.data(), reinterpret_cast<const std::uint8_t *>(record.plaintext.data()), record.plaintext.size(), message.nonce.data(), message.tag.data(), message.ciphertext.data()) != 0) {
		message.ciphertext.clear();
	}
	return message;
}

bool SocketManager::decryptFromNetwork(const WireMessage &message, std::vector<std::uint8_t> &plaintext) const {
	if (!crypto_ || !crypto_->loaded()) {
		return false;
	}

	plaintext.resize(message.ciphertext.size());
	if (message.ciphertext.empty()) {
		return true;
	}

	return crypto_->decrypt(key_.data(), message.nonce.data(), message.tag.data(), message.ciphertext.data(), message.ciphertext.size(), plaintext.data()) == 0;
}

void SocketManager::broadcast(const WireMessage &message, const std::string &exclude_peer_id) {
	std::lock_guard lock(peers_mutex_);
	for (const auto &peer : peers_) {
		if (!peer || peer->fd < 0) {
			continue;
		}
		if (!exclude_peer_id.empty() && peer->peer_id == exclude_peer_id) {
			continue;
		}
		(void)sendWireMessage(peer->fd, message);
	}
}

void SocketManager::addPeer(std::shared_ptr<PeerConnection> peer) {
	if (!peer) {
		return;
	}

	std::lock_guard lock(peers_mutex_);
	peers_.push_back(std::move(peer));
}

std::string SocketManager::uuidToHex(const std::array<std::uint8_t, 16> &uuid) const {
		return aether::hash::hex_encode(uuid);
}

std::array<std::uint8_t, 32> SocketManager::senderDigestFor(const std::string &sender) const {
	return aether::hash::sha256_string(sender);
}

} // namespace aether::net
