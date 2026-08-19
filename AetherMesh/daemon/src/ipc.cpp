#include "ipc.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace aether::ipc {
namespace {

constexpr char kMagic[4] = {'A', 'M', 'S', 'G'};
constexpr std::size_t kHeaderSize = 12;
constexpr std::uint32_t kMaxPayload = 1024 * 1024;

template <typename T>
void appendIntegral(std::vector<std::uint8_t> &buffer, T value) {
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		buffer.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
	}
}

template <typename T>
T readIntegral(const std::uint8_t *bytes, std::size_t offset) {
	T value = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		value |= static_cast<T>(bytes[offset + i]) << (8U * i);
	}
	return value;
}

std::uint64_t nowMs() {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
										  std::chrono::system_clock::now().time_since_epoch())
										  .count());
}

} // namespace

Server::Server() = default;

Server::~Server() {
	stop();
}

bool Server::start(const std::string &socket_path, IncomingHandler handler) {
	std::lock_guard lock(mutex_);
	if (running_) {
		return true;
	}

	socket_path_ = socket_path;
	incoming_handler_ = std::move(handler);

	::unlink(socket_path_.c_str());
	server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_fd_ < 0) {
		return false;
	}

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (socket_path_.size() >= sizeof(address.sun_path)) {
		::close(server_fd_);
		server_fd_ = -1;
		return false;
	}
	std::strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);

	if (::bind(server_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		::close(server_fd_);
		server_fd_ = -1;
		return false;
	}

	if (::listen(server_fd_, 1) < 0) {
		::close(server_fd_);
		server_fd_ = -1;
		return false;
	}

	running_ = true;
	accept_thread_ = std::thread(&Server::acceptLoop, this);
	return true;
}

void Server::stop() {
	std::unique_lock lock(mutex_);
	if (!running_) {
		if (server_fd_ >= 0) {
			::close(server_fd_);
			server_fd_ = -1;
		}
		return;
	}

	running_ = false;
	outgoing_cv_.notify_all();
	client_ready_cv_.notify_all();

	const int server_fd = server_fd_;
	const int client_fd = client_fd_;
	server_fd_ = -1;
	client_fd_ = -1;
	lock.unlock();

	if (server_fd >= 0) {
		::shutdown(server_fd, SHUT_RDWR);
		::close(server_fd);
	}
	if (client_fd >= 0) {
		::shutdown(client_fd, SHUT_RDWR);
		::close(client_fd);
	}

	if (accept_thread_.joinable()) {
		accept_thread_.join();
	}
	if (reader_thread_.joinable()) {
		reader_thread_.join();
	}
	if (writer_thread_.joinable()) {
		writer_thread_.join();
	}

	::unlink(socket_path_.c_str());
}

bool Server::send(const ChatRecord &record) {
	std::lock_guard lock(mutex_);
	if (!running_) {
		return false;
	}
	outgoing_.push(record);
	outgoing_cv_.notify_one();
	return true;
}

bool Server::isRunning() const {
	std::lock_guard lock(mutex_);
	return running_;
}

void Server::acceptLoop() {
	while (true) {
		{
			std::lock_guard lock(mutex_);
			if (!running_ || server_fd_ < 0) {
				return;
			}
		}

		const int accepted_fd = ::accept(server_fd_, nullptr, nullptr);
		if (accepted_fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (!running_) {
				return;
			}
			continue;
		}

		{
			std::lock_guard lock(mutex_);
			if (client_fd_ >= 0) {
				::close(client_fd_);
			}
			client_fd_ = accepted_fd;
		}

		client_ready_cv_.notify_all();

		if (reader_thread_.joinable()) {
			reader_thread_.join();
		}
		if (writer_thread_.joinable()) {
			writer_thread_.join();
		}

		reader_thread_ = std::thread(&Server::clientReaderLoop, this, accepted_fd);
		writer_thread_ = std::thread(&Server::clientWriterLoop, this, accepted_fd);
	}
}

void Server::clientReaderLoop(int client_fd) {
	while (true) {
		const auto frame = readFrame(client_fd);
		if (!frame) {
			break;
		}

		if (frame->first == FrameType::Send) {
			const auto record = decodeChatRecord(frame->second.data(), frame->second.size());
			if (record && incoming_handler_) {
				incoming_handler_(*record);
			}
		}

		if (frame->first == FrameType::Shutdown) {
			break;
		}
	}

	std::lock_guard lock(mutex_);
	if (client_fd_ == client_fd) {
		client_fd_ = -1;
	}
}

void Server::clientWriterLoop(int client_fd) {
	while (true) {
		std::unique_lock lock(mutex_);
		outgoing_cv_.wait(lock, [&] { return !running_ || !outgoing_.empty(); });
		if (!running_ && outgoing_.empty()) {
			break;
		}
		if (client_fd_ < 0) {
			continue;
		}

		ChatRecord record = std::move(outgoing_.front());
		outgoing_.pop();
		lock.unlock();

		const auto payload = encodeChatRecord(record);
		if (!sendFrame(client_fd, FrameType::Deliver, payload)) {
			break;
		}
	}
}

bool Server::sendFrame(int fd, FrameType type, const std::vector<std::uint8_t> &payload) {
	if (payload.size() > kMaxPayload) {
		return false;
	}

	std::vector<std::uint8_t> frame;
	frame.reserve(kHeaderSize + payload.size());
	frame.insert(frame.end(), std::begin(kMagic), std::end(kMagic));
	frame.push_back(static_cast<std::uint8_t>(type));
	frame.insert(frame.end(), 3, 0);
	appendIntegral<std::uint32_t>(frame, static_cast<std::uint32_t>(payload.size()));
	frame.insert(frame.end(), payload.begin(), payload.end());

	return writeAll(fd, frame.data(), frame.size());
}

std::optional<std::pair<FrameType, std::vector<std::uint8_t>>> Server::readFrame(int fd) {
	std::array<std::uint8_t, kHeaderSize> header{};
	if (!readAll(fd, header.data(), header.size())) {
		return std::nullopt;
	}

	if (!std::equal(std::begin(kMagic), std::end(kMagic), header.begin())) {
		return std::nullopt;
	}

	const auto type = static_cast<FrameType>(header[4]);
		const auto length = readIntegral<std::uint32_t>(header.data(), 8);
	if (length > kMaxPayload) {
		return std::nullopt;
	}

	std::vector<std::uint8_t> payload(length);
	if (length > 0 && !readAll(fd, payload.data(), payload.size())) {
		return std::nullopt;
	}

	return std::make_pair(type, std::move(payload));
}

std::vector<std::uint8_t> Server::encodeChatRecord(const ChatRecord &record) const {
	std::vector<std::uint8_t> payload;
	payload.reserve(16 + 8 + 2 + record.sender.size() + 4 + record.plaintext.size());

	payload.insert(payload.end(), record.uuid.begin(), record.uuid.end());
	appendIntegral<std::uint64_t>(payload, record.timestamp_ms == 0 ? nowMs() : record.timestamp_ms);

	if (record.sender.size() > 0xffffU || record.plaintext.size() > 0xffffffffU) {
		return {};
	}

	appendIntegral<std::uint16_t>(payload, static_cast<std::uint16_t>(record.sender.size()));
	payload.insert(payload.end(), record.sender.begin(), record.sender.end());
	appendIntegral<std::uint32_t>(payload, static_cast<std::uint32_t>(record.plaintext.size()));
	payload.insert(payload.end(), record.plaintext.begin(), record.plaintext.end());
	return payload;
}

std::optional<ChatRecord> Server::decodeChatRecord(const std::uint8_t *payload, std::size_t len) const {
	if (len < 16 + 8 + 2 + 4) {
		return std::nullopt;
	}

	ChatRecord record;
	std::copy_n(payload, 16, record.uuid.begin());
	record.timestamp_ms = readIntegral<std::uint64_t>(payload, 16);

	const auto sender_len = readIntegral<std::uint16_t>(payload, 24);
	std::size_t offset = 26;
	if (len < offset + sender_len + 4) {
		return std::nullopt;
	}

	record.sender.assign(reinterpret_cast<const char *>(payload + offset), sender_len);
	offset += sender_len;

	const auto plaintext_len = readIntegral<std::uint32_t>(payload, offset);
	offset += 4;
	if (len < offset + plaintext_len) {
		return std::nullopt;
	}

	record.plaintext.assign(reinterpret_cast<const char *>(payload + offset), plaintext_len);
	return record;
}

bool Server::writeAll(int fd, const std::uint8_t *data, std::size_t len) {
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

bool Server::readAll(int fd, std::uint8_t *data, std::size_t len) {
	std::size_t read_total = 0;
	while (read_total < len) {
		const ssize_t result = ::recv(fd, data + read_total, len - read_total, 0);
		if (result == 0) {
			return false;
		}
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		read_total += static_cast<std::size_t>(result);
	}
	return true;
}

} // namespace aether::ipc
