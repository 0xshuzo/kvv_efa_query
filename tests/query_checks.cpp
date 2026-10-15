#include <kvv_efa_query.h>
#include <transit_refresh.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <stdexcept>
#include <thread>

using Json = nlohmann::json;
using Clock = kvv::TransitRefresh::Clock;

static void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static kvv::GeoPoint Position(double east, double north) {
    const double longitude_scale = 111320.0 * std::cos(49.006250 * 3.141592653589793 / 180.0);
    return {49.006250 + north / 111320.0, 8.431785 + east / longitude_scale};
}

static Json Call(const char* id, double east, double north, const char* time) {
    const auto point = Position(east, north);
    return {{"nameWO", "Test station"},
        {"ref",
            {{"id", id},
                {"coords", std::to_string(point.longitude) + "," + std::to_string(point.latitude)},
                {"depDateTimeSec", time}}}};
}

static void RequirePosition(
    const Json& feed, int elapsed, double east, double north, const char* message) {
    const auto map = kvv::ParseDepartureResponse(feed.dump(), elapsed);

    // These fixtures travel between (0,0) and (100,100). Check temporal
    // progress independently of the application's choice of rail path.
    const auto expected = Position((east + north) / 2, (east + north) / 2);
    Require(map.vehicles.size() == 1, message);

    const auto& estimate = map.vehicles.front();
    const kvv::GeoPoint position{estimate.from.latitude +
            (estimate.to.latitude - estimate.from.latitude) * estimate.progress,
        estimate.from.longitude +
            (estimate.to.longitude - estimate.from.longitude) * estimate.progress};

    Require(map.vehicles.size() == 1 && std::abs(position.latitude - expected.latitude) < 1e-5 &&
            std::abs(position.longitude - expected.longitude) < 1e-5,
        message);
}

static void CheckDepartureDelays(const Json& baseline) {
    Json delayed = baseline;
    auto& row = delayed["departureList"][0];
    row["servingLine"]["delay"] = "2";
    auto& next = row["onwardStopSeq"][0]["ref"];
    next["depDelay"] = 2;
    next["depValid"] = "1";
    RequirePosition(
        delayed, 180, 100, 0, "Line delay fallback did not shift the train by two minutes");
    Require(kvv::ParseDepartureResponse(delayed.dump(), 60).vehicles.empty(),
        "Delayed train moved before its predicted departure");

    row["realDateTime"] = row["dateTime"];
    row["realDateTime"]["minute"] = "01";
    RequirePosition(
        delayed, 150, 100, 0, "Absolute prediction was ignored or delay was applied twice");
    row["realDateTime"] = {{"minute", "bad"}};
    RequirePosition(
        delayed, 180, 100, 0, "Malformed absolute prediction blocked the delay fallback");

    Json early = baseline;
    early["parameters"][0]["value"] = "2026-10-13T11:59:00";
    early["departureList"][0]["servingLine"]["delay"] = -2;
    early["departureList"][0]["onwardStopSeq"][0]["ref"]["depDelay"] = "-2";
    RequirePosition(early, 0, 100, 0, "Negative delay was not treated as early running");
}

static void CheckMissingDelays(const Json& baseline) {
    for (const Json& delay : std::vector<Json>{
             "-9999", -9999, "bad", nullptr, "9223372036854775807", "-9223372036854775808"}) {
        Json missing = baseline;
        missing["departureList"][0]["servingLine"]["delay"] = delay;
        missing["departureList"][0]["onwardStopSeq"][0]["ref"]["depDelay"] = delay;
        RequirePosition(
            missing, 60, 100, 0, "Invalid or unknown delay did not fall back to schedule");
    }
    Json invalid = baseline;
    auto& invalid_ref = invalid["departureList"][0]["onwardStopSeq"][0]["ref"];
    invalid_ref["depDelay"] = "9";
    invalid_ref["depValid"] = 0;
    RequirePosition(invalid, 60, 100, 0, "Invalidated sequence delay affected the estimate");
}

static void CheckDwellDelays(const Json& baseline) {
    Json dwell = baseline;
    auto& dwell_row = dwell["departureList"][0];
    dwell_row["servingLine"]["delay"] = 1;
    auto& stop = dwell_row["onwardStopSeq"][0]["ref"];
    stop["arrDateTimeSec"] = "20261013 12:02:00";
    stop["arrDelay"] = "1";
    stop["arrValid"] = "1";
    stop["depDateTimeSec"] = "20261013 12:03:00";
    stop["depDelay"] = "2";
    stop["depValid"] = "1";
    RequirePosition(
        dwell, 120, 100, 0, "Inbound interpolation did not use the arrival's own delay");
    RequirePosition(
        dwell, 240, 100, 100, "Train did not wait between predicted arrival and departure");
    stop["arrValid"] = "0";
    RequirePosition(dwell, 90, 100, 0, "Invalid arrival delay was not ignored independently");
    stop["arrValid"] = "1";
    dwell_row["onwardStopSeq"].push_back(Call("after", 0, 0, "20261013 12:07:00"));
    RequirePosition(
        dwell, 360, 100, 0, "Outbound interpolation began before the delayed departure");

    Json arrival_only = baseline;
    auto& arrival_ref = arrival_only["departureList"][0]["onwardStopSeq"][0]["ref"];
    arrival_ref.erase("depDateTimeSec");
    arrival_ref["arrDateTime"] = "20261013 12:02";
    arrival_ref["arrDelay"] = 2;
    arrival_ref["arrValid"] = 1;
    RequirePosition(arrival_only, 120, 100, 0, "Arrival-only call lost its delay");
}

static void CheckMidnightDelay(const Json& baseline) {
    struct MidnightCase {
        int year;
        int month;
        int day;
        const char* server_time;
        const char* next_departure;
    };
    const MidnightCase cases[] = {
        {2026, 10, 13, "2026-10-14T00:02:00", "20261014 00:01:00"},
        {2026, 10, 31, "2026-11-01T00:02:00", "20261101 00:01:00"},
        {2026, 12, 31, "2027-01-01T00:02:00", "20270101 00:01:00"},
        {2024, 2, 28, "2024-02-29T00:02:00", "20240229 00:01:00"},
        {2024, 2, 29, "2024-03-01T00:02:00", "20240301 00:01:00"},
        {1900, 2, 28, "1900-03-01T00:02:00", "19000301 00:01:00"},
        {2000, 2, 28, "2000-02-29T00:02:00", "20000229 00:01:00"},
    };
    for (const auto& test : cases) {
        Json midnight = baseline;
        midnight["parameters"][0]["value"] = test.server_time;
        auto& night_row = midnight["departureList"][0];
        night_row["dateTime"] = {{"year", test.year}, {"month", test.month}, {"day", test.day},
            {"hour", 23}, {"minute", 59}};
        night_row["servingLine"]["delay"] = 2;
        auto& night_ref = night_row["onwardStopSeq"][0]["ref"];
        night_ref["depDateTimeSec"] = test.next_departure;
        night_ref["depDelay"] = 2;
        // The delayed trip runs from 00:01 to 00:03 across each calendar boundary.
        RequirePosition(midnight, 0, 100, 0, "Delay crossing midnight used the wrong service date");
    }
}

// Synthetic provider fixtures keep all timing checks independent of the network.
static Json MakeResponse() {
    Json row = {{"stopID", "7000007"}, {"nameWO", "Test station"}, {"x", "8.431785"},
        {"y", "49.006250"},
        {"dateTime",
            {{"year", "2026"}, {"month", "10"}, {"day", "13"}, {"hour", "12"}, {"minute", "00"}}},
        {"servingLine", {{"motType", "4"}, {"symbol", "1"}, {"stateless", "test"}}},
        {"prevStopSeq",
            Json::array({Call("7000007", -70, 10, "20261013 11:56:00"),
                Call("outside", 0, 2000, "20261013 11:58:00")})}};
    const Json response = {
        {"parameters", Json::array({{{"name", "serverTime"}, {"value", "2026-10-13T12:00:00"}}})},
        {"departureList", Json::array({row})}};

    return response;
}

static Json MakeMovingResponse(const Json& response) {
    Json moving = response;
    moving["departureList"][0]["prevStopSeq"] = Json::array();
    moving["departureList"][0]["onwardStopSeq"] =
        Json::array({Call("next", 100, 100, "20261013 12:02:00")});

    return moving;
}

static void CheckClassicSeconds(const Json& baseline) {
    for (const Json& second : std::vector<Json>{30, "30"}) {
        Json timed = baseline;
        auto& row = timed["departureList"][0];
        row["dateTime"]["second"] = second;
        RequirePosition(timed, 75, 100, 0, "Scheduled seconds were discarded");
        row["realDateTime"] = row["dateTime"];
        row["dateTime"].erase("second");
        row["servingLine"]["delay"] = 2;
        RequirePosition(timed, 75, 100, 0,
            "Absolute prediction seconds were discarded or delay was applied twice");
    }
    for (const Json& second : std::vector<Json>{-1, 60, "bad", nullptr, "4294967326"}) {
        Json invalid = baseline;
        auto& row = invalid["departureList"][0];
        row["realDateTime"] = row["dateTime"];
        row["realDateTime"]["second"] = second;
        row["servingLine"]["delay"] = 2;
        row["onwardStopSeq"][0]["ref"]["depDelay"] = 2;
        RequirePosition(
            invalid, 180, 100, 0, "Invalid absolute seconds blocked the scheduled delay fallback");
    }
}

static Json RapidLocation(const char* id, double east, double north, const char* planned = nullptr,
    const char* estimated = nullptr) {
    const auto point = Position(east, north);
    Json location = {{"id", id}, {"name", "Test station"},
        {"coord", Json::array({point.latitude, point.longitude})},
        {"properties", {{"stopId", id}}}};
    if (planned) {
        location["departureTimePlanned"] = planned;
    }
    if (estimated) {
        location["departureTimeEstimated"] = estimated;
    }
    return location;
}

static Json MakeRapidResponse() {
    Json event = {{"location", RapidLocation("7000007", 0, 0)},
        {"departureTimePlanned", "2026-10-13T12:00:00Z"},
        {"departureTimeEstimated", "2026-10-13T12:00:30Z"},
        {"realtimeStatus", Json::array({"MONITORED"})},
        {"transportation",
            {{"id", "test"}, {"number", "1"}, {"product", {{"class", 4}}},
                {"destination", {{"name", "test destination"}}},
                {"properties", {{"tripCode", 42}}}}},
        {"previousLocations",
            Json::array({RapidLocation(
                "previous", -100, -100, "2026-10-13T11:58:00Z", "2026-10-13T11:58:30Z")})},
        {"onwardLocations",
            Json::array({RapidLocation(
                "next", 100, 100, "2026-10-13T12:02:00Z", "2026-10-13T12:02:30Z")})}};
    return {
        {"parameters", Json::array({{{"name", "serverTime"}, {"value", "2026-10-13T12:00:00Z"}}})},
        {"stopEvents", Json::array({event})}};
}

static void CheckRapidSeconds(const Json& baseline) {
    struct PredictionCase {
        const char* departure;
        const char* next_departure;
        int elapsed;
    };
    const PredictionCase cases[] = {
        {"2026-10-13T12:00:30Z", "2026-10-13T12:02:30Z", 90},
        {"2026-10-13T12:00:45Z", "2026-10-13T12:02:45Z", 105},
        {"2026-10-13T11:59:30Z", "2026-10-13T12:01:30Z", 30},
    };
    for (const auto& test : cases) {
        Json feed = baseline;
        auto& event = feed["stopEvents"][0];
        event["departureTimeEstimated"] = test.departure;
        event["onwardLocations"][0]["departureTimeEstimated"] = test.next_departure;
        RequirePosition(
            feed, test.elapsed, 100, 0, "RapidJSON prediction lost its signed sub-minute delay");
    }
    RequirePosition(baseline, 0, -50, 0, "RapidJSON previous location prediction was ignored");
    const auto snapshot = kvv::ParseDepartureResponse(baseline.dump(), 90);
    Require(snapshot.stops.size() == 3 && snapshot.vehicles.size() == 1 &&
            snapshot.vehicles.front().line == "1" &&
            snapshot.vehicles.front().destination == "test destination",
        "RapidJSON locations or transportation metadata were lost");

    for (const Json& estimate : std::vector<Json>{"bad", "2026-10-13T12:00:61Z", nullptr}) {
        Json missing = baseline;
        auto& event = missing["stopEvents"][0];
        event["departureTimeEstimated"] = estimate;
        event["onwardLocations"][0]["departureTimeEstimated"] = estimate;
        RequirePosition(missing, 60, 100, 0,
            "Malformed RapidJSON estimates did not fall back to planned timestamps");
    }
    Json planned = baseline;
    planned["stopEvents"][0].erase("departureTimeEstimated");
    planned["stopEvents"][0]["onwardLocations"][0].erase("departureTimeEstimated");
    RequirePosition(planned, 60, 100, 0, "Missing RapidJSON estimates discarded planned times");
}

static void CheckRapidDwell(const Json& baseline) {
    Json dwell = baseline;
    auto& event = dwell["stopEvents"][0];
    auto& next = event["onwardLocations"][0];
    next["arrivalTimePlanned"] = "2026-10-13T12:02:00Z";
    next["arrivalTimeEstimated"] = "2026-10-13T12:02:30Z";
    next["departureTimePlanned"] = "2026-10-13T12:03:00Z";
    next["departureTimeEstimated"] = "2026-10-13T12:03:45Z";
    RequirePosition(dwell, 90, 100, 0, "RapidJSON inbound motion ignored the arrival estimate");
    RequirePosition(dwell, 180, 100, 100, "RapidJSON arrival and departure lost their dwell");
    event["onwardLocations"].push_back(
        RapidLocation("after", 0, 0, "2026-10-13T12:05:00Z", "2026-10-13T12:05:45Z"));
    RequirePosition(dwell, 285, 100, 0, "RapidJSON outbound motion ignored departure seconds");
}

static void CheckRapidFilteringAndClock(const Json& baseline) {
    Json filtered = baseline;
    filtered["stopEvents"][0]["transportation"]["product"]["class"] = 5;
    Require(kvv::ParseDepartureResponse(filtered.dump(), 90).vehicles.empty(),
        "RapidJSON bus appeared on the rail map");
    filtered["stopEvents"][0]["transportation"]["product"]["class"] = 1;
    RequirePosition(filtered, 90, 100, 0, "RapidJSON rail service was filtered out");
    filtered["stopEvents"][0]["realtimeStatus"] = Json::array({"MONITORED", "CANCELLED"});
    Require(kvv::ParseDepartureResponse(filtered.dump(), 90).vehicles.empty(),
        "Cancelled RapidJSON service appeared on the map");

    Json midnight = baseline;
    midnight["parameters"][0]["value"] = "2027-01-01T00:00:00Z";
    auto& event = midnight["stopEvents"][0];
    event["previousLocations"] = Json::array();
    event["departureTimePlanned"] = "2026-12-31T23:59:00Z";
    event["departureTimeEstimated"] = "2026-12-31T23:59:30Z";
    event["onwardLocations"][0]["departureTimePlanned"] = "2027-01-01T00:01:00Z";
    event["onwardLocations"][0]["departureTimeEstimated"] = "2027-01-01T00:01:30Z";
    RequirePosition(midnight, 30, 100, 0, "RapidJSON UTC midnight changed the predicted interval");

    Json unclocked = baseline;
    unclocked.erase("parameters");
    bool rejected = false;
    try {
        kvv::ParseDepartureResponse(unclocked.dump());
    } catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected, "RapidJSON replay invented a clock for an unstamped response");
}

static void CheckRapidSession(const Json& baseline) {
    kvv::TransitSession session;
    session.Update(baseline.dump());
    Json revised = baseline;
    revised["parameters"][0]["value"] = "2026-10-13T12:00:05Z";
    revised["stopEvents"][0]["departureTimeEstimated"] = "2026-10-13T12:00:45Z";
    revised["stopEvents"][0]["onwardLocations"][0]["departureTimeEstimated"] =
        "2026-10-13T12:02:45Z";
    session.Update(revised.dump());
    const auto changed = session.View(100);
    Require(
        changed.vehicles.size() == 1 && std::abs(changed.vehicles.front().progress - 0.5) < 1e-9,
        "RapidJSON refresh duplicated a trip or kept its previous prediction");
    Json empty = revised;
    empty["stopEvents"] = Json::array();
    empty["parameters"][0]["value"] = "2026-10-13T12:00:10Z";
    session.Update(empty.dump());
    Require(session.View().stops.size() == 3, "Recently missing RapidJSON trip was not retained");
    empty["parameters"][0]["value"] = "2026-10-13T12:03:06Z";
    session.Update(empty.dump());
    Require(session.View().stops.empty(), "Unobserved RapidJSON trip did not expire");

    kvv::TransitSession switched;
    switched.Update(MakeResponse().dump());
    empty["parameters"][0]["value"] = "2026-10-13T10:00:00Z";
    switched.Update(empty.dump());
    Require(switched.View().stops.empty(),
        "Switching from a local replay to UTC retained incompatible trip times");
}

static void CheckPredictionReplacement(const Json& moving) {
    // A new snapshot must replace the previous prediction for the same trip.
    kvv::TransitSession delay_session;
    delay_session.Update(moving.dump());
    Require(delay_session.View().vehicles.size() == 1, "Initial departure prediction missing");
    Json revised = moving;
    revised["parameters"][0]["value"] = "2026-10-13T12:00:05";
    revised["departureList"][0]["servingLine"]["delay"] = 2;
    revised["departureList"][0]["onwardStopSeq"][0]["ref"]["depDelay"] = 2;
    delay_session.Update(revised.dump());
    Require(delay_session.View().vehicles.empty(), "Refresh kept the old on-time estimate");
    revised["parameters"][0]["value"] = "2026-10-13T12:00:10";
    revised["departureList"][0]["servingLine"]["delay"] = 0;
    revised["departureList"][0]["onwardStopSeq"][0]["ref"]["depDelay"] = 0;
    delay_session.Update(revised.dump());
    Require(delay_session.View().vehicles.size() == 1, "Cleared delay persisted after refresh");
}

// Missing trips expire by feed time; invalid responses keep the last good state.
static void CheckRetentionAndCancellation(const Json& response) {
    kvv::TransitSession session;
    session.Update(response.dump());
    Json empty = response;
    empty["parameters"][0]["value"] = "2026-10-13T12:00:05";
    empty["departureList"] = Json::array();
    session.Update(empty.dump());
    Require(session.View().vehicles.size() == 1, "Recently departed trip immediately disappeared");
    empty["parameters"][0]["value"] = "2026-10-13T12:04:00";
    session.Update(empty.dump());
    const auto expired = session.View();
    Require(std::none_of(expired.stops.begin(), expired.stops.end(),
                [](const auto& stop) {
                    return stop.name ==
                        "Test station"; // Synthetic trip calls, not static OSM stations.
                }),
        "Unobserved trip did not expire");
    session.Update(empty.dump());
    bool rejected = false;
    try {
        session.Update("{}");
    } catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected && session.View().server_time == "2026-10-13T12:04:00",
        "Malformed refresh destroyed last successful snapshot");
    Json cancelled = response;
    cancelled["departureList"][0]["realtimeTripStatus"] = "TRIP_CANCELLED";
    Require(kvv::ParseDepartureResponse(cancelled.dump()).vehicles.empty(),
        "Cancelled train appeared on map");
}

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::optional<std::string>& value) : name_(name) {
        if (const char* previous = std::getenv(name_)) {
            previous_ = previous;
        }
        const int result = value ? setenv(name_, value->c_str(), 1) : unsetenv(name_);
        Require(result == 0, "Could not configure transport test environment");
    }
    ~ScopedEnvironment() {
        if (previous_) {
            setenv(name_, previous_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }

private:
    const char* name_;
    std::optional<std::string> previous_;
};

class TransportDirectory {
public:
    TransportDirectory() {
        char pattern[] = "/tmp/kvv-efa-transport-XXXXXX";
        const char* directory = mkdtemp(pattern);
        Require(directory != nullptr, "Could not create transport test directory");
        path = directory;
    }
    ~TransportDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

static void WriteTransportFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary);
    file << contents;
    Require(file.good(), "Could not write transport test fixture");
}

static void CheckNearestStopAndTransport(const Json& baseline) {
    TransportDirectory directory;
    const char* original_path = std::getenv("PATH");
    ScopedEnvironment search_path(
        "PATH", directory.path.string() + ":" + (original_path ? original_path : "/usr/bin:/bin"));
    ScopedEnvironment response_file("KVV_EFA_RESPONSE_FILE", std::nullopt);
    WriteTransportFile(directory.path / "curl",
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.arguments\"\n"
        "/bin/cat \"$0.response\"\n");
    std::filesystem::permissions(directory.path / "curl", std::filesystem::perms::owner_all);
    const auto arguments = [&] {
        std::ifstream input(directory.path / "curl.arguments");
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const kvv::GeoPoint center{48.994369, 8.399670};
    const auto location = [](const char* id, const char* name, double latitude, double longitude,
                              int mode = 4) -> Json {
        return {{"id", "de:08212:89"}, {"name", name}, {"type", "stop"},
            {"coord", {latitude, longitude}}, {"productClasses", {mode}},
            {"properties", {{"stopId", id}, {"distance", 0}}}};
    };
    Json malformed = location("7000999", "Malformed", center.latitude, center.longitude);
    malformed["coord"] = {"48.994369", "8.399670"};
    Json nearest = location("7000089", "Hauptbahnhof (Vorplatz)", 48.994400, 8.399800);
    nearest["properties"]["distance"] = 999;
    const Json candidates = {{"locations",
        Json::array({location("7000090", "Further rail stop", 48.998, 8.399670, 1),
            location("7000100", "Bus only", center.latitude, center.longitude, 5),
            location("7000089'", "Invalid ID", center.latitude, center.longitude),
            location("7000101", "", center.latitude, center.longitude),
            location("7000103", " \t\n", center.latitude, center.longitude),
            location("7000102", "Invalid position", 91.0, 8.399670), malformed, nearest})}};
    WriteTransportFile(directory.path / "curl.response", candidates.dump());

    // Global decimal formatting must not corrupt coordinates in a request URL.
    struct CommaDecimal : std::numpunct<char> {
        char do_decimal_point() const override {
            return ',';
        }
    };
    struct RestoreLocale {
        std::locale previous = std::locale();
        ~RestoreLocale() {
            std::locale::global(previous);
        }
    } restore_locale;
    std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    const auto stop = kvv::FindNearestStop(center);
    std::locale::global(restore_locale.previous);
    Require(stop.id == "7000089" && stop.name == "Hauptbahnhof (Vorplatz)" &&
            stop.position.latitude == 48.994400 && stop.position.longitude == 8.399800,
        "Nearest rail/tram lookup trusted response order, walking distance, or reversed "
        "coordinates");
    Require(arguments().find("XML_COORD_REQUEST") != std::string::npos &&
            arguments().find("coord=8.399670%3A48.994369%3AWGS84%5BDD.ddddd%5D") !=
                std::string::npos &&
            arguments().find("&radius_1=1000") != std::string::npos,
        "Stop lookup did not encode longitude/latitude with a bounded radius");

    // Invalid coordinates and IDs must be rejected before starting the transport.
    std::filesystem::remove(directory.path / "curl.arguments");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const auto point : {kvv::GeoPoint{nan, 8}, kvv::GeoPoint{49, infinity},
             kvv::GeoPoint{90.1, 8}, kvv::GeoPoint{49, -180.1}}) {
        bool rejected = false;
        try {
            kvv::FindNearestStop(point);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "Invalid WGS84 lookup coordinates were accepted");
    }
    for (const char* id :
        {"", "de:08212:89", "7000089&limit=1", "7000089'; false; '", " 7000089"}) {
        bool rejected = false;
        try {
            kvv::DownloadDepartureResponse(id);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "Departure query accepted an invalid numeric stop ID");
    }
    Require(!std::filesystem::exists(directory.path / "curl.arguments"),
        "Invalid query arguments started a network request");

    for (const std::string& payload : {std::string("not JSON"), std::string("{}"),
             Json{{"locations",
                      Json::array(
                          {location("7000100", "Bus only", center.latitude, center.longitude, 5),
                              location("7000090", "Outside radius", 49.1, 8.399670)})}}
                 .dump()}) {
        WriteTransportFile(directory.path / "curl.response", payload);
        bool rejected = false;
        try {
            kvv::FindNearestStop(center);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Require(rejected, "Malformed or empty nearby-stop response selected a station");
    }

    WriteTransportFile(directory.path / "curl.response",
        "HTTP/2 200\r\nDate: Tue, 13 Oct 2026 12:00:37 GMT\r\n\r\n" + baseline.dump());
    const std::string response = kvv::DownloadDepartureResponse(stop.id);
    Require(arguments().find("type_dm=stopID&name_dm=7000089&") != std::string::npos &&
            kvv::ParseDepartureResponse(response).server_time == "2026-10-13T12:00:37Z",
        "Selected station did not reach the departure request or HTTP seconds were lost");

    WriteTransportFile(directory.path / "replay.json", response);
    std::filesystem::remove(directory.path / "curl.arguments");
    ScopedEnvironment replay("KVV_EFA_RESPONSE_FILE", (directory.path / "replay.json").string());
    Require(kvv::DownloadDepartureResponse("") == response &&
            !std::filesystem::exists(directory.path / "curl.arguments"),
        "Replay required a station ID or performed stop lookup/departure networking");
}

static void CheckDownloadClock(const Json& baseline) {
    TransportDirectory directory;
    const char* original_path = std::getenv("PATH");
    ScopedEnvironment search_path(
        "PATH", directory.path.string() + ":" + (original_path ? original_path : "/usr/bin:/bin"));
    ScopedEnvironment response_file("KVV_EFA_RESPONSE_FILE", std::nullopt);
    Json raw = baseline;
    raw.erase("parameters");
    const auto prepare = [&](const std::string& headers, int status = 0) {
        WriteTransportFile(directory.path / "curl.response", headers + raw.dump());
        // Require --include so this fixture also checks that headers are requested.
        WriteTransportFile(directory.path / "curl",
            "#!/bin/sh\n: > \"$0.called\"\n"
            "for arg do\n"
            "  if [ \"$arg\" = --include ]; then\n"
            "    /bin/cat \"$0.response\"\n"
            "    exit " +
                std::to_string(status) +
                "\n"
                "  fi\ndone\nexit 2\n");
        std::filesystem::permissions(directory.path / "curl", std::filesystem::perms::owner_all);
    };
    const auto require_rejected = [&] {
        bool rejected = false;
        try {
            kvv::DownloadDepartureResponse("7000089");
        } catch (const std::exception&) {
            rejected = true;
        }
        Require(rejected, "Download accepted a missing clock or failed curl request");
    };
    std::string stamped;
    for (const char* name : {"date", "dAtE"}) {
        prepare(
            std::string("HTTP/1.1 200 OK\r\n") + name + ": Tue, 13 Oct 2026 12:00:37 GMT\r\n\r\n");
        stamped = kvv::DownloadDepartureResponse("7000089");
        Require(kvv::ParseDepartureResponse(stamped).server_time == "2026-10-13T12:00:37Z" &&
                Json::parse(stamped)["stopEvents"] == raw["stopEvents"],
            "HTTP Date did not preserve UTC seconds and the RapidJSON payload");
    }
    const std::string preliminary = "HTTP/1.1 200 Connection established\r\n"
                                    "Date: Tue, 13 Oct 2026 11:59:00 GMT\r\n\r\n"
                                    "HTTP/1.1 100 Continue\r\n"
                                    "Date: Tue, 13 Oct 2026 12:00:00 GMT\r\n\r\n";
    const std::string final_headers = "HTTP/2 200\r\nDate: Tue, 13 Oct 2026 12:00:37 GMT\r\n\r\n";
    prepare(preliminary + final_headers);
    Require(kvv::ParseDepartureResponse(kvv::DownloadDepartureResponse("7000089")).server_time ==
            "2026-10-13T12:00:37Z",
        "Download used a proxy or informational Date instead of the final response");
    for (const char* invalid : {"HTTP/2 200\r\n\r\n", "HTTP/2 200\r\nDate: invalid\r\n\r\n"}) {
        prepare(preliminary + invalid);
        require_rejected();
    }
    prepare(final_headers, 7);
    require_rejected();

    WriteTransportFile(directory.path / "replay.json", stamped);
    std::filesystem::remove(directory.path / "curl.called");
    ScopedEnvironment replay("KVV_EFA_RESPONSE_FILE", (directory.path / "replay.json").string());
    Require(kvv::DownloadDepartureResponse("") == stamped &&
            !std::filesystem::exists(directory.path / "curl.called"),
        "Replay invoked curl or changed the saved provider clock");
}

static void CheckRefreshCadence(const Json& response) {
    // A fake clock verifies cadence without waiting five seconds per test.
    std::atomic<int> requests{0};
    kvv::TransitRefresh refresh([&]() {
        if (++requests == 2) {
            throw std::runtime_error("test outage");
        }
        return response.dump();
    });
    const auto start = Clock::now();
    const auto complete = [&](Clock::time_point time) {
        for (int i = 0; i < 2000; ++i) {
            if (refresh.Poll(time)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Refresh worker did not complete");
    };

    // Initial and early polls must make exactly one request.
    complete(start);
    Require(requests == 1 && refresh.Snapshot(start).has_value(), "Initial fetch missing");
    refresh.Poll(start + std::chrono::seconds(4));
    Require(requests == 1, "Refresh ran before five seconds");

    // A failed request preserves the last successful reading and marks it stale.
    complete(start + std::chrono::seconds(5));
    Require(requests == 2 && refresh.Snapshot(start + std::chrono::seconds(5))->stale,
        "Failed refresh not marked stale");
    Require(refresh.Snapshot(start + std::chrono::seconds(5))->server_time ==
            response["parameters"][0]["value"].get<std::string>(),
        "Failure discarded the successful reading");

    // Recovery does not make an unchanged provider timestamp fresh again.
    complete(start + std::chrono::seconds(10));
    Require(requests == 3 && refresh.Error().empty(), "Refresh did not recover automatically");
    Require(refresh.Snapshot(start + std::chrono::seconds(16))->stale,
        "Repeated old server timestamp appeared fresh");
}

static void CheckSingleRequest(const Json& response) {
    const auto start = Clock::now();

    // An in-flight request must neither block Poll nor start duplicate requests.
    std::promise<std::string> delayed;
    auto result = delayed.get_future();
    std::atomic<int> slow_requests{0};
    kvv::TransitRefresh slow([&]() {
        ++slow_requests;
        return result.get();
    });
    slow.Poll(start);
    for (int i = 0; i < 2000 && slow_requests == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    slow.Poll(start + std::chrono::seconds(5));
    slow.Poll(start + std::chrono::seconds(10));
    const bool single = slow_requests == 1;
    delayed.set_value(response.dump());
    Require(single, "Overlapping network requests were started");
}

// Viewport filtering belongs to the consumer, so all provider stops are retained.
static void CheckStopCoverage(const Json& response) {
    const auto all_stops = kvv::ParseDepartureResponse(response.dump());
    Require(std::any_of(all_stops.stops.begin(), all_stops.stops.end(),
                [](const auto& stop) {
                    return stop.id == "outside";
                }),
        "EFA library filtered stops using application viewport bounds");
}

// Replay must not advance vehicle estimates with the local clock.
static void CheckReplay(const Json& moving) {
    const auto start = Clock::now();

    kvv::TransitRefresh replay(
        [&] {
            return moving.dump();
        },
        true);
    for (int i = 0; i < 2000 && !replay.Poll(start); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto replayed = replay.Snapshot(start + std::chrono::seconds(20));
    Require(replayed && replayed->replay && replayed->vehicles.size() == 1 &&
            replayed->vehicles.front().progress == 0.0,
        "Replay extrapolated the saved timetable");
}

// A stale provider timestamp caps motion even while the displayed age increases.
static void CheckStaleExtrapolation(const Json& moving) {
    const auto start = Clock::now();

    kvv::TransitRefresh moving_refresh([&] {
        return moving.dump();
    });
    for (int i = 0; i < 2000 && !moving_refresh.Poll(start); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stale = moving_refresh.Snapshot(start + std::chrono::seconds(60));
    const auto capped = moving_refresh.Snapshot(start + std::chrono::seconds(30));
    Require(stale && capped && stale->stale && stale->vehicles.size() == 1 &&
            capped->vehicles.size() == 1 &&
            std::abs(capped->vehicles.front().progress - 0.25) < 1e-9 &&
            stale->vehicles.front().progress == capped->vehicles.front().progress,
        "Extrapolation did not stop after thirty seconds");
}

static Clock::time_point MotionNow(double seconds) {
    return Clock::time_point{} +
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

static std::string MotionTimestamp(int seconds) {
    const int clock = 12 * 3600 + seconds;
    char timestamp[32];
    std::snprintf(timestamp, sizeof(timestamp), "2026-10-13T%02d:%02d:%02dZ", clock / 3600,
        (clock / 60) % 60, clock % 60);
    return timestamp;
}

static void ShiftMotionPrediction(Json& event, int delay) {
    event["departureTimeEstimated"] = MotionTimestamp(delay);
    event["onwardLocations"][0]["departureTimeEstimated"] = MotionTimestamp(240 + delay);
}

// Coordinates lie on EFA's six-decimal grid so timestamp expectations are not
// distorted by coordinate stringification in the provider normalization.
static void SetMotionLocation(Json& location, int step) {
    location["coord"] = Json::array({49.006250 + step * 0.001, 8.431785 + step * 0.001});
}

static Json MakeMotionResponse() {
    Json feed = MakeRapidResponse();
    feed["parameters"][0]["value"] = MotionTimestamp(120);
    auto& event = feed["stopEvents"][0];
    event["previousLocations"] = Json::array();
    event["onwardLocations"][0]["departureTimePlanned"] = MotionTimestamp(240);
    ShiftMotionPrediction(event, 0);
    SetMotionLocation(event["location"], 0);
    SetMotionLocation(event["onwardLocations"][0], 1);
    return feed;
}

// Measure actual position along the straight fixture route, including subsequent
// segments. Segment progress alone resets at each stop and cannot detect jumps.
static std::vector<double> MotionPositions(const kvv::TransitSnapshot& snapshot) {
    std::vector<double> positions;
    for (const auto& vehicle : snapshot.vehicles) {
        const double latitude = vehicle.from.latitude +
            (vehicle.to.latitude - vehicle.from.latitude) * vehicle.progress;
        positions.push_back((latitude - 49.006250) / 0.001);
    }
    std::sort(positions.begin(), positions.end());
    return positions;
}

static double MotionPosition(const kvv::TransitSnapshot& snapshot) {
    const auto positions = MotionPositions(snapshot);
    Require(positions.size() == 1, "Expected one continuously displayed train");
    return positions.front();
}

static void RequireSamePosition(double actual, double expected, const char* message) {
    Require(std::abs(actual - expected) < 1e-8, message);
}

static void CheckMotionCorrections() {
    for (const int delay : {-30, 30, 180}) {
        Json feed = MakeMotionResponse();
        kvv::TransitSession session;
        session.Update(feed.dump());
        session.SmoothedView(0, MotionNow(0));
        const double before = MotionPosition(session.SmoothedView(5, MotionNow(5)));
        feed["parameters"][0]["value"] = MotionTimestamp(125);
        ShiftMotionPrediction(feed["stopEvents"][0], delay);
        session.Update(feed.dump());
        const double after = MotionPosition(session.SmoothedView(0, MotionNow(5)));
        RequireSamePosition(after, before, "A revised prediction teleported the train");
        const double next = MotionPosition(session.SmoothedView(1, MotionNow(6)));
        const double advance = next - after;
        Require(advance > 0 && (delay < 0 ? advance > 1.0 / 240 : advance < 1.0 / 240),
            "Prediction correction did not speed up or slow down forward motion");
        double previous = next;
        for (int seconds = 2; seconds <= 20; ++seconds) {
            const double position =
                MotionPosition(session.SmoothedView(seconds, MotionNow(5 + seconds)));
            Require(position >= previous, "A corrected train moved backwards");
            previous = position;
        }
        const double initial_error = std::abs(after - (125.0 - delay) / 240);
        Require(std::abs(previous - (145.0 - delay) / 240) < initial_error,
            "Speed correction did not reduce the prediction error");
    }
}

static void CheckMotionCadence() {
    Json feed = MakeMotionResponse();
    kvv::TransitSession dense;
    kvv::TransitSession sparse;
    dense.Update(feed.dump());
    sparse.Update(feed.dump());
    dense.SmoothedView(0, MotionNow(0));
    sparse.SmoothedView(0, MotionNow(0));
    for (int frame = 1; frame < 20; ++frame) {
        dense.SmoothedView(frame / 4.0, MotionNow(frame / 4.0));
    }
    feed["parameters"][0]["value"] = MotionTimestamp(125);
    ShiftMotionPrediction(feed["stopEvents"][0], -30);
    dense.Update(feed.dump());
    sparse.Update(feed.dump());
    const double anchor = MotionPosition(dense.SmoothedView(0, MotionNow(5)));
    RequireSamePosition(MotionPosition(sparse.SmoothedView(0, MotionNow(5))), anchor,
        "Skipped frames changed the prediction correction anchor");
    for (int frame = 21; frame <= 40; ++frame) {
        dense.SmoothedView(frame / 4.0 - 5, MotionNow(frame / 4.0));
    }
    const double before = MotionPosition(dense.SmoothedView(5, MotionNow(10)));
    feed["parameters"][0]["value"] = MotionTimestamp(130);
    dense.Update(feed.dump());
    RequireSamePosition(MotionPosition(dense.SmoothedView(0, MotionNow(10))), before,
        "An unchanged prediction with a new server time changed position");
    for (int frame = 41; frame <= 60; ++frame) {
        dense.SmoothedView(frame / 4.0 - 10, MotionNow(frame / 4.0));
    }
    const double dense_position = MotionPosition(dense.SmoothedView(5, MotionNow(15)));
    RequireSamePosition(MotionPosition(sparse.SmoothedView(10, MotionNow(15))), dense_position,
        "Motion depended on redraw frequency or harmless refreshes");
    dense.Update(feed.dump());
    RequireSamePosition(MotionPosition(dense.SmoothedView(5, MotionNow(15))), dense_position,
        "Repeated snapshots or cached responses advanced motion twice");
    ShiftMotionPrediction(feed["stopEvents"][0], 30);
    dense.Update(feed.dump());
    RequireSamePosition(MotionPosition(dense.SmoothedView(5, MotionNow(15))), dense_position,
        "A second correction interrupted the first with a jump");
}

static Json MakeMotionDwellResponse() {
    Json feed = MakeMotionResponse();
    feed["parameters"][0]["value"] = MotionTimestamp(110);
    auto& event = feed["stopEvents"][0];
    auto& next = event["onwardLocations"][0];
    next["arrivalTimePlanned"] = next["arrivalTimeEstimated"] = MotionTimestamp(120);
    next["departureTimePlanned"] = next["departureTimeEstimated"] = MotionTimestamp(125);
    event["onwardLocations"].push_back(RapidLocation(
        "after", 200, 200, MotionTimestamp(145).c_str(), MotionTimestamp(145).c_str()));
    SetMotionLocation(event["onwardLocations"][1], 2);
    return feed;
}

static void CheckMotionDwell() {
    const Json feed = MakeMotionDwellResponse();
    kvv::TransitSession session;
    session.Update(feed.dump());
    RequireSamePosition(MotionPosition(session.SmoothedView(0, MotionNow(0))), 110.0 / 120,
        "Train snapped to a stop before its exact arrival");
    RequireSamePosition(MotionPosition(session.SmoothedView(9, MotionNow(9))), 119.0 / 120,
        "Approaching a stop skipped the final route distance");
    for (const double seconds : {10.0, 12.0, 15.0}) {
        RequireSamePosition(MotionPosition(session.SmoothedView(seconds, MotionNow(seconds))), 1,
            "Train moved through its scheduled dwell");
    }
    RequireSamePosition(MotionPosition(session.SmoothedView(15.1, MotionNow(15.1))), 1.005,
        "Leaving a stop jumped or used the previous segment's progress");
    RequireSamePosition(MotionPosition(session.SmoothedView(20, MotionNow(20))), 1.25,
        "Train did not continue on the next route segment");

    kvv::TransitSession delayed;
    delayed.Update(feed.dump());
    delayed.SmoothedView(0, MotionNow(0));
    const double dwelling = MotionPosition(delayed.SmoothedView(12, MotionNow(12)));
    Json revised = feed;
    revised["parameters"][0]["value"] = MotionTimestamp(122);
    revised["stopEvents"][0]["onwardLocations"][0]["departureTimeEstimated"] = MotionTimestamp(135);
    revised["stopEvents"][0]["onwardLocations"][1]["departureTimeEstimated"] = MotionTimestamp(155);
    delayed.Update(revised.dump());
    RequireSamePosition(MotionPosition(delayed.SmoothedView(0, MotionNow(12))), dwelling,
        "A revised departure moved a dwelling train");
    RequireSamePosition(MotionPosition(delayed.SmoothedView(12, MotionNow(24))), 1,
        "Train left before its revised departure");
    Require(MotionPosition(delayed.SmoothedView(14, MotionNow(26))) > 1,
        "Train never resumed after its revised departure");
}

static void CheckMotionStaleAndOverlap() {
    Json feed = MakeMotionResponse();
    kvv::TransitSession session;
    session.Update(feed.dump());
    session.SmoothedView(0, MotionNow(0));
    feed["parameters"][0]["value"] = MotionTimestamp(125);
    ShiftMotionPrediction(feed["stopEvents"][0], -30);
    session.Update(feed.dump());
    session.SmoothedView(0, MotionNow(5));
    const double capped = MotionPosition(session.SmoothedView(30, MotionNow(35)));
    RequireSamePosition(MotionPosition(session.SmoothedView(60, MotionNow(65))), capped,
        "Speed correction kept moving after the stale extrapolation limit");
    session.Update(feed.dump());
    RequireSamePosition(MotionPosition(session.SmoothedView(65, MotionNow(70))), capped,
        "Cached predictions restarted stale motion");

    Json overlap = MakeRapidResponse();
    SetMotionLocation(overlap["stopEvents"][0]["location"], 0);
    SetMotionLocation(overlap["stopEvents"][0]["onwardLocations"][0], 1);
    SetMotionLocation(overlap["stopEvents"][0]["previousLocations"][0], -1);
    overlap["parameters"][0]["value"] = MotionTimestamp(90);
    kvv::TransitSession ambiguous;
    ambiguous.Update(overlap.dump());
    const double before = MotionPosition(ambiguous.SmoothedView(0, MotionNow(0)));
    auto& previous = overlap["stopEvents"][0]["previousLocations"][0];
    previous["arrivalTimeEstimated"] = MotionTimestamp(-90);
    previous["departureTimeEstimated"] = MotionTimestamp(300);
    ambiguous.Update(overlap.dump());
    RequireSamePosition(MotionPosition(ambiguous.SmoothedView(0, MotionNow(0))), before,
        "An overlapping earlier dwell teleported a train backwards");
    Require(MotionPosition(ambiguous.SmoothedView(1, MotionNow(1))) > before,
        "An inconsistent revision stopped the last valid forward path");
}

static void CheckMotionTripIdentity() {
    Json feed = MakeMotionResponse();
    Json second = feed["stopEvents"][0];
    second["transportation"]["properties"]["tripCode"] = 43;
    ShiftMotionPrediction(second, 60);
    feed["stopEvents"].push_back(second);
    kvv::TransitSession session;
    session.Update(feed.dump());
    session.SmoothedView(0, MotionNow(0));
    const auto before = MotionPositions(session.SmoothedView(5, MotionNow(5)));
    Require(before.size() == 2, "Same-line trains were merged before smoothing");
    feed["parameters"][0]["value"] = MotionTimestamp(125);
    ShiftMotionPrediction(feed["stopEvents"][0], -30);
    std::reverse(feed["stopEvents"].begin(), feed["stopEvents"].end());
    session.Update(feed.dump());
    const auto after = MotionPositions(session.SmoothedView(0, MotionNow(5)));
    Require(after.size() == 2, "A reordered response lost a same-line train");
    for (std::size_t index = 0; index < before.size(); ++index) {
        RequireSamePosition(after[index], before[index], "Same-line motion histories were swapped");
    }
    const auto advanced = MotionPositions(session.SmoothedView(5, MotionNow(10)));
    Require(advanced.size() == 2, "A correcting same-line train disappeared");
    RequireSamePosition(
        advanced[0] - after[0], 5.0 / 240, "One train's correction changed another train's speed");
    Require(advanced[1] - after[1] > 5.0 / 240, "Reordered trip lost its own speed correction");
    feed["parameters"][0]["value"] = MotionTimestamp(130);
    feed["stopEvents"][1]["realtimeStatus"] = Json::array({"CANCELLED"});
    session.Update(feed.dump());
    RequireSamePosition(MotionPosition(session.SmoothedView(0, MotionNow(10))), advanced[0],
        "Cancellation retained a ghost train or changed the surviving train");
    feed["stopEvents"][0]["realtimeStatus"] = Json::array({"CANCELLED"});
    session.Update(feed.dump());
    Require(session.SmoothedView(0, MotionNow(10)).vehicles.empty(),
        "Cancelled trains survived in the motion cache");
}

static void CompleteMotionRefresh(kvv::TransitRefresh& refresh, Clock::time_point now) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        if (refresh.Poll(now)) {
            Require(refresh.Error().empty(), "Motion integration refresh failed");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Motion integration refresh did not complete");
}

static void CheckMotionRefreshIntegration() {
    const Json initial = MakeMotionResponse();
    Json revised = initial;
    revised["parameters"][0]["value"] = MotionTimestamp(125);
    ShiftMotionPrediction(revised["stopEvents"][0], -30);
    const auto run = [&](bool draw_update) {
        std::atomic<int> requests{0};
        kvv::TransitRefresh refresh([&] {
            return ++requests == 1 ? initial.dump() : revised.dump();
        });
        CompleteMotionRefresh(refresh, MotionNow(0));
        const double before = MotionPosition(*refresh.Snapshot(MotionNow(5)));
        CompleteMotionRefresh(refresh, MotionNow(5));
        if (draw_update) {
            RequireSamePosition(MotionPosition(*refresh.Snapshot(MotionNow(5))), before,
                "Consuming a refreshed prediction jumped the displayed train");
        }
        const auto later = refresh.Snapshot(MotionNow(10));
        Require(later && !later->stale, "A fresh smoothed snapshot was marked stale");
        return MotionPosition(*later);
    };
    RequireSamePosition(run(false), run(true),
        "Skipping the update redraw deferred motion correction until the next frame");
}

int main() {
    try {
        const auto response = MakeResponse();
        const auto moving = MakeMovingResponse(response);

        CheckDepartureDelays(moving);
        CheckMissingDelays(moving);
        CheckDwellDelays(moving);
        CheckMidnightDelay(moving);
        CheckClassicSeconds(moving);

        const auto rapid = MakeRapidResponse();
        CheckRapidSeconds(rapid);
        CheckRapidDwell(rapid);
        CheckRapidFilteringAndClock(rapid);
        CheckRapidSession(rapid);
        CheckPredictionReplacement(moving);
        CheckRetentionAndCancellation(response);

        CheckDownloadClock(rapid);
        CheckNearestStopAndTransport(rapid);
        CheckRefreshCadence(response);
        CheckSingleRequest(response);
        CheckStopCoverage(response);
        CheckReplay(moving);
        CheckStaleExtrapolation(moving);
        CheckMotionCorrections();
        CheckMotionCadence();
        CheckMotionDwell();
        CheckMotionStaleAndOverlap();
        CheckMotionTripIdentity();
        CheckMotionRefreshIntegration();

        std::cout << "EFA timing, retention, cancellations, replay and refresh checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
