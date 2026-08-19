#pragma once

#include "socket_manager.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unordered_set>

namespace aether::net {

class DiscoveryService {
public:
	DiscoveryService(std::array<std::uint8_t, 32> psk, std::array<std::uint8_t, 16> node_uuid, std::uint16_t listen_port);
	~DiscoveryService();

	DiscoveryService(const DiscoveryService &) = delete;
	DiscoveryService &operator=(const DiscoveryService &) = delete;

	bool start(SocketManager *manager);
	void stop();

private:
	void transmitLoop();
	void receiveLoop();
	bool sendBeacon(int fd, const sockaddr_in &destination);
	bool readBeacon(int fd);
	bool joinMulticastGroup(int fd);

	std::array<std::uint8_t, 32> psk_{};
	std::array<std::uint8_t, 16> node_uuid_{};
	std::uint16_t listen_port_ = 0;
	SocketManager *manager_ = nullptr;
	std::atomic_bool running_{false};
	std::unordered_set<std::string> discovered_endpoints_;
	std::mutex discovered_mutex_;
	std::thread tx_thread_;
	std::thread rx_thread_;
};

} // namespace aether::net
