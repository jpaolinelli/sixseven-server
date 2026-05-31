#include "sixseven/server/event_loop.h"

#include "sixseven/common/logging.h"

#include "sixseven/common/platform.h"

#include <cerrno>
#include <cstring>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/types.h>
#elif !defined(_WIN32)
#include <sys/epoll.h>
#endif

namespace {

#if defined(_WIN32)

/// On Windows we cannot use pipe fds with WSAPoll (pipes are not sockets).
/// Instead, create a pair of connected loopback UDP sockets for wakeup.
/// pipe_fds[0] = recv socket (polled), pipe_fds[1] = send socket.
bool make_wakeup_pipe(int pipe_fds[2]) {
    SOCKET recv_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET send_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (recv_sock == INVALID_SOCKET || send_sock == INVALID_SOCKET) {
        if (recv_sock != INVALID_SOCKET) closesocket(recv_sock);
        if (send_sock != INVALID_SOCKET) closesocket(send_sock);
        return false;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // OS picks port.

    if (bind(recv_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(recv_sock);
        closesocket(send_sock);
        return false;
    }

    int addrlen = sizeof(addr);
    if (getsockname(recv_sock, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) != 0) {
        closesocket(recv_sock);
        closesocket(send_sock);
        return false;
    }

    // Connect the send socket so we can call send() without specifying dest.
    if (connect(send_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(recv_sock);
        closesocket(send_sock);
        return false;
    }

    // Make recv socket non-blocking.
    u_long mode = 1;
    ioctlsocket(recv_sock, FIONBIO, &mode);

    pipe_fds[0] = static_cast<int>(recv_sock);
    pipe_fds[1] = static_cast<int>(send_sock);
    return true;
}

/// Drain all bytes from the read-end of the wakeup socket pair.
void drain_wakeup_pipe(int read_fd) {
    char buf[64];
    while (::recv(static_cast<SOCKET>(read_fd), buf, sizeof(buf), 0) > 0) {
        // Discard.
    }
}

#else // POSIX

/// Create a non-blocking pipe. Returns true on success.
bool make_wakeup_pipe(int pipe_fds[2]) {
    if (::pipe(pipe_fds) < 0) {
        return false;
    }
    // Set both ends non-blocking.
    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(pipe_fds[i], F_GETFL, 0);
        if (flags < 0 || ::fcntl(pipe_fds[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            return false;
        }
    }
    return true;
}

/// Drain all bytes from the read-end of a wakeup pipe.
void drain_wakeup_pipe(int read_fd) {
    char buf[64];
    while (::read(read_fd, buf, sizeof(buf)) > 0) {
        // Discard.
    }
}

#endif // _WIN32

} // anonymous namespace

namespace sixseven {

#ifdef __APPLE__

// ─── kqueue implementation ──────────────────────────────────────────────────

class KqueueEventLoop : public EventLoop {
public:
    KqueueEventLoop() = default;

    ~KqueueEventLoop() override {
        if (kq_fd_ >= 0) {
            ::close(kq_fd_);
        }
        if (wakeup_pipe_[0] >= 0) ::close(wakeup_pipe_[0]);
        if (wakeup_pipe_[1] >= 0) ::close(wakeup_pipe_[1]);
    }

    KqueueEventLoop(const KqueueEventLoop&) = delete;
    KqueueEventLoop& operator=(const KqueueEventLoop&) = delete;

    Result<void> init() override {
        kq_fd_ = ::kqueue();
        if (kq_fd_ < 0) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("kqueue(): ") + std::strerror(errno));
        }
        // Create wakeup pipe so other threads can interrupt poll().
        if (!make_wakeup_pipe(wakeup_pipe_)) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("pipe(): ") + std::strerror(errno));
        }
        // Register read-end with kqueue.
        struct kevent ev;
        EV_SET(&ev, wakeup_pipe_[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("kevent(wakeup): ") + std::strerror(errno));
        }
        return ok();
    }

    void wakeup() override {
        char byte = 1;
        { ssize_t rc = ::write(wakeup_pipe_[1], &byte, 1); (void)rc; }
    }

    Result<void> add_fd(int fd, EventType type) override { return add_filters(fd, type); }

    Result<void> modify_fd(int fd, EventType type) override { return set_filters(fd, type); }

    Result<void> remove_fd(int fd) override {
        // Remove both read and write filters; ignore ENOENT.
        struct kevent changes[2];
        EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        // Errors here are non-fatal (filter may not exist).
        ::kevent(kq_fd_, changes, 2, nullptr, 0, nullptr);
        return ok();
    }

    Result<std::vector<IoEvent>> poll(int timeout_ms) override {
        struct timespec ts;
        struct timespec* ts_ptr = nullptr;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
            ts_ptr = &ts;
        }

        struct kevent events[MAX_EVENTS];
        int n = ::kevent(kq_fd_, nullptr, 0, events, MAX_EVENTS, ts_ptr);
        if (n < 0) {
            if (errno == EINTR) {
                return ok(std::vector<IoEvent>{});
            }
            return make_error(StatusCode::IO_ERROR,
                              std::string("kevent(): ") + std::strerror(errno));
        }

        std::vector<IoEvent> result;
        result.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);
            // Drain the wakeup pipe but don't expose it as a client event.
            if (fd == wakeup_pipe_[0]) {
                drain_wakeup_pipe(wakeup_pipe_[0]);
                continue;
            }
            EventType type{};
            if (events[i].filter == EVFILT_READ) {
                type = EventType::READ;
            } else if (events[i].filter == EVFILT_WRITE) {
                type = EventType::WRITE;
            } else {
                continue;
            }
            result.push_back({fd, type});
        }
        return ok(std::move(result));
    }

private:
    static constexpr int MAX_EVENTS = 256;

    static bool has_flag(EventType t, EventType flag) {
        return (static_cast<uint8_t>(t) & static_cast<uint8_t>(flag)) != 0;
    }

    /// Add only the requested filters (used by add_fd).
    Result<void> add_filters(int fd, EventType type) {
        struct kevent changes[2];
        int n = 0;
        if (has_flag(type, EventType::READ)) {
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        }
        if (has_flag(type, EventType::WRITE)) {
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        }
        if (n > 0 && ::kevent(kq_fd_, changes, n, nullptr, 0, nullptr) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("kevent() add: ") + std::strerror(errno));
        }
        return ok();
    }

    /// Set exactly the requested filters, deleting others (used by modify_fd).
    Result<void> set_filters(int fd, EventType type) {
        // Apply adds/deletes one at a time to avoid a single ENOENT
        // on a non-existent filter aborting the whole changelist.
        if (has_flag(type, EventType::READ)) {
            struct kevent ev;
            EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
            ::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr);
        } else {
            struct kevent ev;
            EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            ::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr); // Ignore ENOENT.
        }
        if (has_flag(type, EventType::WRITE)) {
            struct kevent ev;
            EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
            ::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr);
        } else {
            struct kevent ev;
            EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            ::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr); // Ignore ENOENT.
        }
        return ok();
    }

    int kq_fd_ = -1;
    int wakeup_pipe_[2] = {-1, -1};
};

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<KqueueEventLoop>();
}

#elif defined(_WIN32)

// ─── WSAPoll implementation ─────────────────────────────────────────────────

class WsaPollEventLoop : public EventLoop {
public:
    WsaPollEventLoop() = default;

    ~WsaPollEventLoop() override {
        if (wakeup_pair_[0] >= 0) sixseven_platform::socket_close(wakeup_pair_[0]);
        if (wakeup_pair_[1] >= 0) sixseven_platform::socket_close(wakeup_pair_[1]);
    }

    WsaPollEventLoop(const WsaPollEventLoop&) = delete;
    WsaPollEventLoop& operator=(const WsaPollEventLoop&) = delete;

    Result<void> init() override {
        if (!make_wakeup_pipe(wakeup_pair_)) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("wakeup socket pair creation failed: WSAError=") +
                                  std::to_string(WSAGetLastError()));
        }
        // Register wakeup recv socket.
        WSAPOLLFD pfd{};
        pfd.fd = static_cast<SOCKET>(wakeup_pair_[0]);
        pfd.events = POLLRDNORM;
        poll_fds_.push_back(pfd);
        return ok();
    }

    void wakeup() override {
        char byte = 1;
        (void)::send(static_cast<SOCKET>(wakeup_pair_[1]), &byte, 1, 0);
    }

    Result<void> add_fd(int fd, EventType type) override {
        WSAPOLLFD pfd{};
        pfd.fd = static_cast<SOCKET>(fd);
        pfd.events = to_poll_events(type);
        poll_fds_.push_back(pfd);
        return ok();
    }

    Result<void> modify_fd(int fd, EventType type) override {
        for (auto& pfd : poll_fds_) {
            if (pfd.fd == static_cast<SOCKET>(fd)) {
                pfd.events = to_poll_events(type);
                return ok();
            }
        }
        return make_error(StatusCode::NOT_FOUND,
                          "modify_fd: fd not registered: " + std::to_string(fd));
    }

    Result<void> remove_fd(int fd) override {
        auto it = poll_fds_.begin();
        while (it != poll_fds_.end()) {
            if (it->fd == static_cast<SOCKET>(fd)) {
                poll_fds_.erase(it);
                return ok();
            }
            ++it;
        }
        return ok(); // Silently ignore if not found (matches POSIX epoll_ctl DEL behaviour).
    }

    Result<std::vector<IoEvent>> poll(int timeout_ms) override {
        if (poll_fds_.empty()) {
            return ok(std::vector<IoEvent>{});
        }

        int n = WSAPoll(poll_fds_.data(),
                        static_cast<ULONG>(poll_fds_.size()),
                        timeout_ms);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) {
                return ok(std::vector<IoEvent>{});
            }
            return make_error(StatusCode::IO_ERROR,
                              std::string("WSAPoll(): WSAError=") +
                                  std::to_string(WSAGetLastError()));
        }

        std::vector<IoEvent> result;
        result.reserve(static_cast<size_t>(n));
        for (auto& pfd : poll_fds_) {
            if (pfd.revents == 0) {
                continue;
            }
            int fd = static_cast<int>(pfd.fd);
            // Drain the wakeup socket but don't expose it as a client event.
            if (fd == wakeup_pair_[0]) {
                drain_wakeup_pipe(fd);
                continue;
            }
            EventType type{};
            bool has_read  = (pfd.revents & (POLLRDNORM | POLLERR | POLLHUP)) != 0;
            bool has_write = (pfd.revents & POLLWRNORM) != 0;
            if (has_read && has_write) {
                type = EventType::READ_WRITE;
            } else if (has_read) {
                type = EventType::READ;
            } else if (has_write) {
                type = EventType::WRITE;
            } else {
                // Error / hangup — treat as readable so the read path detects EOF.
                type = EventType::READ;
            }
            result.push_back({fd, type});
        }
        return ok(std::move(result));
    }

private:
    static SHORT to_poll_events(EventType type) {
        SHORT ev = 0;
        if ((static_cast<uint8_t>(type) & static_cast<uint8_t>(EventType::READ)) != 0) {
            ev |= POLLRDNORM;
        }
        if ((static_cast<uint8_t>(type) & static_cast<uint8_t>(EventType::WRITE)) != 0) {
            ev |= POLLWRNORM;
        }
        return ev;
    }

    std::vector<WSAPOLLFD> poll_fds_;
    int wakeup_pair_[2] = {-1, -1};
};

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<WsaPollEventLoop>();
}

#else

// ─── epoll implementation ───────────────────────────────────────────────────

class EpollEventLoop : public EventLoop {
public:
    EpollEventLoop() = default;

    ~EpollEventLoop() override {
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
        }
        if (wakeup_pipe_[0] >= 0) ::close(wakeup_pipe_[0]);
        if (wakeup_pipe_[1] >= 0) ::close(wakeup_pipe_[1]);
    }

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    Result<void> init() override {
        epoll_fd_ = ::epoll_create1(0);
        if (epoll_fd_ < 0) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("epoll_create1(): ") + std::strerror(errno));
        }
        // Create wakeup pipe so other threads can interrupt poll().
        if (!make_wakeup_pipe(wakeup_pipe_)) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("pipe(): ") + std::strerror(errno));
        }
        // Register read-end with epoll.
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wakeup_pipe_[0];
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_pipe_[0], &ev) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("epoll_ctl(wakeup): ") + std::strerror(errno));
        }
        return ok();
    }

    void wakeup() override {
        char byte = 1;
        { ssize_t rc = ::write(wakeup_pipe_[1], &byte, 1); (void)rc; }
    }

    Result<void> add_fd(int fd, EventType type) override {
        struct epoll_event ev{};
        ev.events = to_epoll_events(type);
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("epoll_ctl(ADD): ") + std::strerror(errno));
        }
        return ok();
    }

    Result<void> modify_fd(int fd, EventType type) override {
        struct epoll_event ev{};
        ev.events = to_epoll_events(type);
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("epoll_ctl(MOD): ") + std::strerror(errno));
        }
        return ok();
    }

    Result<void> remove_fd(int fd) override {
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT) {
            return make_error(StatusCode::IO_ERROR,
                              std::string("epoll_ctl(DEL): ") + std::strerror(errno));
        }
        return ok();
    }

    Result<std::vector<IoEvent>> poll(int timeout_ms) override {
        struct epoll_event events[MAX_EVENTS];
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                return ok(std::vector<IoEvent>{});
            }
            return make_error(StatusCode::IO_ERROR,
                              std::string("epoll_wait(): ") + std::strerror(errno));
        }

        std::vector<IoEvent> result;
        result.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            // Drain the wakeup pipe but don't expose it as a client event.
            if (fd == wakeup_pipe_[0]) {
                drain_wakeup_pipe(wakeup_pipe_[0]);
                continue;
            }
            EventType type{};
            bool has_read = (events[i].events & EPOLLIN) != 0;
            bool has_write = (events[i].events & EPOLLOUT) != 0;
            if (has_read && has_write) {
                type = EventType::READ_WRITE;
            } else if (has_read) {
                type = EventType::READ;
            } else if (has_write) {
                type = EventType::WRITE;
            } else {
                // Error / hangup — treat as readable so the read path detects EOF.
                type = EventType::READ;
            }
            result.push_back({fd, type});
        }
        return ok(std::move(result));
    }

private:
    static constexpr int MAX_EVENTS = 256;

    static uint32_t to_epoll_events(EventType type) {
        uint32_t ev = 0;
        if ((static_cast<uint8_t>(type) & static_cast<uint8_t>(EventType::READ)) != 0) {
            ev |= EPOLLIN;
        }
        if ((static_cast<uint8_t>(type) & static_cast<uint8_t>(EventType::WRITE)) != 0) {
            ev |= EPOLLOUT;
        }
        return ev;
    }

    int epoll_fd_ = -1;
    int wakeup_pipe_[2] = {-1, -1};
};

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<EpollEventLoop>();
}

#endif

} // namespace sixseven
