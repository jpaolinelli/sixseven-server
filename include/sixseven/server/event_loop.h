#pragma once

#include "sixseven/common/result.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace sixseven {

/// Types of I/O events the event loop can monitor.
enum class EventType : uint8_t {
    READ = 1,
    WRITE = 2,
    READ_WRITE = 3,
};

/// A single I/O event returned by EventLoop::poll().
struct IoEvent {
    int fd;
    EventType type;
};

/// Platform-abstracted I/O event loop (epoll on Linux, kqueue on macOS).
class EventLoop {
public:
    virtual ~EventLoop() = default;

    /// Initialize the event loop (create epoll/kqueue fd).
    [[nodiscard]] virtual Result<void> init() = 0;

    /// Register a file descriptor for the given event types.
    [[nodiscard]] virtual Result<void> add_fd(int fd, EventType type) = 0;

    /// Change the monitored event types for a file descriptor.
    [[nodiscard]] virtual Result<void> modify_fd(int fd, EventType type) = 0;

    /// Remove a file descriptor from monitoring.
    [[nodiscard]] virtual Result<void> remove_fd(int fd) = 0;

    /// Wait for events. Returns ready events. timeout_ms=-1 blocks indefinitely.
    [[nodiscard]] virtual Result<std::vector<IoEvent>> poll(int timeout_ms) = 0;

    /// Wake the event loop from another thread. Thread-safe.
    /// Used by thread pool workers to signal query completion so the event
    /// loop can re-enable the connection's fd without waiting for the next
    /// poll timeout.
    virtual void wakeup() = 0;

    /// Create the platform-appropriate EventLoop implementation.
    static std::unique_ptr<EventLoop> create();
};

} // namespace sixseven
