// runtime/epollloop.h
#pragma once

// The destination loop: epoll for descriptors, a deadline heap for timers,
// an eventfd for posts and stop. One instance per thread, everything owned
// by that thread; post() and stop() are the two thread-safe calls, each a
// mutex-guarded queue append plus an eventfd write.

#include "phi/runtime/loop.h"

#include <map>
#include <mutex>
#include <queue>
#include <vector>

namespace phi::runtime {

class EpollLoop final : public Loop
{
public:
    EpollLoop();
    ~EpollLoop() override;

    void post(Callback fn) override;
    void run() override;
    void stop() override;

protected:
    std::uint64_t addFdWatch(int fd, FdEvent event, Callback callback) override;
    void setFdWatchEnabled(std::uint64_t id, bool enabled) override;
    void removeFdWatch(std::uint64_t id) override;
    std::uint64_t addTimer(std::chrono::milliseconds delay, bool repeating,
                           Callback callback) override;
    void removeTimer(std::uint64_t id) override;

private:
    using Clock = std::chrono::steady_clock;

    struct Watch {
        int fd = -1;
        FdEvent event = FdEvent::Read;
        bool enabled = true;
        Callback callback;
    };
    /// The two possible watches on one descriptor; epoll carries the fd, this
    /// resolves it back to whoever is watching. 0 = no watch.
    struct FdIds {
        std::uint64_t readId = 0;
        std::uint64_t writeId = 0;
    };
    struct TimerEntry {
        Clock::time_point deadline;
        std::chrono::milliseconds interval{};
        bool repeating = false;
        Callback callback;
    };
    struct Due {
        Clock::time_point deadline;
        std::uint64_t id;
        bool operator>(const Due &other) const { return deadline > other.deadline; }
    };

    void updateEpoll(int fd);
    void deliverFdEvent(int fd, std::uint32_t ready, FdEvent event);
    void wake();
    void drainPosted();
    void fireDueTimers();
    int timeoutMs() const;

    int m_epollFd = -1;
    int m_eventFd = -1;
    bool m_stopped = false;

    std::uint64_t m_nextId = 1;
    std::map<std::uint64_t, Watch> m_watches;
    std::map<int, FdIds> m_byFd;
    std::map<std::uint64_t, TimerEntry> m_timers;
    std::priority_queue<Due, std::vector<Due>, std::greater<Due>> m_deadlines;

    std::mutex m_postMutex;
    std::vector<Callback> m_posted;
};

} // namespace phi::runtime

// Transition alias (6e-a): phi-core still says phicore::runtime everywhere;
// the alias dies with 6d's string sweep.
namespace phicore { namespace runtime = phi::runtime; }
