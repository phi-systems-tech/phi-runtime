// runtime/thread.h
#pragma once

// A named thread running a loop of its own - the QThread-with-an-event-loop
// shape, without QThread. The constructor returns once the loop exists, so
// loop() is valid immediately; destruction stops the loop and joins.

#include "phi/runtime/loop.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace phi::runtime {

class Thread
{
public:
    using LoopFactory = std::function<std::unique_ptr<Loop>()>;

    /// Starts the thread and waits until its loop is running its factory's
    /// product. The default factory makes an EpollLoop. `name` lands in the
    /// kernel thread name (15 bytes survive), the way the Qt workers were
    /// named for ps and gdb.
    explicit Thread(std::string name, LoopFactory factory = {});
    ~Thread();
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;

    /// The thread's loop. post() is the only safe call from outside.
    Loop &loop() { return *m_loop; }

    /// Stops the loop and joins. Idempotent; the destructor calls it.
    void stop();

private:
    std::thread m_thread;
    Loop *m_loop = nullptr;
};

} // namespace phi::runtime

// Transition alias (6e-a): phi-core still says phicore::runtime everywhere;
// the alias dies with 6d's string sweep.
namespace phicore { namespace runtime = phi::runtime; }
