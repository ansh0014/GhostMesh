#include "discovery.hpp"
#include "ipc.hpp"
#include "sha256.hpp"
#include "socket_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Config {
	std::string name = "anonymous";
	std::string psk;
	std::uint16_t port = 7331;
	std::string ipc_socket = "/tmp/aether.sock";
};

std::atomic_bool g_running{true};

void signalHandler(int) {
	g_running = false;
}

bool parseArgs(int argc, char **argv, Config &config) {
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg{argv[i]};
		if (arg == "--name" && i + 1 < argc) {
			config.name = argv[++i];
		} else if (arg == "--psk" && i + 1 < argc) {
			config.psk = argv[++i];
		} else if (arg == "--port" && i + 1 < argc) {
			config.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
		} else if (arg == "--ipc-socket" && i + 1 < argc) {
			config.ipc_socket = argv[++i];
		} else if (arg == "--help") {
			std::cout << "Usage: aetherd --name <node> --psk <secret> --port <port> [--ipc-socket /tmp/aether.sock]\n";
			return false;
		}
	}

	if (config.psk.empty()) {
		std::cerr << "Missing --psk\n";
		return false;
	}

	return true;
}

std::array<std::uint8_t, 32> deriveKey(const std::string &psk) {
	const auto digest = aether::hash::sha256_string(psk);
	std::array<std::uint8_t, 32> key{};
	std::copy(digest.begin(), digest.end(), key.begin());
	return key;
}

std::array<std::uint8_t, 16> makeNodeUuid(const std::string &name, const std::string &psk) {
	const auto digest = aether::hash::sha256_string(name + "|" + psk);
	std::array<std::uint8_t, 16> uuid{};
	std::copy_n(digest.begin(), uuid.size(), uuid.begin());
	return uuid;
}

std::vector<std::string> cryptoCandidates() {
	std::vector<std::string> paths;
	if (const char *env = std::getenv("AETHER_CRYPTO_LIBRARY"); env != nullptr) {
		paths.emplace_back(env);
	}
	paths.emplace_back("../crypto/zig-out/lib/libaethercrypto.so");
	paths.emplace_back("./crypto/zig-out/lib/libaethercrypto.so");
	paths.emplace_back("libaethercrypto.so");
	return paths;
}

} // namespace

int main(int argc, char **argv) {
	Config config;
	if (!parseArgs(argc, argv, config)) {
		return 1;
	}

	std::signal(SIGINT, signalHandler);
	std::signal(SIGTERM, signalHandler);

	aether::net::CryptoBridge crypto;
	if (!crypto.load(cryptoCandidates())) {
		std::cerr << "Failed to load libaethercrypto.so. Set AETHER_CRYPTO_LIBRARY or build the Zig crypto library first.\n";
		return 1;
	}

	auto key = deriveKey(config.psk);
	const auto node_uuid = makeNodeUuid(config.name, config.psk);

	aether::ipc::Server ipc_server;
	aether::net::SocketManager socket_manager(config.name, key, &crypto);
	aether::net::DiscoveryService discovery(key, node_uuid, config.port);

	if (!ipc_server.start(config.ipc_socket, [&](const aether::ipc::ChatRecord &record) {
			(void)socket_manager.sendLocalMessage(record);
		})) {
		std::cerr << "Failed to start IPC server at " << config.ipc_socket << "\n";
		return 1;
	}

	if (!socket_manager.start(config.port, [&](const aether::ipc::ChatRecord &record) {
			(void)ipc_server.send(record);
		})) {
		std::cerr << "Failed to start TCP listener on port " << config.port << "\n";
		ipc_server.stop();
		return 1;
	}

	if (!discovery.start(&socket_manager)) {
		std::cerr << "Failed to start UDP discovery\n";
		socket_manager.stop();
		ipc_server.stop();
		return 1;
	}

	std::cout << "AetherMesh daemon running as '" << config.name << "' on port " << config.port << "\n";

	while (g_running) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	discovery.stop();
	socket_manager.stop();
	ipc_server.stop();

	crypto.secureZero(key.data(), key.size());
	return 0;
}
