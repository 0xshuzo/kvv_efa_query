#pragma once

#include "kvv_efa_query.h"
#include <chrono>
#include <functional>
#include <future>
#include <optional>

namespace kvv {

// Networking runs in a background worker. One request at a time, five seconds
// between request starts (or five seconds after an error); no catch-up bursts.
class TransitRefresh {
public:
    using Clock = std::chrono::steady_clock;
    using Fetch = std::function<std::string()>;

    explicit TransitRefresh(Fetch fetch, bool replay = false)
        : fetch_(std::move(fetch)), replay_(replay) {
    }

    // Poll only checks readiness and schedules work; it never waits for network I/O.
    bool Poll(Clock::time_point now);
    std::optional<TransitSnapshot> Snapshot(Clock::time_point now);

    const std::string& Error() const {
        return error_;
    }

private:
    Fetch fetch_;
    bool replay_ = false;
    bool received_ = false;
    std::future<std::string> pending_;
    TransitSession session_;
    Clock::time_point next_{};
    Clock::time_point received_at_{};
    std::string last_server_time_;
    std::string error_;
};

} // namespace kvv
