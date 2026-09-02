#include "phi/runtime/epollloop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace phi::runtime {

namespace {
constexpr int kMaxEvents = 32;
}

EpollLoop::EpollLoop()
{
    m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    m_eventFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_epollFd < 0 || m_eventFd < 0) {
        std::fprintf(stderr, "runtime: epoll/eventfd creation failed\n");
        std::abort();
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = m_eventFd;
    ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_eventFd, &ev);
}

EpollLoop::~EpollLoop()
{
    ::close(m_eventFd);
    ::close(m_epollFd);
}

void EpollLoop::post(Callback fn)
{
    {
        std::lock_guard<std::mutex> lock(m_postMutex);
        m_posted.push_back(std::move(fn));
    }
    wake();
}

void EpollLoop::stop()
{
    // m_stopped is only read on the loop thread; the crossing here is the
    // same post-and-wake every other crossing uses.
    post([this]() { m_stopped = true; });
}

void EpollLoop::wake()
{
    const std::uint64_t one = 1;
    while (::write(m_eventFd, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
}

void EpollLoop::drainPosted()
{
    std::uint64_t value = 0;
    while (::read(m_eventFd, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
    // Swap the queue out so a posted callback may post again; what it adds
    // runs on the next round, after another wake.
    std::vector<Callback> batch;
    {
        std::lock_guard<std::mutex> lock(m_postMutex);
        batch.swap(m_posted);
    }
    for (Callback &fn : batch)
        fn();
}

int EpollLoop::timeoutMs() const
{
    if (m_deadlines.empty())
        return -1;
    const auto now = Clock::now();
    if (m_deadlines.top().deadline <= now)
        return 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        m_deadlines.top().deadline - now).count() + 1;
    return static_cast<int>(std::min<long long>(ms, 60000));
}

void EpollLoop::fireDueTimers()
{
    const auto now = Clock::now();
    while (!m_deadlines.empty() && m_deadlines.top().deadline <= now) {
        const Due due = m_deadlines.top();
        m_deadlines.pop();
        auto it = m_timers.find(due.id);
        // A reset timer leaves its heap entry behind; the map is the truth,
        // and ids are never reused. A repeating timer always has exactly one
        // entry - popped here, pushed again below - so a found id is due.
        if (it == m_timers.end())
            continue;
        if (it->second.repeating) {
            // The next deadline moves with a late loop instead of bursting.
            auto next = it->second.deadline + it->second.interval;
            if (next <= now)
                next = now + it->second.interval;
            it->second.deadline = next;
            m_deadlines.push({next, due.id});
            it->second.callback();
        } else {
            Callback callback = std::move(it->second.callback);
            m_timers.erase(it);
            callback();
        }
    }
}

void EpollLoop::deliverFdEvent(int fd, std::uint32_t ready, FdEvent event)
{
    // Re-resolved per delivery: the previous callback may have removed or
    // disabled this watch, or closed the descriptor and watched another.
    auto at = m_byFd.find(fd);
    if (at == m_byFd.end())
        return;
    const std::uint64_t id = event == FdEvent::Read ? at->second.readId
                                                    : at->second.writeId;
    if (id == 0)
        return;
    auto it = m_watches.find(id);
    if (it == m_watches.end() || !it->second.enabled)
        return;
    const std::uint32_t wanted = event == FdEvent::Read
                                     ? (EPOLLIN | EPOLLERR | EPOLLHUP)
                                     : (EPOLLOUT | EPOLLERR | EPOLLHUP);
    if ((ready & wanted) != 0)
        it->second.callback();
}

void EpollLoop::run()
{
    assertOnLoop("run");
    while (!m_stopped) {
        epoll_event events[kMaxEvents];
        const int count = ::epoll_wait(m_epollFd, events, kMaxEvents, timeoutMs());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            std::fprintf(stderr, "runtime: epoll_wait failed: %d\n", errno);
            std::abort();
        }
        for (int i = 0; i < count && !m_stopped; ++i) {
            const int fd = events[i].data.fd;
            if (fd == m_eventFd) {
                drainPosted();
                continue;
            }
            deliverFdEvent(fd, events[i].events, FdEvent::Read);
            deliverFdEvent(fd, events[i].events, FdEvent::Write);
        }
        if (!m_stopped)
            fireDueTimers();
    }
    m_stopped = false;
}

std::uint64_t EpollLoop::addFdWatch(int fd, FdEvent event, Callback callback)
{
    FdIds &ids = m_byFd[fd];
    std::uint64_t &slot = event == FdEvent::Read ? ids.readId : ids.writeId;
    if (slot != 0) {
        std::fprintf(stderr, "runtime: second %s watch on fd %d\n",
                     event == FdEvent::Read ? "read" : "write", fd);
        std::abort();
    }
    const std::uint64_t id = m_nextId++;
    slot = id;
    m_watches.emplace(id, Watch{fd, event, true, std::move(callback)});
    updateEpoll(fd);
    return id;
}

void EpollLoop::setFdWatchEnabled(std::uint64_t id, bool enabled)
{
    auto it = m_watches.find(id);
    if (it == m_watches.end() || it->second.enabled == enabled)
        return;
    it->second.enabled = enabled;
    updateEpoll(it->second.fd);
}

void EpollLoop::removeFdWatch(std::uint64_t id)
{
    auto it = m_watches.find(id);
    if (it == m_watches.end())
        return;
    const int fd = it->second.fd;
    const FdEvent event = it->second.event;
    m_watches.erase(it);
    auto at = m_byFd.find(fd);
    if (at != m_byFd.end()) {
        (event == FdEvent::Read ? at->second.readId : at->second.writeId) = 0;
        if (at->second.readId == 0 && at->second.writeId == 0)
            m_byFd.erase(at);
    }
    updateEpoll(fd);
}

std::uint64_t EpollLoop::addTimer(std::chrono::milliseconds delay, bool repeating,
                                  Callback callback)
{
    const std::uint64_t id = m_nextId++;
    const auto deadline = Clock::now() + delay;
    m_timers.emplace(id, TimerEntry{deadline, delay, repeating, std::move(callback)});
    m_deadlines.push({deadline, id});
    return id;
}

void EpollLoop::removeTimer(std::uint64_t id)
{
    m_timers.erase(id); // stale heap entries fall out in fireDueTimers
}

void EpollLoop::updateEpoll(int fd)
{
    auto at = m_byFd.find(fd);
    if (at == m_byFd.end()) {
        ::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
        return;
    }
    std::uint32_t mask = 0;
    for (const std::uint64_t id : {at->second.readId, at->second.writeId}) {
        if (id == 0)
            continue;
        const auto it = m_watches.find(id);
        if (it == m_watches.end() || !it->second.enabled)
            continue;
        mask |= it->second.event == FdEvent::Read ? EPOLLIN : EPOLLOUT;
    }
    epoll_event ev{};
    ev.events = mask; // 0 keeps the registration and delivers nothing
    ev.data.fd = fd;
    if (::epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT)
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev);
}

} // namespace phi::runtime
