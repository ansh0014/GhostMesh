#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aether::ipc {

enum class FrameType : std::uint8_t {
	Hello = 1,
	Send = 2,
	Deliver = 3,
	Shutdown = 4,
	Stats = 5,
};

struct ChatRecord {
	std::array<std::uint8_t, 16> uuid{};
	std::uint64_t timestamp_ms = 0;
	std::string sender;
	std::string plaintext;
};

class Server {
public:
	using IncomingHandler = std::function<void(const ChatRecord &)>;

	Server();
	~Server();

	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	bool start(const std::string &socket_path, IncomingHandler handler);
	void stop();
	bool send(const ChatRecord &record);
	bool isRunning() const;

private:
	void acceptLoop();
	void clientReaderLoop(int client_fd);
	void clientWriterLoop(int client_fd);
	bool sendFrame(int fd, FrameType type, const std::vector<std::uint8_t> &payload);
	std::optional<std::pair<FrameType, std::vector<std::uint8_t>>> readFrame(int fd);
	std::vector<std::uint8_t> encodeChatRecord(const ChatRecord &record) const;
	std::optional<ChatRecord> decodeChatRecord(const std::uint8_t *payload, std::size_t len) const;
	bool writeAll(int fd, const std::uint8_t *data, std::size_t len);
	bool readAll(int fd, std::uint8_t *data, std::size_t len);

	std::string socket_path_;
	IncomingHandler incoming_handler_;
	int server_fd_ = -1;
	int client_fd_ = -1;
	bool running_ = false;
	mutable std::mutex mutex_;
	std::condition_variable client_ready_cv_;
	std::condition_variable outgoing_cv_;
	std::queue<ChatRecord> outgoing_;
	std::thread accept_thread_;
	std::thread reader_thread_;
	std::thread writer_thread_;
};

} // namespace aether::ipc
