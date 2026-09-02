#include "phi/runtime/loop.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

namespace phi::runtime {

namespace {
thread_local Loop *t_current = nullptr;
}

Loop::Loop()
{
    if (t_current) {
        std::fprintf(stderr, "runtime: a second Loop on one thread\n");
        std::abort();
    }
    t_current = this;
}

Loop::~Loop()
{
    if (t_current == this)
        t_current = nullptr;
}

Loop *Loop::current()
{
    return t_current;
}

void Loop::assertOnLoop(const char *what) const
{
    if (current() == this)
        return;
    std::fprintf(stderr, "runtime: %s called off its loop's thread\n",
                 what ? what : "a loop-affine call");
    std::abort();
}

FdWatch Loop::watchFd(int fd, FdEvent event, Callback callback)
{
    assertOnLoop("watchFd");
    return FdWatch(this, addFdWatch(fd, event, std::move(callback)));
}

Timer Loop::timerAfter(std::chrono::milliseconds delay, Callback callback)
{
    assertOnLoop("timerAfter");
    return Timer(this, addTimer(delay, false, std::move(callback)));
}

Timer Loop::timerEvery(std::chrono::milliseconds interval, Callback callback)
{
    assertOnLoop("timerEvery");
    return Timer(this, addTimer(interval, true, std::move(callback)));
}

FdWatch::FdWatch(FdWatch &&other) noexcept
    : m_loop(std::exchange(other.m_loop, nullptr))
    , m_id(std::exchange(other.m_id, 0))
{
}

FdWatch &FdWatch::operator=(FdWatch &&other) noexcept
{
    if (this != &other) {
        reset();
        m_loop = std::exchange(other.m_loop, nullptr);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

FdWatch::~FdWatch()
{
    reset();
}

void FdWatch::reset()
{
    if (m_id != 0) {
        m_loop->assertOnLoop("FdWatch::reset");
        m_loop->removeFdWatch(m_id);
    }
    m_loop = nullptr;
    m_id = 0;
}

void FdWatch::setEnabled(bool enabled)
{
    if (m_id == 0)
        return;
    m_loop->assertOnLoop("FdWatch::setEnabled");
    m_loop->setFdWatchEnabled(m_id, enabled);
}

Timer::Timer(Timer &&other) noexcept
    : m_loop(std::exchange(other.m_loop, nullptr))
    , m_id(std::exchange(other.m_id, 0))
{
}

Timer &Timer::operator=(Timer &&other) noexcept
{
    if (this != &other) {
        reset();
        m_loop = std::exchange(other.m_loop, nullptr);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

Timer::~Timer()
{
    reset();
}

void Timer::reset()
{
    if (m_id != 0) {
        m_loop->assertOnLoop("Timer::reset");
        m_loop->removeTimer(m_id);
    }
    m_loop = nullptr;
    m_id = 0;
}

void postAndWait(Loop &loop, std::function<void()> fn)
{
    // Waiting for one's own loop is waiting for oneself.
    if (loop.isOnLoop()) {
        fn();
        return;
    }
    struct Handoff {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto handoff = std::make_shared<Handoff>();
    loop.post([handoff, fn = std::move(fn)]() {
        fn();
        {
            std::lock_guard<std::mutex> lock(handoff->mutex);
            handoff->done = true;
        }
        handoff->cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(handoff->mutex);
    handoff->cv.wait(lock, [&handoff]() { return handoff->done; });
}

} // namespace phi::runtime
