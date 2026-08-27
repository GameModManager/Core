#include "engine/core/events/event_bus.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace engine {

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

uint64_t EventBus::subscribe(const std::string& event_id, Handler handler,
                             const std::string& source) {
    if (event_id.empty() || !handler) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.push_back({next_token_, event_id, source, std::move(handler)});
    return next_token_++;
}

bool EventBus::unsubscribe(uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = subs_.begin(); it != subs_.end(); ++it) {
        if (it->token == token) {
            subs_.erase(it);
            return true;
        }
    }
    return false;
}

void EventBus::dispatch(const std::string& event_id,
                        const std::string& payload_json) const {
    if (event_id.empty()) return;

    // Record into the history ring buffer (always, even with no subscribers) so
    // viewers can show recent activity. Bounded to kMaxHistory entries.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(
            {event_id, payload_json, std::chrono::system_clock::now()});
        if (history_.size() > kMaxHistory) {
            const size_t excess = history_.size() - kMaxHistory;
            history_.erase(history_.begin(), history_.begin() + excess);
        }
    }

    // Copy the matching handlers under the lock, then invoke each OUTSIDE it:
    // a handler that re-emits or unsubscribes must never deadlock, and plugin
    // code must never run under a bus lock.
    std::vector<Handler> to_call;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        to_call.reserve(subs_.size());
        for (const auto& s : subs_)
            if (s.event_id == event_id) to_call.push_back(s.handler);
    }
    if (to_call.empty()) return;
    for (auto& h : to_call) h(event_id, payload_json);
}

void EventBus::clear_source(const std::string& source) {
    if (source.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [&](const Subscription& s) {
                                   return s.source == source;
                               }),
                subs_.end());
}

void EventBus::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.clear();
}

size_t EventBus::subscriber_count(const std::string& event_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& s : subs_)
        if (s.event_id == event_id) ++n;
    return n;
}

std::vector<EventRecord> EventBus::recent_events(size_t max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (max == 0 || max >= history_.size()) return history_;
    return std::vector<EventRecord>(history_.end() - max, history_.end());
}

namespace {

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

std::string json_obj(std::vector<std::pair<std::string, std::string>> kv) {
    std::string out = "{";
    bool first = true;
    for (auto& [k, v] : kv) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += escape_json(k);
        out += "\":\"";
        out += escape_json(v);
        out += "\"";
    }
    out += "}";
    return out;
}

}  // namespace engine
