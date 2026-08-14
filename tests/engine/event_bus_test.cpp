#include "engine/core/events/event_bus.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>

using namespace engine;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

TEST_CASE("event bus", "[engine]") {
    auto& bus = EventBus::instance();
    bus.clear();  // hermetic: no state leaks across test runs

    // --- subscribe / dispatch / order ---
    {
        std::vector<std::string> received;
        bus.subscribe(events::kModInstalled,
                      [&](const std::string& eid, const std::string& payload) {
                          (void)eid;
                          received.push_back(payload);
                      });
        require(bus.subscriber_count(events::kModInstalled) == 1,
                "subscriber_count reflects the subscription");
        require(bus.subscriber_count(events::kModRemoved) == 0,
                "subscriber_count is per-event");

        bus.dispatch(events::kModInstalled, R"({"mod":"A"})");
        bus.dispatch(events::kModInstalled, R"({"mod":"B"})");
        require(received.size() == 2, "both dispatches delivered");
        require(received[0] == R"({"mod":"A"})", "payload 1 preserved verbatim");
        require(received[1] == R"({"mod":"B"})", "dispatch order preserved");

        // A different event id never reaches this subscriber.
        bus.dispatch(events::kModRemoved, R"({"mod":"A"})");
        require(received.size() == 2, "other events are not delivered");
    }

    // --- json_obj builds a valid escaped object string ---
    {
        std::string s = json_obj({
            {"mod", "Sky\"UI"},
            {"name", "line1\nline2"},
        });
        require(s == R"({"mod":"Sky\"UI","name":"line1\nline2"})",
                "json_obj escapes quotes and newlines");
    }

    // --- unsubscribe stops delivery; token is idempotent ---
    {
        int hits = 0;
        uint64_t tok = bus.subscribe(events::kModMoved,
                                     [&](const std::string&, const std::string&) { ++hits; });
        require(tok != 0, "subscribe returns a non-zero token");
        bus.dispatch(events::kModMoved, "{}");
        require(bus.unsubscribe(tok), "unsubscribe returns true for a live token");
        require(!bus.unsubscribe(tok), "second unsubscribe is false (idempotent)");
        bus.dispatch(events::kModMoved, "{}");
        require(hits == 1, "no delivery after unsubscribe");
    }

    // --- invalid subscriptions are refused ---
    {
        require(bus.subscribe("", [](const std::string&, const std::string&) {}) == 0,
                "empty event_id refused");
        require(bus.subscribe(events::kModInstalled, EventBus::Handler{}) == 0,
                "null handler refused");
    }

    // --- clear_source drops only that plugin's subscriptions ---
    {
        int a = 0, b = 0;
        bus.subscribe(events::kProfileChanged,
                      [&](const std::string&, const std::string&) { ++a; },
                      "/plugins/game_a.so");
        bus.subscribe(events::kProfileChanged,
                      [&](const std::string&, const std::string&) { ++b; },
                      "/plugins/game_b.so");
        bus.clear_source("/plugins/game_a.so");
        bus.dispatch(events::kProfileChanged, "{}");
        require(a == 0, "cleared source no longer receives");
        require(b == 1, "other source still receives");
        bus.clear();
        require(bus.subscriber_count(events::kProfileChanged) == 0,
                "clear() drops every subscription");
    }

    // --- reentrancy: a handler may emit and unsubscribe without deadlock ---
    {
        int nested = 0;
        bus.subscribe(events::kModRemoved,
                      [&](const std::string&, const std::string&) { ++nested; });
        uint64_t self_tok = 0;
        self_tok = bus.subscribe(events::kModStateChanged,
                                 [&](const std::string&, const std::string&) {
                                     bus.unsubscribe(self_tok);  // self-unsubscribe mid-dispatch
                                     bus.dispatch(events::kModRemoved, "{}");  // re-entrant emit
                                 });
        bus.dispatch(events::kModStateChanged, "{}");
        require(nested == 1, "re-entrant emit inside a handler delivered");
        bus.dispatch(events::kModStateChanged, "{}");
        require(nested == 1, "self-unsubscribed handler not called again");
    }

    // --- thread-safety: concurrent emits + subscribes never race ---
    {
        std::atomic<int> hits{0};
        constexpr int kSubs = 8;
        constexpr int kEmitters = 4;
        constexpr int kPerEmitter = 100;
        for (int i = 0; i < kSubs; ++i) {
            bus.subscribe(events::kGameLaunched,
                          [&](const std::string&, const std::string&) { hits.fetch_add(1); });
        }
        std::vector<std::thread> threads;
        for (int e = 0; e < kEmitters; ++e) {
            threads.emplace_back([&]() {
                for (int i = 0; i < kPerEmitter; ++i)
                    bus.dispatch(events::kGameLaunched, "{}");
            });
        }
        for (auto& t : threads) t.join();
        require(hits.load() == kSubs * kEmitters * kPerEmitter,
                "every subscription sees every concurrent dispatch exactly once");
    }

    bus.clear();
}
