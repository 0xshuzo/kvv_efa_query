#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kvv {

// WGS84 coordinates shared with consumers of the transit estimates.
struct GeoPoint {
    double latitude = 0.0;
    double longitude = 0.0;
};

struct Stop {
    std::string id;
    std::string name;
    GeoPoint position;
};

// A timetable estimate, not GPS. During dwell, from == to and progress == 0.
// Consumers can map this progress to their own route geometry.
struct VehicleEstimate {
    std::string line;
    std::string destination;
    GeoPoint from;
    GeoPoint to;
    double progress = 0.0;
};

// One provider reading, plus locally measured freshness information.
struct TransitSnapshot {
    // ISO UTC (Z) for live RapidJSON; local provider time for legacy replays.
    std::string server_time;
    std::vector<Stop> stops;
    std::vector<VehicleEstimate> vehicles;
    bool stale = false;
    bool replay = false;
    int age_seconds = 0;
};

// Transport and parsing stay separate so saved responses can use the same decoder.
// Select the nearest named rail/tram stop within 1000 m of the WGS84 center.
// Throws when coordinates are invalid, lookup fails, or no supported stop exists.
Stop FindNearestStop(GeoPoint center);
// Fetch one numeric EFA stop ID. KVV_EFA_RESPONSE_FILE bypasses validation/network.
std::string DownloadDepartureResponse(const std::string& stop_id);
TransitSnapshot ParseDepartureResponse(const std::string& response, int elapsed_seconds = 0);

// Keep recently observed trips available after they disappear from the board.
class TransitSession {
public:
    void Update(const std::string& response);
    TransitSnapshot View(int elapsed_seconds = 0) const;

    // Live presentation preserves each trip's position across prediction updates.
    // Use a steady clock; repeated calls at the same instant do not advance motion.
    // Anchor once after each Update, then redraw with the latest snapshot's age.
    TransitSnapshot SmoothedView(double elapsed_seconds, std::chrono::steady_clock::time_point now);

private:
    struct MotionState {
        std::string prediction;
        long long server_seconds = 0;
        double virtual_time = 0.0;
        double target_time = 0.0;
        double remaining_seconds = 0.0;
        std::chrono::steady_clock::time_point anchored_at{};

        double TimeAt(std::chrono::steady_clock::time_point now) const;
    };

    std::unordered_map<std::string, MotionState> motion_;
    std::string response_;
    long long server_seconds_ = 0;
    bool utc_clock_ = false;
    std::unordered_map<std::string, std::pair<std::string, long long>> rows_;
};

} // namespace kvv
