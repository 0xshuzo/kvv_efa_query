#include "kvv_efa_query.h"
#include "kvv_efa_constants.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <locale>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace kvv {
namespace {
using Json = nlohmann::json;

struct TimedStop {
    Stop stop;
    std::optional<long long> arrival;
    std::optional<long long> departure;
};

std::string ReadString(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) {
        return {};
    }

    // EFA sometimes encodes the same field as a number and sometimes as text.
    const Json& value = object.at(key);
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    return {};
}

const Json* ReadObject(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_object()) {
        return nullptr;
    }
    return &object.at(key);
}

std::vector<const Json*> RecordList(const Json& value) {
    // Normalize singleton records and arrays for the parsing loops below.
    std::vector<const Json*> records;

    if (value.is_array()) {
        for (const Json& item : value) {
            if (item.is_object()) {
                records.push_back(&item);
            }
        }
    } else if (value.is_object()) {
        records.push_back(&value);
    }

    return records;
}

std::vector<const Json*> ReadRecordList(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) {
        return {};
    }
    return RecordList(object.at(key));
}

std::optional<double> ParseDouble(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const double result = std::stod(text, &consumed);
        if (consumed == text.size() && std::isfinite(result)) {
            return result;
        }
    } catch (const std::exception&) {
        // Invalid numeric fields are treated as missing provider values.
    }
    return std::nullopt;
}

std::optional<long long> ParseInteger(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const long long result = std::stoll(text, &consumed);
        if (consumed == text.size()) {
            return result;
        }
    } catch (const std::exception&) {
        // Invalid numeric fields are treated as missing provider values.
    }
    return std::nullopt;
}

std::optional<GeoPoint> ParseCoordinates(const std::string& text) {
    const std::size_t comma = text.find(',');
    if (comma == std::string::npos) {
        return std::nullopt;
    }

    // EFA coordinates are longitude first, while GeoPoint stores latitude first.
    const std::optional<double> longitude = ParseDouble(text.substr(0, comma));
    const std::optional<double> latitude = ParseDouble(text.substr(comma + 1));
    if (!latitude || !longitude || std::abs(*latitude) > KVV_EFA_MAX_ABS_LATITUDE_DEGREES ||
        std::abs(*longitude) > KVV_EFA_MAX_ABS_LONGITUDE_DEGREES) {
        return std::nullopt;
    }

    return GeoPoint{*latitude, *longitude};
}

// Convert a civil clock to seconds. RapidJSON events and its HTTP clock are UTC;
// legacy JSON replays consistently use the provider's local wall time.
long long DaysFromCivil(int year, unsigned month, unsigned day) {
    // March starts the calculation year so leap days stay at its end.
    year -= month < KVV_EFA_MARCH;
    // C++ truncates toward zero; adjust negative years to get floor division.
    const int era = (year >= 0 ? year : year - (KVV_EFA_YEARS_PER_ERA - 1)) / KVV_EFA_YEARS_PER_ERA;
    const unsigned year_of_era = year - era * KVV_EFA_YEARS_PER_ERA;
    const unsigned march_based_month = month >= KVV_EFA_MARCH
        ? month - KVV_EFA_MARCH
        : month + (KVV_EFA_MONTHS_PER_YEAR - KVV_EFA_MARCH);
    const unsigned day_of_year =
        (KVV_EFA_DAYS_PER_BLOCK * march_based_month + KVV_EFA_MONTH_OFFSET_ROUNDING) /
            KVV_EFA_MONTHS_PER_BLOCK +
        day - 1;
    const unsigned day_of_era = year_of_era * KVV_EFA_DAYS_PER_COMMON_YEAR +
        year_of_era / KVV_EFA_LEAP_YEAR_INTERVAL - year_of_era / KVV_EFA_YEARS_PER_CENTURY +
        day_of_year;

    return era * KVV_EFA_DAYS_PER_ERA + day_of_era - KVV_EFA_UNIX_EPOCH_OFFSET_DAYS;
}

bool IsYearInRange(long long year) {
    return year >= 0 && year <= KVV_EFA_MAX_TIMESTAMP_YEAR;
}

bool IsMonthInRange(long long month) {
    return month >= 1 && month <= KVV_EFA_MONTHS_PER_YEAR;
}

bool IsDayOfMonthInRange(long long day) {
    return day >= 1 && day <= KVV_EFA_MAX_DAYS_PER_MONTH;
}

bool IsHourInRange(long long hour) {
    return hour >= 0 && hour < KVV_EFA_HOURS_PER_DAY;
}

bool IsMinuteInRange(long long minute) {
    return minute >= 0 && minute < KVV_EFA_MINUTES_PER_HOUR;
}

bool IsSecondInRange(long long second) {
    return second >= 0 && second < KVV_EFA_SECONDS_PER_MINUTE;
}

std::optional<long long> CivilSeconds(
    int year, int month, int day, int hour, int minute, int second = 0) {
    if (!IsMonthInRange(month) || !IsDayOfMonthInRange(day) || !IsHourInRange(hour) ||
        !IsMinuteInRange(minute) || !IsSecondInRange(second)) {
        return std::nullopt;
    }

    return DaysFromCivil(year, month, day) * KVV_EFA_SECONDS_PER_DAY +
        hour * KVV_EFA_SECONDS_PER_HOUR + minute * KVV_EFA_SECONDS_PER_MINUTE + second;
}

std::optional<long long> ParseEfaSequenceTime(const std::string& text) {
    // EFA stop sequences use YYYYMMDD HH:MM[:SS].
    if (text.size() != KVV_EFA_SEQUENCE_MINUTE_LENGTH &&
        text.size() != KVV_EFA_SEQUENCE_SECOND_LENGTH) {
        return std::nullopt;
    }
    if (text[KVV_EFA_SEQUENCE_DATE_SEPARATOR_OFFSET] != ' ' ||
        text[KVV_EFA_SEQUENCE_HOUR_SEPARATOR_OFFSET] != ':' ||
        (text.size() == KVV_EFA_SEQUENCE_SECOND_LENGTH &&
            text[KVV_EFA_SEQUENCE_MINUTE_SEPARATOR_OFFSET] != ':')) {
        return std::nullopt;
    }

    try {
        return CivilSeconds(
            std::stoi(text.substr(KVV_EFA_TIMESTAMP_YEAR_OFFSET, KVV_EFA_TIMESTAMP_YEAR_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SEQUENCE_MONTH_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SEQUENCE_DAY_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SEQUENCE_HOUR_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SEQUENCE_MINUTE_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            text.size() == KVV_EFA_SEQUENCE_SECOND_LENGTH
                ? std::stoi(
                      text.substr(KVV_EFA_SEQUENCE_SECOND_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH))
                : 0);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<long long> ParseEfaDateTime(const Json& date_time) {
    const std::optional<long long> year = ParseInteger(ReadString(date_time, "year"));
    const std::optional<long long> month = ParseInteger(ReadString(date_time, "month"));
    const std::optional<long long> day = ParseInteger(ReadString(date_time, "day"));
    const std::optional<long long> hour = ParseInteger(ReadString(date_time, "hour"));
    const std::optional<long long> minute = ParseInteger(ReadString(date_time, "minute"));

    const std::optional<long long> second = date_time.contains("second")
        ? ParseInteger(ReadString(date_time, "second"))
        : std::optional<long long>{0};

    // Validate before narrowing provider integers to CivilSeconds' int fields.
    if (!year || !IsYearInRange(*year) || !month || !IsMonthInRange(*month) || !day ||
        !IsDayOfMonthInRange(*day) || !hour || !IsHourInRange(*hour) || !minute ||
        !IsMinuteInRange(*minute) || !second || !IsSecondInRange(*second)) {
        return std::nullopt;
    }

    return CivilSeconds(*year, *month, *day, *hour, *minute, *second);
}

std::optional<long long> ParseServerTime(const std::string& text) {
    // serverTime uses UTC for RapidJSON, or local civil time for legacy feeds.
    // Each response's event timestamps use the same clock basis.
    if (text.size() < KVV_EFA_SERVER_MINUTE_LENGTH ||
        text[KVV_EFA_SERVER_YEAR_SEPARATOR_OFFSET] != '-' ||
        text[KVV_EFA_SERVER_MONTH_SEPARATOR_OFFSET] != '-' ||
        text[KVV_EFA_SERVER_DATE_SEPARATOR_OFFSET] != 'T' ||
        text[KVV_EFA_SERVER_HOUR_SEPARATOR_OFFSET] != ':') {
        return std::nullopt;
    }

    try {
        const int second = text.size() >= KVV_EFA_SERVER_SECOND_LENGTH &&
                text[KVV_EFA_SERVER_MINUTE_SEPARATOR_OFFSET] == ':'
            ? std::stoi(text.substr(KVV_EFA_SERVER_SECOND_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH))
            : 0;
        return CivilSeconds(
            std::stoi(text.substr(KVV_EFA_TIMESTAMP_YEAR_OFFSET, KVV_EFA_TIMESTAMP_YEAR_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SERVER_MONTH_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SERVER_DAY_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SERVER_HOUR_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            std::stoi(text.substr(KVV_EFA_SERVER_MINUTE_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)),
            second);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<long long> ApplyDelay(
    std::optional<long long> planned, const Json& object, const char* delay_key) {
    if (!planned) {
        return std::nullopt;
    }

    const auto delay = ParseInteger(ReadString(object, delay_key));
    // EFA delays are signed minutes; -9999 means no prediction, not early running.
    if (!delay || *delay == KVV_EFA_UNKNOWN_DELAY_MINUTES) {
        return planned;
    }

    // Reject arithmetic overflow before converting minutes and adding the delay.
    if (*delay < KVV_EFA_MIN_DELAY_MINUTES || *delay > KVV_EFA_MAX_DELAY_MINUTES) {
        return planned;
    }

    const auto seconds = *delay * KVV_EFA_SECONDS_PER_MINUTE;
    if ((seconds > 0 && *planned > KVV_EFA_MAX_TIMESTAMP_SECONDS - seconds) ||
        (seconds < 0 && *planned < KVV_EFA_MIN_TIMESTAMP_SECONDS - seconds)) {
        return planned;
    }

    return *planned + seconds;
}

std::optional<long long> ParseReferenceTime(const Json& reference, bool is_departure) {
    // Prefer second-resolution event times, then fall back to minute precision.
    auto planned = ParseEfaSequenceTime(
        ReadString(reference, is_departure ? "depDateTimeSec" : "arrDateTimeSec"));
    if (!planned) {
        planned = ParseEfaSequenceTime(
            ReadString(reference, is_departure ? "depDateTime" : "arrDateTime"));
    }

    // An explicitly invalid prediction must leave the scheduled time unchanged.
    const char* const delay_key = is_departure ? "depDelay" : "arrDelay";
    const char* const valid_key = is_departure ? "depValid" : "arrValid";
    return ReadString(reference, valid_key) == KVV_EFA_INVALID_PREDICTION_FLAG
        ? planned
        : ApplyDelay(planned, reference, delay_key);
}

std::optional<TimedStop> ParseSequenceStop(const Json& entry) {
    const Json* const reference = ReadObject(entry, "ref");
    if (reference == nullptr) {
        return std::nullopt;
    }

    const std::optional<GeoPoint> point = ParseCoordinates(ReadString(*reference, "coords"));
    if (!point) {
        return std::nullopt;
    }

    TimedStop call;
    call.stop.id = ReadString(*reference, "id");
    call.stop.name = ReadString(entry, "nameWO");
    if (call.stop.name.empty()) {
        call.stop.name = ReadString(entry, "name");
    }
    call.stop.position = *point;

    call.arrival = ParseReferenceTime(*reference, false);
    call.departure = ParseReferenceTime(*reference, true);

    // Some calls expose only one event. Do not invent a dwell duration.
    if (!call.arrival) {
        call.arrival = call.departure;
    }
    if (!call.departure) {
        call.departure = call.arrival;
    }

    return call;
}

std::optional<TimedStop> ParseCurrentStop(const Json& entry) {
    const std::optional<double> longitude = ParseDouble(ReadString(entry, "x"));
    const std::optional<double> latitude = ParseDouble(ReadString(entry, "y"));
    if (!longitude || !latitude) {
        return std::nullopt;
    }

    TimedStop call;
    call.stop.id = ReadString(entry, "stopID");
    call.stop.name = ReadString(entry, "nameWO");
    call.stop.position = GeoPoint{*latitude, *longitude};

    const Json* const real_time = ReadObject(entry, "realDateTime");
    const Json* const scheduled_time = ReadObject(entry, "dateTime");

    // A valid absolute prediction wins: applying delay again would count it twice.
    if (real_time) {
        call.departure = ParseEfaDateTime(*real_time);
    }
    if (!call.departure && scheduled_time) {
        call.departure = ParseEfaDateTime(*scheduled_time);
        if (const Json* const line = ReadObject(entry, "servingLine")) {
            call.departure = ApplyDelay(call.departure, *line, "delay");
        }
    }

    // The departure-board row carries no arrival time.
    call.arrival = call.departure;

    return call;
}

std::string StopKey(const Stop& stop) {
    // A station ID covers several platforms. Keep their distinct coordinates.
    return stop.id + ":" +
        std::to_string(
            std::llround(stop.position.latitude * KVV_EFA_COORDINATE_KEY_UNITS_PER_DEGREE)) +
        ":" +
        std::to_string(
            std::llround(stop.position.longitude * KVV_EFA_COORDINATE_KEY_UNITS_PER_DEGREE));
}

struct JourneyProgress {
    std::size_t from;
    std::size_t to;
    double fraction;
};

std::optional<JourneyProgress> FindJourneyProgress(
    const std::vector<TimedStop>& calls, double now, double dwell_tolerance) {
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const TimedStop& call = calls[index];
        if (call.arrival && call.departure && *call.departure >= *call.arrival &&
            now >= *call.arrival - dwell_tolerance && now <= *call.departure + dwell_tolerance) {
            return JourneyProgress{index, index, 0.0};
        }
    }

    // Between calls, express progress from departure to the next arrival.
    for (std::size_t index = 0; index + 1 < calls.size(); ++index) {
        const TimedStop& from = calls[index];
        const TimedStop& to = calls[index + 1];
        if (!from.departure || !to.arrival || *to.arrival <= *from.departure ||
            now < *from.departure || now > *to.arrival) {
            continue;
        }
        return JourneyProgress{
            index, index + 1, (now - *from.departure) / double(*to.arrival - *from.departure)};
    }
    return std::nullopt;
}

std::optional<VehicleEstimate> EstimateVehiclePosition(const std::vector<TimedStop>& calls,
    double now, double dwell_tolerance = KVV_EFA_DWELL_TOLERANCE_SECONDS) {
    const auto position = FindJourneyProgress(calls, now, dwell_tolerance);
    if (!position) {
        return std::nullopt;
    }
    return VehicleEstimate{"", "", calls[position->from].stop.position,
        calls[position->to].stop.position, position->fraction};
}

// Integrate a moving target clock analytically. Outside the speed limits the
// correction is linear; inside them its error decays exponentially. Anchoring
// this calculation makes the result independent of how often frames are drawn.
double AdvanceMotionClock(double error, double elapsed) {
    double advance = 0.0;
    const double fast_error = KVV_EFA_MOTION_CORRECTION_SECONDS * (KVV_EFA_MOTION_MAX_RATE - 1.0);
    const double slow_error = KVV_EFA_MOTION_CORRECTION_SECONDS * (KVV_EFA_MOTION_MIN_RATE - 1.0);
    if (error > fast_error) {
        const double capped =
            std::min(elapsed, (error - fast_error) / (KVV_EFA_MOTION_MAX_RATE - 1.0));
        advance += capped * KVV_EFA_MOTION_MAX_RATE;
        error -= capped * (KVV_EFA_MOTION_MAX_RATE - 1.0);
        elapsed -= capped;
    } else if (error < slow_error) {
        const double capped =
            std::min(elapsed, (slow_error - error) / (1.0 - KVV_EFA_MOTION_MIN_RATE));
        advance += capped * KVV_EFA_MOTION_MIN_RATE;
        error += capped * (1.0 - KVV_EFA_MOTION_MIN_RATE);
        elapsed -= capped;
    }
    return advance + elapsed - error * std::expm1(-elapsed / KVV_EFA_MOTION_CORRECTION_SECONDS);
}

std::optional<double> RebaseMotionClock(const std::vector<TimedStop>& previous,
    const JourneyProgress& position, const std::vector<TimedStop>& revised, double target) {
    const std::string from_key = StopKey(previous[position.from].stop);
    const std::string to_key = StopKey(previous[position.to].stop);
    const bool dwelling = position.from == position.to;
    std::optional<std::size_t> matched;
    for (std::size_t index = 0; index < revised.size(); ++index) {
        if (StopKey(revised[index].stop) != from_key ||
            (!dwelling &&
                (index + 1 >= revised.size() || StopKey(revised[index + 1].stop) != to_key))) {
            continue;
        }
        if (matched) {
            // A loop can visit the same platform more than once. Keep the old
            // timeline when the stop occurrence cannot be identified safely.
            return std::nullopt;
        }
        matched = index;
    }
    if (!matched) {
        return std::nullopt;
    }
    const TimedStop& from = revised[*matched];
    double anchor = 0.0;
    if (dwelling) {
        if (!from.arrival || !from.departure || *from.departure < *from.arrival) {
            return std::nullopt;
        }
        anchor = std::clamp(target, double(*from.arrival), double(*from.departure));
    } else {
        const TimedStop& to = revised[*matched + 1];
        if (!from.departure || !to.arrival || *to.arrival <= *from.departure) {
            return std::nullopt;
        }
        anchor = *from.departure + position.fraction * double(*to.arrival - *from.departure);
    }
    // Partial realtime updates can leave overlapping stop intervals. Accept the
    // rebase only when sampling it returns the same route position, rather than
    // an earlier overlapping dwell selected by the sampler.
    const auto sampled = FindJourneyProgress(revised, anchor, 0.0);
    const std::size_t expected_to = dwelling ? *matched : *matched + 1;
    if (!sampled || sampled->from != *matched || sampled->to != expected_to ||
        std::abs(sampled->fraction - position.fraction) > KVV_EFA_MOTION_FRACTION_EPSILON) {
        return std::nullopt;
    }
    return anchor;
}

bool JourneyFinished(const std::vector<TimedStop>& calls, double time) {
    return !calls.empty() && calls.back().departure && time > *calls.back().departure;
}

std::string FindServerTime(const Json& response) {
    for (const Json* const parameter : ReadRecordList(response, "parameters")) {
        if (ReadString(*parameter, "name") == KVV_EFA_SERVER_TIME_PARAMETER) {
            return ReadString(*parameter, "value");
        }
    }
    return {};
}

std::string FindTripId(const Json& entry) {
    for (const Json* const attribute : ReadRecordList(entry, "attrs")) {
        if (ReadString(*attribute, "name") == KVV_EFA_REALTIME_TRIP_ID_ATTRIBUTE) {
            return ReadString(*attribute, "value");
        }
    }
    return {};
}

std::string TripKey(const Json& entry) {
    const Json* const line = ReadObject(entry, "servingLine");
    const Json* const date = ReadObject(entry, "dateTime");
    if (!line || !date) {
        return {};
    }

    // Prefer the realtime ID; older responses need a line and departure fallback.
    std::string identity = FindTripId(entry);
    if (identity.empty()) {
        identity = ReadString(*line, "stateless") + ":" + ReadString(*line, "key");
    }
    if (ReadString(*line, "key").empty() && FindTripId(entry).empty()) {
        identity += ":" + ReadString(*date, "hour") + ":" + ReadString(*date, "minute") + ":" +
            std::to_string(ParseInteger(ReadString(*date, "second")).value_or(0));
    }

    // Scope identifiers to a service date so trips on different days stay separate.
    return ReadString(*date, "year") + "-" + ReadString(*date, "month") + "-" +
        ReadString(*date, "day") + ":" + identity;
}

bool Cancelled(const Json& entry) {
    return ReadString(entry, "realtimeStatus").find(KVV_EFA_CANCELLED_STATUS_TOKEN) !=
        std::string::npos ||
        ReadString(entry, "realtimeTripStatus").find(KVV_EFA_CANCELLED_STATUS_TOKEN) !=
        std::string::npos;
}

const Json* ActiveRailLine(const Json& entry) {
    const Json* const line = ReadObject(entry, "servingLine");
    if (!line || Cancelled(entry) || Cancelled(*line)) {
        return nullptr;
    }
    const std::string mode = ReadString(*line, "motType");
    return mode == KVV_EFA_MODE_RAIL || mode == KVV_EFA_MODE_TRAM ? line : nullptr;
}

void SetVehicleService(VehicleEstimate& vehicle, const Json& line) {
    vehicle.line = ReadString(line, "symbol");
    if (vehicle.line.empty()) {
        vehicle.line = ReadString(line, "number");
    }
    vehicle.destination = ReadString(line, "direction");
}

// RapidJSON guarantees UTC ISO 8601 timestamps. Do not silently interpret an
// unexpected suffix as UTC or accept partial numeric fields from a bad estimate.
bool IsRapidTimestamp(const std::string& text) {
    if (text.size() != KVV_EFA_UTC_TIMESTAMP_LENGTH || text.back() != 'Z') {
        return false;
    }
    for (std::size_t index = 0; index < KVV_EFA_SERVER_SECOND_LENGTH; ++index) {
        if (index == KVV_EFA_SERVER_YEAR_SEPARATOR_OFFSET ||
            index == KVV_EFA_SERVER_MONTH_SEPARATOR_OFFSET) {
            if (text[index] != '-') {
                return false;
            }
        } else if (index == KVV_EFA_SERVER_DATE_SEPARATOR_OFFSET) {
            if (text[index] != 'T') {
                return false;
            }
        } else if (index == KVV_EFA_SERVER_HOUR_SEPARATOR_OFFSET ||
            index == KVV_EFA_SERVER_MINUTE_SEPARATOR_OFFSET) {
            if (text[index] != ':') {
                return false;
            }
        } else if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return ParseServerTime(text).has_value();
}

Json RapidDateTime(const std::string& text) {
    if (!IsRapidTimestamp(text)) {
        return Json::object();
    }
    return {{"year", text.substr(KVV_EFA_TIMESTAMP_YEAR_OFFSET, KVV_EFA_TIMESTAMP_YEAR_WIDTH)},
        {"month", text.substr(KVV_EFA_SERVER_MONTH_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)},
        {"day", text.substr(KVV_EFA_SERVER_DAY_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)},
        {"hour", text.substr(KVV_EFA_SERVER_HOUR_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)},
        {"minute", text.substr(KVV_EFA_SERVER_MINUTE_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)},
        {"second", text.substr(KVV_EFA_SERVER_SECOND_OFFSET, KVV_EFA_TIMESTAMP_FIELD_WIDTH)}};
}

std::string RapidSequenceTime(const Json& location, bool is_departure) {
    std::string time =
        ReadString(location, is_departure ? "departureTimeEstimated" : "arrivalTimeEstimated");
    if (!IsRapidTimestamp(time)) {
        time = ReadString(location, is_departure ? "departureTimePlanned" : "arrivalTimePlanned");
    }
    const Json date = RapidDateTime(time);
    if (date.empty()) {
        return {};
    }
    return ReadString(date, "year") + ReadString(date, "month") + ReadString(date, "day") + " " +
        ReadString(date, "hour") + ":" + ReadString(date, "minute") + ":" +
        ReadString(date, "second");
}

Json RapidStop(const Json& location) {
    Json stop = {{"stopID", ReadString(location, "id")}, {"nameWO", ReadString(location, "name")}};
    const Json* const parent = ReadObject(location, "parent");
    const Json* properties = ReadObject(location, "properties");
    if ((!properties || ReadString(*properties, "stopId").empty()) && parent) {
        properties = ReadObject(*parent, "properties");
    }
    if (properties && !ReadString(*properties, "stopId").empty()) {
        stop["stopID"] = ReadString(*properties, "stopId");
    }
    if (parent && ReadString(*parent, "type") == "stop" &&
        !ReadString(*parent, "disassembledName").empty()) {
        stop["nameWO"] = ReadString(*parent, "disassembledName");
    }
    const auto coordinates = location.find("coord");
    if (coordinates != location.end() && coordinates->is_array() &&
        coordinates->size() == KVV_EFA_COORDINATE_COMPONENTS && coordinates->front().is_number() &&
        coordinates->back().is_number()) {
        // RapidJSON reverses the classic feed's longitude/latitude ordering.
        const double latitude = coordinates->front().get<double>();
        const double longitude = coordinates->back().get<double>();
        if (std::isfinite(latitude) && std::isfinite(longitude) &&
            std::abs(latitude) <= KVV_EFA_MAX_ABS_LATITUDE_DEGREES &&
            std::abs(longitude) <= KVV_EFA_MAX_ABS_LONGITUDE_DEGREES) {
            stop["x"] = longitude;
            stop["y"] = latitude;
        }
    }
    return stop;
}

Json RapidSequence(const Json& event, const char* key) {
    Json sequence = Json::array();
    for (const Json* const location : ReadRecordList(event, key)) {
        const Json stop = RapidStop(*location);
        // These are already absolute predictions; no minute delay is applied.
        sequence.push_back({{"nameWO", ReadString(stop, "nameWO")},
            {"ref",
                {{"id", ReadString(stop, "stopID")},
                    {"coords", ReadString(stop, "x") + "," + ReadString(stop, "y")},
                    {"arrDateTimeSec", RapidSequenceTime(*location, false)},
                    {"depDateTimeSec", RapidSequenceTime(*location, true)}}}});
    }
    return sequence;
}

Json NormalizeResponse(Json response) {
    if (!response.is_object() || !response.contains("stopEvents")) {
        return response; // Previously recorded classic JSON remains supported.
    }
    if (!response["stopEvents"].is_array()) {
        throw std::runtime_error("KVV EFA response has no usable stop events");
    }
    if (!IsRapidTimestamp(FindServerTime(response))) {
        throw std::runtime_error("KVV RapidJSON response requires its HTTP clock in serverTime");
    }

    // Adapt provider schemas once at the boundary. The existing trip retention,
    // cancellation and interpolation logic then handles both without rounding.
    Json normalized = {{"parameters", response["parameters"]}, {"departureList", Json::array()}};
    for (const Json* const event : ReadRecordList(response, "stopEvents")) {
        const Json* const location = ReadObject(*event, "location");
        const Json* const transportation = ReadObject(*event, "transportation");
        if (!location || !transportation) {
            continue;
        }
        Json row = RapidStop(*location);
        row["dateTime"] = RapidDateTime(ReadString(*event, "departureTimePlanned"));
        row["realDateTime"] = RapidDateTime(ReadString(*event, "departureTimeEstimated"));
        Json line = {{"stateless", ReadString(*transportation, "id")},
            {"number", ReadString(*transportation, "number")}};
        if (ReadString(line, "number").empty()) {
            line["number"] = ReadString(*transportation, "disassembledName");
        }
        if (const Json* const product = ReadObject(*transportation, "product")) {
            line["motType"] = ReadString(*product, "class");
        }
        if (const Json* const destination = ReadObject(*transportation, "destination")) {
            line["direction"] = ReadString(*destination, "name");
        }
        if (const Json* const properties = ReadObject(*transportation, "properties")) {
            line["key"] = ReadString(*properties, "tripCode");
        }
        if (const Json* const properties = ReadObject(*event, "properties")) {
            row["attrs"] = Json::array({{{"name", KVV_EFA_REALTIME_TRIP_ID_ATTRIBUTE},
                {"value", ReadString(*properties, KVV_EFA_REALTIME_TRIP_ID_ATTRIBUTE)}}});
        }
        if (event->contains("realtimeStatus")) {
            row["realtimeStatus"] = event->at("realtimeStatus").dump();
        }
        row["servingLine"] = std::move(line);
        row["prevStopSeq"] = RapidSequence(*event, "previousLocations");
        row["onwardStopSeq"] = RapidSequence(*event, "onwardLocations");
        normalized["departureList"].push_back(std::move(row));
    }
    return normalized;
}

std::string ReadWholeFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(std::string("Cannot read KVV response file: ") + path);
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ParseHttpDate(const std::string& value) {
    std::tm date{};
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    input >> std::ws >> std::get_time(&date, KVV_EFA_HTTP_DATE_FORMAT) >> std::ws;
    if (input.fail() || !input.eof()) {
        return {};
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&date, KVV_EFA_UTC_DATE_FORMAT);
    return IsRapidTimestamp(output.str()) ? output.str() : std::string{};
}

std::string PreserveHttpClock(const std::string& response) {
    std::size_t body_offset = 0;
    std::string server_time;
    const std::string header_end = KVV_EFA_HTTP_HEADER_TERMINATOR;
    // curl can include proxy/informational header blocks before the final one.
    while (response.compare(body_offset, std::char_traits<char>::length(KVV_EFA_HTTP_PREFIX),
               KVV_EFA_HTTP_PREFIX) == 0) {
        const auto end = response.find(header_end, body_offset);
        if (end == std::string::npos) {
            throw std::runtime_error("KVV EFA response has incomplete HTTP headers");
        }
        server_time.clear();
        std::istringstream headers(response.substr(body_offset, end - body_offset));
        std::string line;
        while (std::getline(headers, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
                return std::tolower(character);
            });
            if (name == "date") {
                server_time = ParseHttpDate(line.substr(colon + 1));
            }
        }
        body_offset = end + header_end.size();
    }
    if (server_time.empty()) {
        throw std::runtime_error("KVV EFA response has no usable HTTP Date header");
    }
    Json body = Json::parse(response.substr(body_offset));
    if (!body.is_object() || !body.contains("stopEvents") || !body["stopEvents"].is_array()) {
        throw std::runtime_error("KVV EFA response has no RapidJSON departure board");
    }
    // Save the provider clock with the payload, never substitute the replay clock.
    body["parameters"] =
        Json::array({{{"name", KVV_EFA_SERVER_TIME_PARAMETER}, {"value", server_time}}});
    return body.dump();
}

std::string DownloadUrl(const std::string& url, bool include_headers) {
    // TLS is delegated to curl, which is installed by default on Ubuntu/WSL.
    // Callers add only validated numeric IDs or encoded numeric coordinates.
    const std::string command = "curl --fail --silent --show-error --compressed " +
        std::string(include_headers ? "--include " : "") +
        "--suppress-connect-headers --connect-timeout " +
        std::to_string(KVV_EFA_CONNECT_TIMEOUT_SECONDS) + " --max-time " +
        std::to_string(KVV_EFA_REQUEST_TIMEOUT_SECONDS) + " '" + url + "'";
    FILE* const pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("Could not start curl for the KVV EFA request");
    }

    // Read the complete response, then check curl before exposing it to the parser.
    std::string response;
    char buffer[KVV_EFA_RESPONSE_BUFFER_SIZE];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        response += buffer;
    }
    if (pclose(pipe) != 0) {
        throw std::runtime_error("KVV EFA request failed (curl returned an error)");
    }
    return response;
}

bool ValidStopId(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char character) {
        return character >= '0' && character <= '9';
    });
}

bool ValidCoordinate(GeoPoint point) {
    return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
        std::abs(point.latitude) <= KVV_EFA_MAX_ABS_LATITUDE_DEGREES &&
        std::abs(point.longitude) <= KVV_EFA_MAX_ABS_LONGITUDE_DEGREES;
}

double DistanceMetres(GeoPoint from, GeoPoint to) {
    const double latitude = (to.latitude - from.latitude) * KVV_EFA_RADIANS_PER_DEGREE;
    const double longitude = (to.longitude - from.longitude) * KVV_EFA_RADIANS_PER_DEGREE;
    const double sine_latitude = std::sin(latitude / 2);
    const double sine_longitude = std::sin(longitude / 2);
    const double haversine = std::clamp(sine_latitude * sine_latitude +
            std::cos(from.latitude * KVV_EFA_RADIANS_PER_DEGREE) *
                std::cos(to.latitude * KVV_EFA_RADIANS_PER_DEGREE) * sine_longitude *
                sine_longitude,
        0.0, 1.0);
    return 2 * KVV_EFA_EARTH_RADIUS_METRES *
        std::atan2(std::sqrt(haversine), std::sqrt(1 - haversine));
}

bool HasRailOrTram(const Json& location) {
    if (!location.contains("productClasses") || !location["productClasses"].is_array()) {
        return false;
    }
    for (const Json& mode : location["productClasses"]) {
        if (mode == KVV_EFA_PRODUCT_CLASS_RAIL || mode == KVV_EFA_PRODUCT_CLASS_TRAM) {
            return true;
        }
    }
    return false;
}

// Keep provider ordering: previous calls, this departure, then onward calls.
std::vector<TimedStop> ParseTripCalls(const Json& entry) {
    std::vector<TimedStop> calls;
    for (const Json* const call : ReadRecordList(entry, "prevStopSeq")) {
        if (const std::optional<TimedStop> parsed = ParseSequenceStop(*call)) {
            calls.push_back(*parsed);
        }
    }

    if (const std::optional<TimedStop> current = ParseCurrentStop(entry)) {
        calls.push_back(*current);
    }

    for (const Json* const call : ReadRecordList(entry, "onwardStopSeq")) {
        if (const std::optional<TimedStop> parsed = ParseSequenceStop(*call)) {
            calls.push_back(*parsed);
        }
    }

    return calls;
}

TransitSnapshot DecodeResponse(const Json& response, int elapsed_seconds) {
    // Validate the snapshot clock before estimating any trip positions.
    TransitSnapshot result;
    result.server_time = FindServerTime(response);
    const std::optional<long long> server_time = ParseServerTime(result.server_time);
    if (!server_time) {
        throw std::runtime_error("KVV EFA response has no usable server time");
    }
    const long long now = *server_time + std::max(0, elapsed_seconds);
    if (!response.is_object() || !response.contains("departureList")) {
        throw std::runtime_error("KVV EFA response has no departure board");
    }

    std::set<std::string> known_stops;
    std::unordered_set<std::string> known_vehicles;

    for (const Json* const entry : ReadRecordList(response, "departureList")) {
        // Only active rail and tram services participate in this snapshot.
        const Json* const serving_line = ActiveRailLine(*entry);
        if (!serving_line) {
            continue;
        }

        const auto calls = ParseTripCalls(*entry);
        if (calls.empty()) {
            continue;
        }

        for (const TimedStop& call : calls) {
            if (known_stops.insert(StopKey(call.stop)).second) {
                result.stops.push_back(call.stop);
            }
        }

        // Stops remain useful even if a trip has no estimate at this instant.
        const auto estimate = EstimateVehiclePosition(calls, now);
        if (!estimate) {
            continue;
        }
        const std::string vehicle_key = TripKey(*entry);
        if (!known_vehicles.insert(vehicle_key).second) {
            continue;
        }

        VehicleEstimate vehicle = *estimate;
        SetVehicleService(vehicle, *serving_line);
        result.vehicles.push_back(std::move(vehicle));
    }

    return result;
}

} // namespace

Stop FindNearestStop(GeoPoint center) {
    if (!ValidCoordinate(center)) {
        throw std::invalid_argument("KVV stop lookup requires valid WGS84 coordinates");
    }
    // The coordinate endpoint takes longitude first; RapidJSON returns latitude first.
    std::ostringstream coordinates;
    coordinates.imbue(std::locale::classic());
    coordinates << std::fixed << std::setprecision(KVV_EFA_COORDINATE_DECIMAL_PLACES)
                << center.longitude << "%3A" << center.latitude << "%3AWGS84%5BDD.ddddd%5D";
    const std::string url =
        KVV_EFA_COORDINATE_ENDPOINT + coordinates.str() + KVV_EFA_COORDINATE_OPTIONS;

    try {
        const Json response = Json::parse(DownloadUrl(url, false));
        std::optional<Stop> nearest;
        double nearest_distance = KVV_EFA_NEAREST_STOP_RADIUS_METRES;
        for (const Json* const location : ReadRecordList(response, "locations")) {
            const Json* const properties = ReadObject(*location, "properties");
            if (ReadString(*location, "type") != "stop" || properties == nullptr ||
                !HasRailOrTram(*location)) {
                continue;
            }
            const std::string id = ReadString(*properties, "stopId");
            const std::string name = ReadString(*location, "name");
            const bool named = std::any_of(name.begin(), name.end(), [](unsigned char character) {
                return !std::isspace(character);
            });
            const auto coordinate = location->find("coord");
            if (!ValidStopId(id) || !named || coordinate == location->end() ||
                !coordinate->is_array() || coordinate->size() != KVV_EFA_COORDINATE_COMPONENTS ||
                !coordinate->front().is_number() || !coordinate->back().is_number()) {
                continue;
            }
            const GeoPoint point{
                coordinate->front().get<double>(), coordinate->back().get<double>()};
            if (!ValidCoordinate(point)) {
                continue;
            }
            const double distance = DistanceMetres(center, point);
            // Provider ordering/distance can reflect walking routes instead of proximity.
            if (distance <= nearest_distance && (!nearest || distance < nearest_distance)) {
                nearest = Stop{id, name, point};
                nearest_distance = distance;
            }
        }
        if (nearest) {
            return *nearest;
        }
    } catch (const Json::exception& error) {
        throw std::runtime_error(std::string("Could not parse KVV stop lookup: ") + error.what());
    }
    throw std::runtime_error("No rail/tram stop found within " +
        std::to_string(KVV_EFA_NEAREST_STOP_RADIUS_METRES) + " metres of the map center");
}

std::string DownloadDepartureResponse(const std::string& stop_id) {
    // Replays need neither a selected station nor a network connection.
    if (const char* const response_file = std::getenv(KVV_EFA_RESPONSE_FILE_ENV)) {
        if (*response_file != '\0') {
            return ReadWholeFile(response_file);
        }
    }
    if (!ValidStopId(stop_id)) {
        throw std::invalid_argument("KVV departures require a numeric EFA stop ID");
    }
    return PreserveHttpClock(
        DownloadUrl(KVV_EFA_DEPARTURE_ENDPOINT + stop_id + KVV_EFA_DEPARTURE_OPTIONS, true));
}

TransitSnapshot ParseDepartureResponse(const std::string& response, int elapsed_seconds) {
    try {
        return DecodeResponse(NormalizeResponse(Json::parse(response)), elapsed_seconds);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("Could not parse KVV EFA response: ") + error.what());
    }
}

void TransitSession::Update(const std::string& text) {
    Json response = NormalizeResponse(Json::parse(text));

    // Validate before touching the last successful reading.
    DecodeResponse(response, 0);
    const std::string server_time = FindServerTime(response);
    const long long now = *ParseServerTime(server_time);
    const bool utc_clock = IsRapidTimestamp(server_time);
    if (!response_.empty() && utc_clock == utc_clock_ && now < server_seconds_) {
        throw std::runtime_error("KVV returned an older snapshot");
    }

    // A replay format change also changes the clock basis; retained rows must match.
    if (utc_clock != utc_clock_) {
        rows_.clear();
        motion_.clear();
    }

    // Replace predictions for observed trips, retaining recently missing trips.
    for (const Json* const row : ReadRecordList(response, "departureList")) {
        const auto key = TripKey(*row);
        if (!key.empty()) {
            rows_[key] = {row->dump(), now};
        }
    }

    // Expire by feed time, so local redraws cannot prolong a missing trip.
    response["departureList"] = Json::array();
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (now - it->second.second > KVV_EFA_TRIP_RETENTION_SECONDS) {
            motion_.erase(it->first);
            it = rows_.erase(it);
        } else {
            response["departureList"].push_back(Json::parse(it->second.first));
            ++it;
        }
    }

    utc_clock_ = utc_clock;
    server_seconds_ = now;
    response_ = response.dump();
}

TransitSnapshot TransitSession::View(int elapsed_seconds) const {
    return ParseDepartureResponse(response_, elapsed_seconds);
}

double TransitSession::MotionState::TimeAt(std::chrono::steady_clock::time_point now) const {
    const double elapsed = std::clamp(
        std::chrono::duration<double>(now - anchored_at).count(), 0.0, remaining_seconds);
    return virtual_time + AdvanceMotionClock(target_time - virtual_time, elapsed);
}

TransitSnapshot TransitSession::SmoothedView(
    double elapsed_seconds, std::chrono::steady_clock::time_point now) {
    const Json response = Json::parse(response_);
    // Update already validated this response. Parse each trip only once here.
    TransitSnapshot result;
    result.server_time = FindServerTime(response);
    std::set<std::string> known_stops;
    const double age = std::clamp(elapsed_seconds, 0.0, double(KVV_EFA_MAX_EXTRAPOLATION_SECONDS));
    const double target = server_seconds_ + age;
    std::unordered_set<std::string> active;

    for (const Json* const row : ReadRecordList(response, "departureList")) {
        const Json* const line = ActiveRailLine(*row);
        if (!line) {
            continue;
        }
        const std::string key = TripKey(*row);
        if (key.empty() || !active.insert(key).second) {
            continue;
        }
        const std::string& prediction = rows_.at(key).first;
        auto calls = ParseTripCalls(*row);
        for (const TimedStop& call : calls) {
            if (known_stops.insert(StopKey(call.stop)).second) {
                result.stops.push_back(call.stop);
            }
        }
        auto [it, inserted] = motion_.try_emplace(key);
        MotionState& motion = it->second;
        const bool revised =
            inserted || motion.server_seconds != server_seconds_ || motion.prediction != prediction;
        if (revised) {
            std::optional<double> anchor = target;
            if (!inserted) {
                const auto previous = ParseTripCalls(Json::parse(motion.prediction));
                const double old_time = motion.TimeAt(now);
                const auto position = FindJourneyProgress(previous, old_time, 0.0);
                if (position) {
                    anchor = RebaseMotionClock(previous, *position, calls, target);
                } else if (JourneyFinished(previous, old_time)) {
                    // A later delay must not resurrect a train at an earlier stop.
                    anchor = std::nullopt;
                }
            }
            if (anchor) {
                motion = {prediction, server_seconds_, *anchor, target,
                    KVV_EFA_MAX_EXTRAPOLATION_SECONDS - age, now};
            } else {
                // A changed/ambiguous route cannot be joined without teleporting.
                // Continue the last known path only within its original stale cap.
                calls = ParseTripCalls(Json::parse(motion.prediction));
            }
        }
        // Exact dwell boundaries avoid the legacy ±15-second stop snapping.
        if (auto vehicle = EstimateVehiclePosition(calls, motion.TimeAt(now), 0.0)) {
            SetVehicleService(*vehicle, *line);
            result.vehicles.push_back(std::move(*vehicle));
        }
    }

    // Cancellation, mode changes and retention expiry also discard motion history.
    for (auto it = motion_.begin(); it != motion_.end();) {
        if (!active.count(it->first)) {
            it = motion_.erase(it);
        } else {
            ++it;
        }
    }
    return result;
}

} // namespace kvv
