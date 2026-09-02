// runtime/signal.h
#pragma once

// The fan-out that remains once connections are counted: of the 93 distinct
// signals the daemon connects, about ten have more than one receiver and
// none more than three. This is those hundred lines - connect() hands back a
// scoped connection, emission survives a slot disconnecting anyone (itself
// included) mid-emit, and a slot connected during an emit first hears the
// next one. Single-threaded on purpose: a Signal belongs to its owner's
// loop, and a thread crossing is a visible post() in the receiving slot,
// never something a signal does on its own.
//
// Emission is operator(), not a method named emit: Qt defines emit as an
// empty macro, and this header is included from Qt-built sources until the
// 6c swap ends that.

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace phi::runtime {

class SignalConnection
{
public:
    SignalConnection() = default;
    SignalConnection(SignalConnection &&other) noexcept = default;
    SignalConnection &operator=(SignalConnection &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_alive = std::move(other.m_alive);
        }
        return *this;
    }
    ~SignalConnection() { reset(); }
    SignalConnection(const SignalConnection &) = delete;
    SignalConnection &operator=(const SignalConnection &) = delete;

    explicit operator bool() const { return !m_alive.expired(); }
    void reset()
    {
        if (auto alive = m_alive.lock())
            *alive = false;
        m_alive.reset();
    }

private:
    template<typename... Args>
    friend class Signal;
    explicit SignalConnection(std::weak_ptr<bool> alive) : m_alive(std::move(alive)) {}
    std::weak_ptr<bool> m_alive;
};

template<typename... Args>
class Signal
{
public:
    using Slot = std::function<void(Args...)>;

    Signal() = default;
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;

    [[nodiscard]] SignalConnection connect(Slot slot)
    {
        auto entry = std::make_shared<Entry>();
        entry->alive = std::make_shared<bool>(true);
        entry->slot = std::move(slot);
        m_slots.push_back(entry);
        return SignalConnection(entry->alive);
    }

    void operator()(Args... args)
    {
        // The count pins this emission: slots connected while it runs wait
        // for the next one, slots disconnected while it runs are skipped by
        // their flag, and each entry is re-read from the list so a connect's
        // reallocation moves nothing out from under the loop.
        const std::size_t pinned = m_slots.size();
        for (std::size_t i = 0; i < pinned; ++i) {
            const std::shared_ptr<Entry> entry = m_slots[i];
            if (*entry->alive)
                entry->slot(args...);
        }
        // Compact outside the emission, never during one.
        std::erase_if(m_slots, [](const auto &entry) { return !*entry->alive; });
    }

    std::size_t slotCount() const
    {
        std::size_t count = 0;
        for (const auto &entry : m_slots)
            count += *entry->alive ? 1 : 0;
        return count;
    }

private:
    struct Entry {
        std::shared_ptr<bool> alive;
        Slot slot;
    };
    std::vector<std::shared_ptr<Entry>> m_slots;
};

} // namespace phi::runtime

// Transition alias (6e-a): phi-core still says phicore::runtime everywhere;
// the alias dies with 6d's string sweep.
namespace phicore { namespace runtime = phi::runtime; }
