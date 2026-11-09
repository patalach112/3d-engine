#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// EventBus (Phase 17)
//
// Lightweight, type-safe publish/subscribe hub that decouples event
// producers (physics, scene lifetime, gameplay) from consumers
// (gameplay, audio, UI, logging). Any struct can be an event; events
// are delivered synchronously, in subscription order, to every
// listener registered for that exact type.
//
// Semantics:
//   * subscribe<T>() returns an id for manual unsubscribe(), or use
//     subscribeScoped<T>() for an RAII handle that unsubscribes on
//     destruction. Listeners are never replayed past events.
//   * publish() dispatches over a snapshot of the listener list, so
//     listeners may safely subscribe/unsubscribe (including
//     themselves) during dispatch; structural changes take effect
//     from the NEXT publish.
//   * Publishing an event with no listeners is a no-op.
//
// The bus is deliberately single-threaded (the engine's main loop
// owns it); producers on other threads must hand events to the main
// thread first.
// ---------------------------------------------------------------------------
class EventBus {
public:
    using SubscriptionId = uint64_t;
    static constexpr SubscriptionId kInvalidSubscription = 0;

    // RAII subscription handle: destroys the subscription when the
    // handle goes out of scope. Move-only; the bus must outlive it.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(EventBus& bus, SubscriptionId id) : m_bus(&bus), m_id(id) {}
        ~Subscription() { reset(); }

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept : m_bus(other.m_bus), m_id(other.m_id) {
            other.m_bus = nullptr;
            other.m_id = kInvalidSubscription;
        }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                m_bus = other.m_bus;
                m_id = other.m_id;
                other.m_bus = nullptr;
                other.m_id = kInvalidSubscription;
            }
            return *this;
        }

        void reset() {
            if (m_bus != nullptr && m_id != kInvalidSubscription) {
                m_bus->unsubscribe(m_id);
            }
            m_bus = nullptr;
            m_id = kInvalidSubscription;
        }
        [[nodiscard]] bool active() const { return m_bus != nullptr && m_id != kInvalidSubscription; }

    private:
        EventBus* m_bus = nullptr;
        SubscriptionId m_id = kInvalidSubscription;
    };

    template <typename Event, typename Listener>
    SubscriptionId subscribe(Listener&& listener) {
        const SubscriptionId id = m_nextId++;
        m_listeners[std::type_index{typeid(Event)}].push_back(Entry{
            id, [fn = std::forward<Listener>(listener)](const void* raw) mutable {
                fn(*static_cast<const Event*>(raw));
            }});
        m_subscriptionTypes.emplace(id, std::type_index{typeid(Event)});
        return id;
    }

    template <typename Event, typename Listener>
    [[nodiscard]] Subscription subscribeScoped(Listener&& listener) {
        return Subscription{*this, subscribe<Event>(std::forward<Listener>(listener))};
    }

    void unsubscribe(SubscriptionId id) {
        if (id == kInvalidSubscription) {
            return;
        }
        const auto type = m_subscriptionTypes.find(id);
        if (type == m_subscriptionTypes.end()) {
            return;
        }
        if (const auto bucket = m_listeners.find(type->second); bucket != m_listeners.end()) {
            auto& entries = bucket->second;
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [id](const Entry& entry) { return entry.id == id; }),
                          entries.end());
        }
        m_subscriptionTypes.erase(type);
    }

    template <typename Event>
    void publish(const Event& event) {
        const auto bucket = m_listeners.find(std::type_index{typeid(Event)});
        if (bucket == m_listeners.end()) {
            return;
        }
        // Snapshot: listeners (un)subscribed by other listeners during
        // this dispatch only affect the next publish.
        const std::vector<Entry> snapshot = bucket->second;
        for (const Entry& entry : snapshot) {
            entry.invoke(static_cast<const void*>(&event));
        }
    }

    template <typename Event>
    [[nodiscard]] size_t listenerCount() const {
        const auto bucket = m_listeners.find(std::type_index{typeid(Event)});
        return bucket != m_listeners.end() ? bucket->second.size() : 0;
    }

    // Drops every subscription of every event type.
    void clear() {
        m_listeners.clear();
        m_subscriptionTypes.clear();
    }

private:
    struct Entry {
        SubscriptionId id = kInvalidSubscription;
        std::function<void(const void*)> invoke;
    };

    std::unordered_map<std::type_index, std::vector<Entry>> m_listeners;
    std::unordered_map<SubscriptionId, std::type_index> m_subscriptionTypes;
    SubscriptionId m_nextId = 1;
};

} // namespace engine
