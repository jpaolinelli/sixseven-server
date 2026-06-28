#include "sixseven/cli/socket_client.h"

#include "sixseven/common/platform.h"

#include <cerrno>
#include <cstring>
#include <string>

// Platform socket includes (platform.h already pulled them in via #ifdef).
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sixseven::cli {

namespace {

std::string last_socket_error() {
#if defined(_WIN32)
    int err = WSAGetLastError();
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,
                   err,
                   0,
                   buf,
                   sizeof(buf),
                   nullptr);
    return std::string(buf);
#else
    return std::string(std::strerror(errno));
#endif
}

void close_socket(int fd) {
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

} // namespace

SocketClient::~SocketClient() {
    disconnect();
}

SocketClient::SocketClient(SocketClient&& other) noexcept
    : fd_(other.fd_), recv_buf_(std::move(other.recv_buf_)) {
    other.fd_ = -1;
}

SocketClient& SocketClient::operator=(SocketClient&& other) noexcept {
    if (this != &other) {
        disconnect();
        fd_ = other.fd_;
        recv_buf_ = std::move(other.recv_buf_);
        other.fd_ = -1;
    }
    return *this;
}

Result<void> SocketClient::connect(const std::string& host, uint16_t port) {
    if (fd_ >= 0) {
        disconnect();
    }

    // Resolve host.
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
#if defined(_WIN32)
        return make_error(StatusCode::NETWORK_ERROR, "getaddrinfo failed: " + std::to_string(rc));
#else
        return make_error(StatusCode::NETWORK_ERROR,
                          "getaddrinfo failed: " + std::string(gai_strerror(rc)));
#endif
    }

    int sock = static_cast<int>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (sock < 0) {
        ::freeaddrinfo(res);
        return make_error(StatusCode::NETWORK_ERROR, "socket(): " + last_socket_error());
    }

    if (::connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        ::freeaddrinfo(res);
        close_socket(sock);
        return make_error(StatusCode::NETWORK_ERROR,
                          "connect() to " + host + ":" + port_str +
                              " failed: " + last_socket_error());
    }

    ::freeaddrinfo(res);
    fd_ = sock;
    recv_buf_.clear();
    return ok();
}

void SocketClient::disconnect() {
    if (fd_ >= 0) {
        // Best-effort Terminate message.
        auto term = encode_terminate_message();
        if (!term.empty()) {
#if defined(_WIN32)
            ::send(static_cast<SOCKET>(fd_),
                   reinterpret_cast<const char*>(term.data()),
                   static_cast<int>(term.size()),
                   0);
#else
            ::send(fd_, term.data(), term.size(), 0);
#endif
        }
        close_socket(fd_);
        fd_ = -1;
        recv_buf_.clear();
    }
}

bool SocketClient::is_connected() const {
    return fd_ >= 0;
}

Result<void> SocketClient::send_bytes(const std::vector<uint8_t>& data) {
    if (fd_ < 0) {
        return make_error(StatusCode::NETWORK_ERROR, "not connected");
    }
    size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        auto n = ::send(static_cast<SOCKET>(fd_),
                        reinterpret_cast<const char*>(data.data() + sent),
                        static_cast<int>(data.size() - sent),
                        0);
        if (n == SOCKET_ERROR) {
#else
        auto n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
#endif
            return make_error(StatusCode::NETWORK_ERROR, "send() failed: " + last_socket_error());
        }
        sent += static_cast<size_t>(n);
    }
    return ok();
}

Result<void> SocketClient::fill_buffer(size_t need) {
    while (recv_buf_.size() < need) {
        uint8_t chunk[4096];
#if defined(_WIN32)
        auto n = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n == 0) {
            return make_error(StatusCode::NETWORK_ERROR, "server closed connection");
        }
        if (n == SOCKET_ERROR) {
#else
        auto n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) {
            return make_error(StatusCode::NETWORK_ERROR, "server closed connection");
        }
        if (n < 0) {
#endif
            return make_error(StatusCode::NETWORK_ERROR, "recv() failed: " + last_socket_error());
        }
        recv_buf_.insert(recv_buf_.end(), chunk, chunk + static_cast<size_t>(n));
    }
    return ok();
}

Result<ServerMessage> SocketClient::read_message() {
    // Read until we have at least 5 bytes (type + length).
    auto fill5 = fill_buffer(5);
    if (!fill5) {
        return make_error(fill5.error().code, fill5.error().message);
    }

    while (true) {
        size_t consumed = 0;
        auto msg = decode_one_message(recv_buf_, consumed);
        if (msg) {
            recv_buf_.erase(recv_buf_.begin(),
                            recv_buf_.begin() + static_cast<ptrdiff_t>(consumed));
            return msg;
        }
        // NOT_FOUND means incomplete -- read more.
        if (msg.error().code == StatusCode::NOT_FOUND) {
            // Need at least 5 bytes to know total size; after that, need full message.
            size_t need = recv_buf_.size() + 1;
            if (recv_buf_.size() >= 5) {
                // We can compute the total expected size.
                uint32_t msg_len = (static_cast<uint32_t>(recv_buf_[1]) << 24) |
                                   (static_cast<uint32_t>(recv_buf_[2]) << 16) |
                                   (static_cast<uint32_t>(recv_buf_[3]) << 8) |
                                   static_cast<uint32_t>(recv_buf_[4]);
                need = 1 + static_cast<size_t>(msg_len);
            }
            auto fill_more = fill_buffer(need);
            if (!fill_more) {
                return make_error(fill_more.error().code, fill_more.error().message);
            }
        } else {
            return make_error(msg.error().code, msg.error().message);
        }
    }
}

Result<void> SocketClient::startup(const std::string& user, const std::string& database) {
    // Send StartupMessage.
    auto startup_msg = encode_startup_message(user, database);
    auto send_result = send_bytes(startup_msg);
    if (!send_result) {
        return make_error(send_result.error().code, send_result.error().message);
    }

    // Read messages until ReadyForQuery.
    while (true) {
        auto msg = read_message();
        if (!msg) {
            return make_error(msg.error().code, msg.error().message);
        }

        switch (msg->tag) {
        case ServerMsgTag::Authentication: {
            if (msg->auth.auth_type == 0) {
                // AuthenticationOk -- continue reading until ReadyForQuery.
                break;
            }
            // Any other auth type means the server wants credentials we don't support.
            std::string auth_name = "unknown";
            switch (msg->auth.auth_type) {
            case 3:
                auth_name = "CleartextPassword";
                break;
            case 5:
                auth_name = "MD5Password";
                break;
            case 10:
                auth_name = "SASL (SCRAM-SHA-256)";
                break;
            default:
                auth_name = "type " + std::to_string(msg->auth.auth_type);
                break;
            }
            return make_error(StatusCode::AUTH_ERROR,
                              "server requested authentication (" + auth_name +
                                  ") which is not supported by this client.\n"
                                  "Configure the server with auth_method = \"trust\" or run with "
                                  "a matching password client.");
        }
        case ServerMsgTag::ErrorResponse:
            return make_error(StatusCode::AUTH_ERROR,
                              msg->error_resp.severity + ": " + msg->error_resp.message);
        case ServerMsgTag::ReadyForQuery:
            return ok();
        case ServerMsgTag::BackendKeyData:
        case ServerMsgTag::ParameterStatus:
        case ServerMsgTag::NoticeResponse:
            // Normal startup messages -- ignore.
            break;
        default:
            // Unexpected message during startup -- ignore but continue.
            break;
        }
    }
}

} // namespace sixseven::cli
