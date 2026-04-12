#include "sixseven/server/event_loop.h"

#include "sixseven/common/logging.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <sys/epoll.h>
#include <unistd.h>
#endif

namespace {

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
        (void)::write(wakeup_pipe_[1], &byte, 1);
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
        (void)::write(wakeup_pipe_[1], &byte, 1);
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
