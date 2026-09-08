// The runtime lib (Qt exit, phase 6a): the loop suite.
//
// This ran twice while the migration was under way, against EpollLoop and
// against the Qt-backed QtLoop; the suite passing on both was the property
// the 6c swap stood on. QtLoop fell with the thread architecture, and what
// remains pins the behaviour of the loop that won - delivery, ordering,
// re-entrancy, and what a reset handle no longer does.

#include "phi/runtime/boundedqueue.h"
#include "phi/runtime/epollloop.h"
#include "phi/runtime/oneshots.h"
#include "phi/runtime/signal.h"
#include "phi/runtime/thread.h"

#include "test_support.h"

#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using phi::runtime::BoundedQueue;
using phi::runtime::EpollLoop;
using phi::runtime::FdWatch;
using phi::runtime::Loop;
using phi::runtime::Signal;
using phi::runtime::SignalConnection;
using phi::runtime::Thread;
using phi::runtime::Timer;

namespace {

using LoopFactory = std::function<std::unique_ptr<Loop>()>;

/// Every loop test gets a watchdog: when the logic under test fails to stop
/// the loop, the watchdog does, and the assertions after run() say what was
/// missed - a hang would say nothing.
constexpr auto kWatchdog = std::chrono::milliseconds(3000);

void testPostedWorkRunsInOrderOnTheLoopThread(const LoopFactory &make)
{
    auto loop = make();
    std::vector<int> order;
    std::thread::id ranOn;

    loop->post([&]() { order.push_back(1); });
    loop->post([&]() {
        order.push_back(2);
        ranOn = std::this_thread::get_id();
        // A posted callback may post again; what it posts runs later, and
        // the loop is stoppable from within.
        loop->post([&]() {
            order.push_back(3);
            loop->stop();
        });
    });

    std::thread other([&]() { loop->post([&]() { order.push_back(4); }); });
    other.join();

    Timer watchdog = loop->timerAfter(kWatchdog, [&]() { loop->stop(); });
    loop->run();

    CHECK(order.size() == 4);
    CHECK(order[0] == 1 && order[1] == 2);
    CHECK(ranOn == std::this_thread::get_id());
}

void testTimersFireByDeadlineAndAResetOneStaysQuiet(const LoopFactory &make)
{
    auto loop = make();
    std::vector<int> order;

    Timer late = loop->timerAfter(std::chrono::milliseconds(60),
                                  [&]() { order.push_back(2); });
    Timer early = loop->timerAfter(std::chrono::milliseconds(15), [&]() {
        order.push_back(1);
        // Reset before its deadline: the late timer must never fire.
        late.reset();
    });
    Timer never = loop->timerAfter(std::chrono::milliseconds(20),
                                   [&]() { order.push_back(99); });
    never.reset();

    Timer end = loop->timerAfter(std::chrono::milliseconds(120),
                                 [&]() { loop->stop(); });
    loop->run();

    CHECK(order == std::vector<int>({1}));
    CHECK(!late);
    CHECK(static_cast<bool>(end)); // handle survives the shot; reset is a no-op
    end.reset();
}

void testARepeatingTimerTicksUntilReset(const LoopFactory &make)
{
    auto loop = make();
    int ticks = 0;
    Timer ticker = loop->timerEvery(std::chrono::milliseconds(10), [&]() {
        if (++ticks == 3) {
            ticker.reset(); // from inside its own callback
            loop->stop();
        }
    });
    Timer watchdog = loop->timerAfter(kWatchdog, [&]() { loop->stop(); });
    loop->run();
    CHECK(ticks == 3);
    CHECK(!ticker);
}

void testACallbackOutlivesTheHandleItRetires(const LoopFactory &make)
{
    // A callback that resets its own handle keeps running afterwards - a poll
    // that has decided it is finished still has to report that. The loop used
    // to destroy the running std::function inside reset(), so everything the
    // callback had captured was freed under its feet. It did not crash, which
    // is the only reason it lasted; the two re-entrancy tests above walked
    // straight into it the first time this suite ran under AddressSanitizer.
    //
    // The capture is deliberately too large for std::function's small-object
    // buffer, so the closure is a heap block and the read after the reset is a
    // read of freed memory rather than of a stack slot that happens to survive.
    auto loop = make();
    struct Bulky {
        std::string tag = "the callback still has its captures";
        std::vector<int> ballast = std::vector<int>(64, 7);
    };
    Bulky bulky;
    int ticks = 0;
    bool tagIntact = false;
    bool ballastIntact = false;

    Timer ticker;
    ticker = loop->timerEvery(std::chrono::milliseconds(5), [&loop, &ticker, &ticks,
                                                            bulky, &tagIntact,
                                                            &ballastIntact]() {
        if (++ticks < 2)
            return;
        ticker.reset();
        tagIntact = bulky.tag == "the callback still has its captures";
        ballastIntact = bulky.ballast.size() == 64 && bulky.ballast.back() == 7;
        loop->stop();
    });
    Timer watchdog = loop->timerAfter(kWatchdog, [&]() { loop->stop(); });
    loop->run();

    CHECK(ticks == 2);
    CHECK(!ticker);
    CHECK(tagIntact);
    CHECK(ballastIntact);
}

void testFdWatchesDeliverReadAndWrite(const LoopFactory &make)
{
    auto loop = make();
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

    int readable = 0;
    int writable = 0;
    FdWatch writeWatch = loop->watchFd(fds[0], Loop::FdEvent::Write, [&]() {
        // Writability is level-triggered noise unless silenced: count once,
        // disable, and send the bytes the read side is waiting for.
        ++writable;
        writeWatch.setEnabled(false);
        CHECK(::write(fds[0], "x", 1) == 1);
    });
    FdWatch readWatch = loop->watchFd(fds[1], Loop::FdEvent::Read, [&]() {
        char buf[8];
        ++readable;
        CHECK(::read(fds[1], buf, sizeof(buf)) == 1);
        loop->stop();
    });

    Timer watchdog = loop->timerAfter(kWatchdog, [&]() { loop->stop(); });
    loop->run();

    CHECK(writable == 1);
    CHECK(readable == 1);
    ::close(fds[0]);
    ::close(fds[1]);
}

void testADisabledWatchStaysSilentUntilEnabled(const LoopFactory &make)
{
    auto loop = make();
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    CHECK(::write(fds[0], "x", 1) == 1); // data already waiting

    int delivered = 0;
    FdWatch watch = loop->watchFd(fds[1], Loop::FdEvent::Read, [&]() {
        char buf[8];
        ++delivered;
        CHECK(::read(fds[1], buf, sizeof(buf)) == 1);
        loop->stop();
    });
    watch.setEnabled(false);

    // While disabled: the pending byte must not reach the callback.
    Timer probe = loop->timerAfter(std::chrono::milliseconds(40), [&]() {
        CHECK(delivered == 0);
        watch.setEnabled(true); // now it must
    });
    Timer watchdog = loop->timerAfter(kWatchdog, [&]() { loop->stop(); });
    loop->run();

    CHECK(delivered == 1);
    ::close(fds[0]);
    ::close(fds[1]);
}

void testADisabledWatchDoesNotSpinTheLoop(const LoopFactory &make)
{
    // A disabled watch must leave the poll asleep, not wake it and rely on
    // delivery-time suppression: pending data on a disabled read watch would
    // otherwise burn a core. Process CPU time over a 150 ms idle run is the
    // observable - near zero asleep, near wall-clock spinning.
    auto loop = make();
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    CHECK(::write(fds[0], "x", 1) == 1);

    FdWatch watch = loop->watchFd(fds[1], Loop::FdEvent::Read, []() {});
    watch.setEnabled(false);
    Timer end = loop->timerAfter(std::chrono::milliseconds(150),
                                 [&]() { loop->stop(); });

    rusage before{};
    ::getrusage(RUSAGE_SELF, &before);
    loop->run();
    rusage after{};
    ::getrusage(RUSAGE_SELF, &after);
    const long long cpuUs =
        (after.ru_utime.tv_sec - before.ru_utime.tv_sec) * 1000000LL
        + (after.ru_utime.tv_usec - before.ru_utime.tv_usec)
        + (after.ru_stime.tv_sec - before.ru_stime.tv_sec) * 1000000LL
        + (after.ru_stime.tv_usec - before.ru_stime.tv_usec);
    CHECK_MSG(cpuUs < 60000, "%lld us of CPU during an idle 150 ms run", cpuUs);
    ::close(fds[0]);
    ::close(fds[1]);
}

void testAWatchMayResetItselfMidCallback(const LoopFactory &make)
{
    // The socket classes close descriptors from inside their own readable
    // callback; the handle reset that goes with it must be safe there, and
    // final - the byte stays unread, so a level-triggered loop would deliver
    // again if anything of the watch survived.
    auto loop = make();
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    CHECK(::write(fds[0], "x", 1) == 1);

    int delivered = 0;
    FdWatch watch = loop->watchFd(fds[1], Loop::FdEvent::Read, [&]() {
        ++delivered;
        watch.reset();
    });
    Timer end = loop->timerAfter(std::chrono::milliseconds(80),
                                 [&]() { loop->stop(); });
    loop->run();

    CHECK(delivered == 1);
    CHECK(!watch);
    ::close(fds[0]);
    ::close(fds[1]);
}

void testACallbackMayRemoveTheOtherWatch(const LoopFactory &make)
{
    // The re-entrancy trap: both directions of one descriptor are ready in
    // the same poll round, and the first callback removes the second's
    // watch. Nothing may call a removed watch.
    auto loop = make();
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    CHECK(::write(fds[1], "x", 1) == 1); // fds[0] is readable and writable

    int reads = 0;
    int writes = 0;
    FdWatch writeWatch;
    FdWatch readWatch = loop->watchFd(fds[0], Loop::FdEvent::Read, [&]() {
        char buf[8];
        ++reads;
        CHECK(::read(fds[0], buf, sizeof(buf)) == 1);
        writeWatch.reset(); // the other watch, mid-dispatch
        loop->stop();
    });
    writeWatch = loop->watchFd(fds[0], Loop::FdEvent::Write, [&]() { ++writes; });

    Timer watchdog = loop->timerAfter(kWatchdog, [&]() { loop->stop(); });
    loop->run();

    CHECK(reads == 1);
    // Qt dispatches the two notifiers as separate events and may serve the
    // write first; epoll serves read first within one round. Either way,
    // after the read callback ran, the write watch is gone for good.
    CHECK(writes <= 1);
    CHECK(!writeWatch);
    ::close(fds[0]);
    ::close(fds[1]);
}

void testStopFromAnotherThreadEndsRun(const LoopFactory &make)
{
    auto loop = make();
    std::thread other([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        loop->stop();
    });
    Timer watchdog = loop->timerAfter(kWatchdog, [&]() { loop->stop(); });
    loop->run();
    other.join();
    CHECK(true); // reaching here is the assertion; the watchdog names a hang
}

void testCurrentNamesTheOwningThreadsLoop(const LoopFactory &make)
{
    auto loop = make();
    CHECK(Loop::current() == loop.get());
    CHECK(loop->isOnLoop());

    Loop *seenOnOther = loop.get(); // must be overwritten
    std::thread other([&]() { seenOnOther = Loop::current(); });
    other.join();
    CHECK(seenOnOther == nullptr);
}

void testOneShotsFireOnceAndDieWithTheBag(const LoopFactory &make)
{
    auto loop = make();
    int fired = 0;
    int never = 0;
    {
        phi::runtime::OneShots shots(*loop);
        shots.runOnce(std::chrono::milliseconds(10), [&]() {
            ++fired;
            // Re-entry from inside a shot must work.
            shots.runOnce(std::chrono::milliseconds(10), [&]() { ++fired; });
        });
        Timer end = loop->timerAfter(std::chrono::milliseconds(80), [&]() { loop->stop(); });
        loop->run();
        CHECK(fired == 2);
        CHECK(shots.pendingCount() == 0);
        shots.runOnce(std::chrono::milliseconds(5), [&]() { ++never; });
    } // the bag dies with a shot pending
    Timer end = loop->timerAfter(std::chrono::milliseconds(40), [&]() { loop->stop(); });
    loop->run();
    CHECK(never == 0);
}

void runLoopSuite(const char *label, const LoopFactory &make)
{
    const int before = phitest::g_failures;
    testPostedWorkRunsInOrderOnTheLoopThread(make);
    testTimersFireByDeadlineAndAResetOneStaysQuiet(make);
    testARepeatingTimerTicksUntilReset(make);
    testACallbackOutlivesTheHandleItRetires(make);
    testFdWatchesDeliverReadAndWrite(make);
    testADisabledWatchStaysSilentUntilEnabled(make);
    testADisabledWatchDoesNotSpinTheLoop(make);
    testAWatchMayResetItselfMidCallback(make);
    testACallbackMayRemoveTheOtherWatch(make);
    testStopFromAnotherThreadEndsRun(make);
    testCurrentNamesTheOwningThreadsLoop(make);
    testOneShotsFireOnceAndDieWithTheBag(make);
    if (phitest::g_failures != before)
        std::printf("  (failures above are from the %s loop)\n", label);
}

void testAThreadRunsItsOwnLoopAndJoins()
{
    std::atomic<int> ran{0};
    std::thread::id workerId;
    {
        Thread worker("runtime-test");
        CHECK(&worker.loop() != nullptr);
        worker.loop().post([&]() {
            workerId = std::this_thread::get_id();
            CHECK(Loop::current() == &worker.loop());
            ran.fetch_add(1);
        });
        // Destruction stops the loop and joins: everything posted before
        // the stop crossed is done when the block ends.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(ran.load() == 1);
    CHECK(workerId != std::this_thread::get_id());
}

void testBoundedQueueFailsOrShedsByChoice()
{
    BoundedQueue<int> queue(3);
    CHECK(queue.tryPush(1));
    CHECK(queue.tryPush(2));
    CHECK(queue.tryPush(3));
    CHECK(!queue.tryPush(4)); // commands fail, they are not shed
    CHECK(queue.size() == 3);

    CHECK(queue.pushDroppingOldest(5) == 1); // state sheds the oldest, counted
    CHECK(queue.size() == 3);
    CHECK(queue.tryPop() == std::optional<int>(2));
    CHECK(queue.tryPop() == std::optional<int>(3));
    CHECK(queue.tryPop() == std::optional<int>(5));
    CHECK(!queue.tryPop().has_value());
}

void testSignalFanOutSurvivesReentrantDisconnects()
{
    Signal<int> signal;
    std::vector<std::string> heard;

    SignalConnection first = signal.connect([&](int value) {
        heard.push_back("first:" + std::to_string(value));
    });
    SignalConnection second;
    second = signal.connect([&](int value) {
        heard.push_back("second:" + std::to_string(value));
        second.reset(); // disconnects itself mid-emit
    });
    SignalConnection third = signal.connect([&](int value) {
        heard.push_back("third:" + std::to_string(value));
    });
    CHECK(signal.slotCount() == 3);

    signal(7);
    CHECK((heard == std::vector<std::string>{"first:7", "second:7", "third:7"}));
    CHECK(signal.slotCount() == 2);

    heard.clear();
    SignalConnection late;
    SignalConnection connector = signal.connect([&](int value) {
        heard.push_back("connector:" + std::to_string(value));
        if (!late)
            late = signal.connect([&](int v) {
                heard.push_back("late:" + std::to_string(v));
            });
    });
    signal(8);
    // The slot connected during the emit waits for the next one.
    CHECK((heard == std::vector<std::string>{"first:8", "third:8", "connector:8"}));
    signal(9);
    CHECK((heard.back() == "late:9"));

    first.reset();
    third.reset();
    connector.reset();
    late.reset();
    heard.clear();
    signal(10);
    CHECK(heard.empty());
    CHECK(signal.slotCount() == 0);
}

void testAMovedConnectionKeepsTheSubscription()
{
    Signal<> signal;
    int calls = 0;
    SignalConnection a = signal.connect([&]() { ++calls; });
    SignalConnection b = std::move(a);
    CHECK(!a);
    CHECK(static_cast<bool>(b));
    signal();
    CHECK(calls == 1);
    b.reset();
    signal();
    CHECK(calls == 1);
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // QtLoop is gone with the 6c thread architecture; the suite that once
    // proved the two implementations equivalent now guards the one that won.
    runLoopSuite("epoll", []() { return std::make_unique<EpollLoop>(); });

    testAThreadRunsItsOwnLoopAndJoins();
    testBoundedQueueFailsOrShedsByChoice();
    testSignalFanOutSurvivesReentrantDisconnects();
    testAMovedConnectionKeepsTheSubscription();

    return phitest::report("runtime_tests");
}
