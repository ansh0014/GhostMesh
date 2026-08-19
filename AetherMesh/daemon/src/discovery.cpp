#include "discovery.hpp"

#include "sha256.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <thread>
#include <vector>

namespace aether::net {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {0x41, 0x45, 0x54, 0x48};
constexpr char kMulticastIp[] = "224.0.0.1";
constexpr std::uint16_t kMulticastPort = 9999;
constexpr std::size_t kBeaconSize = 4 + 2 + 16 + 16;

template <typename T>
void appendLE(std::vector<std::uint8_t> &buffer, T value) {
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		buffer.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
	}
}

template <typename T>
T readLE(const std::uint8_t *data, std::size_t offset) {
	T value = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		value |= static_cast<T>(data[offset + i]) << (8U * i);
	}
	return value;
}

std::string endpointKey(const sockaddr_in &address, std::uint16_t port) {
	char host[INET_ADDRSTRLEN] = {};
	if (::inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host)) == nullptr) {
		return {};
	}
	return std::string(host) + ":" + std::to_string(port);
}

} // namespace

DiscoveryService::DiscoveryService(std::array<std::uint8_t, 32> psk, std::array<std::uint8_t, 16> node_uuid, std::uint16_t listen_port)
	: psk_(psk), node_uuid_(node_uuid), listen_port_(listen_port) {}

DiscoveryService::~DiscoveryService() {
	stop();
}

bool DiscoveryService::start(SocketManager *manager) {
	if (running_) {
		return true;
	}

	if (manager == nullptr) {
		return false;
	}

	manager_ = manager;
	running_ = true;
	tx_thread_ = std::thread(&DiscoveryService::transmitLoop, this);
	rx_thread_ = std::thread(&DiscoveryService::receiveLoop, this);
	return true;
}

void DiscoveryService::stop() {
	running_ = false;
	if (tx_thread_.joinable()) {
		tx_thread_.join();
	}
	if (rx_thread_.joinable()) {
		rx_thread_.join();
	}
}

void DiscoveryService::transmitLoop() {
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return;
	}

	int ttl = 1;
	::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

	sockaddr_in destination{};
	destination.sin_family = AF_INET;
	destination.sin_port = htons(kMulticastPort);
	::inet_pton(AF_INET, kMulticastIp, &destination.sin_addr);

	while (running_) {
		(void)sendBeacon(fd, destination);
		for (int i = 0; i < 50 && running_; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	::close(fd);
}

void DiscoveryService::receiveLoop() {
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return;
	}

	int reuse = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(kMulticastPort);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		::close(fd);
		return;
	}

	if (!joinMulticastGroup(fd)) {
		::close(fd);
		return;
	}

	while (running_) {
		if (!readBeacon(fd) && !running_) {
			break;
		}
	}

	::close(fd);
}

bool DiscoveryService::sendBeacon(int fd, const sockaddr_in &destination) {
	std::vector<std::uint8_t> payload;
	payload.reserve(kBeaconSize);
	payload.insert(payload.end(), kMagic.begin(), kMagic.end());
	appendLE<std::uint16_t>(payload, listen_port_);
	payload.insert(payload.end(), node_uuid_.begin(), node_uuid_.end());

	const auto mac = aether::hash::first16(aether::hash::hmac_sha256(psk_, payload));
	payload.insert(payload.end(), mac.begin(), mac.end());

	return ::sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&destination), sizeof(destination)) >= 0;
}

bool DiscoveryService::readBeacon(int fd) {
	std::array<std::uint8_t, kBeaconSize> beacon{};
	sockaddr_in sender{};
	socklen_t sender_len = sizeof(sender);
	const ssize_t received = ::recvfrom(fd, beacon.data(), beacon.size(), 0, reinterpret_cast<sockaddr *>(&sender), &sender_len);
	if (received != static_cast<ssize_t>(beacon.size())) {
		return false;
	}

	if (!std::equal(kMagic.begin(), kMagic.end(), beacon.begin())) {
		return false;
	}

	const auto port = readLE<std::uint16_t>(beacon.data(), 4);
	std::array<std::uint8_t, 16> uuid{};
	std::copy_n(beacon.begin() + 6, 16, uuid.begin());
	std::array<std::uint8_t, 16> mac{};
	std::copy_n(beacon.begin() + 22, 16, mac.begin());

	std::vector<std::uint8_t> signed_payload;
	signed_payload.reserve(22);
	signed_payload.insert(signed_payload.end(), kMagic.begin(), kMagic.end());
	appendLE<std::uint16_t>(signed_payload, port);
	signed_payload.insert(signed_payload.end(), uuid.begin(), uuid.end());

	const auto expected = aether::hash::first16(aether::hash::hmac_sha256(psk_.data(), psk_.size(), signed_payload.data(), signed_payload.size()));
	if (expected != mac || uuid == node_uuid_) {
		return true;
	}

	const auto endpoint = endpointKey(sender, port);
	{
		std::lock_guard lock(discovered_mutex_);
		if (!discovered_endpoints_.insert(endpoint).second) {
			return true;
		}
	}

	if (manager_) {
		char host[INET_ADDRSTRLEN] = {};
		if (::inet_ntop(AF_INET, &sender.sin_addr, host, sizeof(host)) != nullptr) {
			(void)manager_->connectPeer(PeerEndpoint{host, port});
		}
	}

	return true;
}

bool DiscoveryService::joinMulticastGroup(int fd) {
	ip_mreq request{};
	::inet_pton(AF_INET, kMulticastIp, &request.imr_multiaddr);
	request.imr_interface.s_addr = htonl(INADDR_ANY);
	return ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) == 0;
}

} // namespace aether::net
