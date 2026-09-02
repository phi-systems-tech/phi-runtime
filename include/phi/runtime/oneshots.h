// runtime/oneshots.h
#pragma once

// The QTimer::singleShot(ms, this, fn) shape: a deferred call that dies with
// its owner. Each shot owns its handle in the bag and erases itself when it
// fires; destroying the bag cancels everything still pending. The callback
// must not destroy the bag it lives in.

#include "phi/runtime/loop.h"

#include <cstdint>
#include <functional>
#include <map>
#include <utility>

namespace phi::runtime {

class OneShots
{
public:
    explicit OneShots(Loop &loop) : m_loop(loop) {}
    OneShots(const OneShots &) = delete;
    OneShots &operator=(const OneShots &) = delete;

    void runOnce(std::chrono::milliseconds delay, std::function<void()> fn)
    {
        const std::uint64_t id = m_next++;
        m_shots.emplace(id, m_loop.timerAfter(delay, [this, id, fn = std::move(fn)]() {
            // Erase first: fn may re-enter and schedule more shots. The
            // extracted handle is a fired timer, whose reset is a no-op.
            auto node = m_shots.extract(id);
            fn();
        }));
    }

    std::size_t pendingCount() const { return m_shots.size(); }

private:
    Loop &m_loop;
    std::uint64_t m_next = 1;
    std::map<std::uint64_t, Timer> m_shots;
};

} // namespace phi::runtime

// Transition alias (6e-a): phi-core still says phicore::runtime everywhere;
// the alias dies with 6d's string sweep.
namespace phicore { namespace runtime = phi::runtime; }
