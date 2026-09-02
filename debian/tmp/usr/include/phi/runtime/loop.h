// runtime/loop.h
#pragma once

// The loop seam (Qt exit, phase 6a). This is the interface the phase 3-5
// classes have been coding against without naming it: a file descriptor
// watched for readability or writability, a timer, work posted from another
// thread, and one loop per thread that owns everything created on it.
//
// One implementation remains: EpollLoop. A Qt-backed QtLoop drove the same
// interface while the migration ran, so modules could move one by one under
// Qt; the suite in tests/runtime_tests proved the two equivalent, which is
// what made the 6c swap a swap rather than a rewrite. QtLoop fell with the
// 6c thread architecture.
//
// Affinity is the model, as it was under Qt: a Loop belongs to the thread
// that constructed it, and everything but post() and stop() must be called
// on that thread. A thread crossing is written as loop.post(...) - visible
// at the call site, never hidden in a connection type. assertOnLoop() is the
// compiler-adjacent enforcement: it aborts loudly in every build, because a
// wrong-thread call is a bug wherever it happens and costs one pointer
// comparison to catch.

#include <chrono>
#include <cstdint>
#include <functional>

namespace phi::runtime {

class Loop;

/// A watched file descriptor. Dropping the handle, or reset(), stops the
/// callbacks. Owned by the loop's thread, like everything else here.
class FdWatch
{
public:
    FdWatch() = default;
    FdWatch(FdWatch &&other) noexcept;
    FdWatch &operator=(FdWatch &&other) noexcept;
    ~FdWatch();
    FdWatch(const FdWatch &) = delete;
    FdWatch &operator=(const FdWatch &) = delete;

    explicit operator bool() const { return m_id != 0; }
    void reset();
    /// A disabled watch keeps its registration but delivers nothing - the
    /// QSocketNotifier::setEnabled shape the socket classes are built on.
    void setEnabled(bool enabled);

private:
    friend class Loop;
    FdWatch(Loop *loop, std::uint64_t id) : m_loop(loop), m_id(id) {}
    Loop *m_loop = nullptr;
    std::uint64_t m_id = 0;
};

/// A pending timer. Dropping the handle, or reset(), cancels it; a
/// single-shot timer that has fired holds nothing further.
class Timer
{
public:
    Timer() = default;
    Timer(Timer &&other) noexcept;
    Timer &operator=(Timer &&other) noexcept;
    ~Timer();
    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;

    explicit operator bool() const { return m_id != 0; }
    void reset();

private:
    friend class Loop;
    Timer(Loop *loop, std::uint64_t id) : m_loop(loop), m_id(id) {}
    Loop *m_loop = nullptr;
    std::uint64_t m_id = 0;
};

class Loop
{
public:
    enum class FdEvent { Read, Write };
    using Callback = std::function<void()>;

    virtual ~Loop();
    Loop(const Loop &) = delete;
    Loop &operator=(const Loop &) = delete;

    /// The loop constructed on the calling thread, or null. One loop per
    /// thread; constructing a second one aborts.
    static Loop *current();

    /// Watches `fd` for the event and calls back on this thread for as long
    /// as the handle lives and is enabled. At most one Read and one Write
    /// watch per descriptor.
    FdWatch watchFd(int fd, FdEvent event, Callback callback);

    /// Calls back once after `delay` on this thread.
    Timer timerAfter(std::chrono::milliseconds delay, Callback callback);
    /// Calls back every `interval` until the handle is dropped. A late loop
    /// does not burst to catch up; the next deadline moves with it.
    Timer timerEvery(std::chrono::milliseconds interval, Callback callback);

    /// Queues `fn` to run on this loop's thread. The one call that is safe
    /// from any thread, and the only way work crosses threads.
    virtual void post(Callback fn) = 0;

    /// Runs until stop(). Must be called on the owning thread.
    virtual void run() = 0;
    /// Ends run(). Safe from any thread; before run() it makes run() return
    /// at once.
    virtual void stop() = 0;

    /// The deleteLater shape (the 6c swap): destruction deferred to the next
    /// turn of the calling thread's loop, so a callback may retire the very
    /// object it is running inside. A post after the loop has finally stopped
    /// never runs; at process exit that is a leak the process takes with it.
    ///
    /// Handing an object here hands the loop sole ownership. A QObject is
    /// unparented first, because a parent that dies before the post drains
    /// would delete it too - Qt's deleteLater survived that only because
    /// ~QObject withdrew its own pending deferred-delete events, a net this
    /// post does not have.
    template <typename T>
    static void deleteDeferred(T *object)
    {
        if (!object)
            return;
        if constexpr (requires { object->setParent(nullptr); })
            object->setParent(nullptr);
        current()->post([object]() { delete object; });
    }

    bool isOnLoop() const { return current() == this; }
    /// Aborts with a message when called off the owning thread. Every build:
    /// the check is a pointer comparison, and a wrong-thread call is a bug
    /// wherever it happens.
    void assertOnLoop(const char *what) const;

protected:
    Loop();

    // The primitives an implementation provides. Handle bookkeeping and the
    // affinity checks live in the base so both implementations stay honest.
    virtual std::uint64_t addFdWatch(int fd, FdEvent event, Callback callback) = 0;
    virtual void setFdWatchEnabled(std::uint64_t id, bool enabled) = 0;
    virtual void removeFdWatch(std::uint64_t id) = 0;
    virtual std::uint64_t addTimer(std::chrono::milliseconds delay, bool repeating,
                                   Callback callback) = 0;
    virtual void removeTimer(std::uint64_t id) = 0;

private:
    friend class FdWatch;
    friend class Timer;
};

/// Posts `fn` onto `loop` and parks the calling thread until it has run -
/// the Qt::BlockingQueuedConnection shape, written as the post it is (the
/// 6c thread architecture). Called on `loop`'s own thread it just runs `fn`,
/// because waiting for one's own loop is waiting for oneself. No timeout:
/// this is for startup loads and shutdown flushes, which must complete.
void postAndWait(Loop &loop, std::function<void()> fn);

} // namespace phi::runtime

// Transition alias (6e-a): phi-core still says phicore::runtime everywhere;
// the alias dies with 6d's string sweep.
namespace phicore { namespace runtime = phi::runtime; }
