#include "transit_refresh.h"
#include "kvv_efa_constants.h"

#include <algorithm>
#include <iostream>

namespace kvv {

bool TransitRefresh::Poll(Clock::time_point now) {
    bool changed = false;

    // Consume a completed request without blocking the caller's update loop.
    if (pending_.valid() &&
        pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            session_.Update(pending_.get());
            const auto map = session_.View();

            // Repeated cached server timestamps must not falsely reset freshness.
            if (!received_ || map.server_time != last_server_time_) {
                received_at_ = now;
            }

            received_ = true;
            last_server_time_ = map.server_time;
            error_.clear();

            // Anchor every accepted update even if the UI skips that redraw.
            // Cached timestamps retain their age and cannot restart stale motion.
            if (!replay_) {
                session_.SmoothedView(
                    std::chrono::duration<double>(now - received_at_).count(), now);
            }

            std::cerr << "KVV refreshed " << map.server_time << ": " << map.vehicles.size()
                      << " estimated trains\n";
        } catch (const std::exception& error) {
            // Keep the last successful snapshot and schedule a delayed retry.
            error_ = error.what();
            next_ = now + std::chrono::seconds(5);
            std::cerr << "KVV refresh failed; retrying in 5s: " << error_ << '\n';
        }
        changed = true;
    }

    // One outstanding future prevents overlapping downloads and catch-up bursts.
    if (!pending_.valid() && now >= next_) {
        pending_ = std::async(std::launch::async, fetch_);
        next_ = now + std::chrono::seconds(5);
    }

    return changed;
}

std::optional<TransitSnapshot> TransitRefresh::Snapshot(Clock::time_point now) {
    if (!received_) {
        return std::nullopt;
    }

    const double elapsed = std::max(0.0, std::chrono::duration<double>(now - received_at_).count());
    const int age = static_cast<int>(std::min(elapsed, double(KVV_EFA_MAX_DISPLAY_AGE_SECONDS)));

    // Replay stays exact; live estimates absorb refreshed predictions through speed.
    auto map = replay_ ? session_.View(0) : session_.SmoothedView(elapsed, now);
    map.age_seconds = age;
    map.stale = !error_.empty() || age >= KVV_EFA_STALE_AFTER_SECONDS;
    map.replay = replay_;

    return map;
}

} // namespace kvv
