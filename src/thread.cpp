#include "phi/runtime/thread.h"

#include "phi/runtime/epollloop.h"

#include <pthread.h>

#include <utility>

namespace phi::runtime {

Thread::Thread(std::string name, LoopFactory factory)
{
    std::mutex mutex;
    std::condition_variable ready;
    Loop *made = nullptr;

    m_thread = std::thread([&mutex, &ready, &made, name = std::move(name),
                            factory = std::move(factory)]() {
        ::pthread_setname_np(::pthread_self(), name.substr(0, 15).c_str());
        // The loop is constructed on this thread: affinity starts at birth.
        std::unique_ptr<Loop> loop = factory ? factory()
                                             : std::make_unique<EpollLoop>();
        {
            std::lock_guard<std::mutex> lock(mutex);
            made = loop.get();
        }
        ready.notify_one();
        loop->run();
    });

    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [&made]() { return made != nullptr; });
    m_loop = made;
}

Thread::~Thread()
{
    stop();
}

void Thread::stop()
{
    if (!m_thread.joinable())
        return;
    m_loop->stop();
    m_thread.join();
    m_loop = nullptr;
}

} // namespace phi::runtime
