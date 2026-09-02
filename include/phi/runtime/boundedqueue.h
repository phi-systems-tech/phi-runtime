// runtime/boundedqueue.h
#pragma once

// A bounded hand-off between threads with the drop policy stated at the call
// site - the shape CoreLogFacade proved. State updates may shed the oldest
// under pressure and say how many; commands never shed, they fail and the
// caller answers the failure. There is no blocking push and no condition
// variable: the consumer drains from its loop, woken by a post.

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace phi::runtime {

template<typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(std::size_t capacity) : m_capacity(capacity) {}

    /// Fails when full. For work that must not be lost silently.
    bool tryPush(T value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.size() >= m_capacity)
            return false;
        m_items.push_back(std::move(value));
        return true;
    }

    /// Makes room by shedding the oldest and answers how many were shed.
    /// For state, where the newest value is the one that matters.
    std::size_t pushDroppingOldest(T value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::size_t dropped = 0;
        while (m_items.size() >= m_capacity) {
            m_items.pop_front();
            ++dropped;
        }
        m_items.push_back(std::move(value));
        return dropped;
    }

    std::optional<T> tryPop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.empty())
            return std::nullopt;
        T value = std::move(m_items.front());
        m_items.pop_front();
        return value;
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size();
    }

private:
    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::deque<T> m_items;
};

} // namespace phi::runtime

// Transition alias (6e-a): phi-core still says phicore::runtime everywhere;
// the alias dies with 6d's string sweep.
namespace phicore { namespace runtime = phi::runtime; }
