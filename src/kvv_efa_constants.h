#ifndef KVV_EFA_CONSTANTS_H
#define KVV_EFA_CONSTANTS_H

#include <climits>

// Request configuration and provider codes. Stringification keeps the URL in
// sync with numeric request limits while remaining a compile-time string.
#define KVV_EFA_STRINGIFY_IMPL(value) #value
#define KVV_EFA_STRINGIFY(value) KVV_EFA_STRINGIFY_IMPL(value)
#define KVV_EFA_DEPARTURE_LIMIT 40
#define KVV_EFA_SEQUENCE_LIMIT 40
#define KVV_EFA_PRODUCT_CLASS_RAIL 1
#define KVV_EFA_PRODUCT_CLASS_TRAM 4
#define KVV_EFA_MODE_RAIL KVV_EFA_STRINGIFY(KVV_EFA_PRODUCT_CLASS_RAIL)
#define KVV_EFA_MODE_TRAM KVV_EFA_STRINGIFY(KVV_EFA_PRODUCT_CLASS_TRAM)
#define KVV_EFA_NEAREST_STOP_RADIUS_METRES 1000
#define KVV_EFA_COORDINATE_DECIMAL_PLACES 6
#define KVV_EFA_EARTH_RADIUS_METRES 6371000.0
#define KVV_EFA_RADIANS_PER_DEGREE (3.141592653589793 / 180.0)
// clang-format off
#define KVV_EFA_BASE_URL "https://projekte.kvv-efa.de/sl3-alone/"
#define KVV_EFA_DEPARTURE_ENDPOINT \
    KVV_EFA_BASE_URL "XSLT_DM_REQUEST?outputFormat=rapidJSON&type_dm=stopID&name_dm="
#define KVV_EFA_DEPARTURE_OPTIONS \
    "&mode=direct&useRealtime=1&useProxFootSearch=0&itdDateTimeDepArr=dep" \
    "&limit=" KVV_EFA_STRINGIFY(KVV_EFA_DEPARTURE_LIMIT) \
    "&depSequence=" KVV_EFA_STRINGIFY(KVV_EFA_SEQUENCE_LIMIT) \
    "&std3_commonMacro=dm&includedMeans=checkbox" \
    "&std3_inclMOT_" KVV_EFA_MODE_RAIL "Macro=true" \
    "&std3_inclMOT_" KVV_EFA_MODE_TRAM "Macro=true" \
    "&coordOutputFormat=WGS84%5BDD.ddddd%5D" \
    "&depType=stopEvents&includeCompleteStopSeq=1"
#define KVV_EFA_COORDINATE_ENDPOINT \
    KVV_EFA_BASE_URL "XML_COORD_REQUEST?outputFormat=RapidJSON&coord="
#define KVV_EFA_COORDINATE_OPTIONS \
    "&coordOutputFormat=WGS84%5BDD.ddddd%5D&inclFilter=1&type_1=STOP" \
    "&radius_1=" KVV_EFA_STRINGIFY(KVV_EFA_NEAREST_STOP_RADIUS_METRES)
// clang-format on
#define KVV_EFA_RESPONSE_FILE_ENV "KVV_EFA_RESPONSE_FILE"
#define KVV_EFA_CONNECT_TIMEOUT_SECONDS 4
#define KVV_EFA_REQUEST_TIMEOUT_SECONDS 10
#define KVV_EFA_RESPONSE_BUFFER_SIZE 4096
#define KVV_EFA_HTTP_PREFIX "HTTP/"
#define KVV_EFA_HTTP_HEADER_TERMINATOR "\r\n\r\n"
#define KVV_EFA_HTTP_DATE_FORMAT "%a, %d %b %Y %H:%M:%S GMT"
#define KVV_EFA_UTC_DATE_FORMAT "%Y-%m-%dT%H:%M:%SZ"

#define KVV_EFA_UNKNOWN_DELAY_MINUTES (-9999LL)
#define KVV_EFA_INVALID_PREDICTION_FLAG "0"
#define KVV_EFA_CANCELLED_STATUS_TOKEN "CANCEL"
#define KVV_EFA_SERVER_TIME_PARAMETER "serverTime"
#define KVV_EFA_REALTIME_TRIP_ID_ATTRIBUTE "RealtimeTripId"

// Geographic bounds and six-decimal-place coordinate keys for stop platforms.
#define KVV_EFA_MAX_ABS_LATITUDE_DEGREES 90.0
#define KVV_EFA_MAX_ABS_LONGITUDE_DEGREES 180.0
#define KVV_EFA_COORDINATE_KEY_UNITS_PER_DEGREE 1e6
#define KVV_EFA_COORDINATE_COMPONENTS 2

// Gregorian calendar. Keep years per era signed for negative-year division.
#define KVV_EFA_MARCH 3U
#define KVV_EFA_MONTHS_PER_YEAR 12
#define KVV_EFA_YEARS_PER_ERA 400
#define KVV_EFA_YEARS_PER_CENTURY 100U
#define KVV_EFA_LEAP_YEAR_INTERVAL 4U
#define KVV_EFA_DAYS_PER_COMMON_YEAR 365U
#define KVV_EFA_MAX_DAYS_PER_MONTH 31
#define KVV_EFA_DAYS_PER_ERA                                                                       \
    (1LL * KVV_EFA_YEARS_PER_ERA * KVV_EFA_DAYS_PER_COMMON_YEAR +                                  \
        KVV_EFA_YEARS_PER_ERA / KVV_EFA_LEAP_YEAR_INTERVAL -                                       \
        KVV_EFA_YEARS_PER_ERA / KVV_EFA_YEARS_PER_CENTURY + 1)

// March-July and August-December each contain 153 days over five months.
// Rounding their average month length gives each month's exact start offset.
#define KVV_EFA_MONTHS_PER_BLOCK 5U
#define KVV_EFA_DAYS_PER_BLOCK 153U
#define KVV_EFA_MONTH_OFFSET_ROUNDING (KVV_EFA_MONTHS_PER_BLOCK / 2)
// Days from the calculation's origin (0000-03-01) to 1970-01-01.
#define KVV_EFA_UNIX_EPOCH_OFFSET_DAYS 719468LL

// Time units and the representable range of signed timestamp arithmetic.
#define KVV_EFA_HOURS_PER_DAY 24
#define KVV_EFA_MINUTES_PER_HOUR 60
#define KVV_EFA_SECONDS_PER_MINUTE 60LL
#define KVV_EFA_SECONDS_PER_HOUR (KVV_EFA_SECONDS_PER_MINUTE * KVV_EFA_MINUTES_PER_HOUR)
#define KVV_EFA_SECONDS_PER_DAY (KVV_EFA_SECONDS_PER_HOUR * KVV_EFA_HOURS_PER_DAY)
#define KVV_EFA_MIN_TIMESTAMP_SECONDS LLONG_MIN
#define KVV_EFA_MAX_TIMESTAMP_SECONDS LLONG_MAX
// Bounds for converting signed delay minutes to seconds without overflow.
#define KVV_EFA_MIN_DELAY_MINUTES (KVV_EFA_MIN_TIMESTAMP_SECONDS / KVV_EFA_SECONDS_PER_MINUTE)
#define KVV_EFA_MAX_DELAY_MINUTES (KVV_EFA_MAX_TIMESTAMP_SECONDS / KVV_EFA_SECONDS_PER_MINUTE)
#define KVV_EFA_DWELL_TOLERANCE_SECONDS 15
#define KVV_EFA_TRIP_RETENTION_SECONDS (3 * KVV_EFA_SECONDS_PER_MINUTE)
// Presentation catches up through speed changes, never negative route motion.
#define KVV_EFA_MOTION_CORRECTION_SECONDS 15.0
#define KVV_EFA_MOTION_MIN_RATE 0.1
#define KVV_EFA_MOTION_MAX_RATE 3.0
#define KVV_EFA_MOTION_FRACTION_EPSILON 1e-6
#define KVV_EFA_MAX_EXTRAPOLATION_SECONDS 30
#define KVV_EFA_STALE_AFTER_SECONDS 15
#define KVV_EFA_MAX_DISPLAY_AGE_SECONDS 9999

// Fixed provider timestamp layouts; offsets are zero-based character indices.
#define KVV_EFA_TIMESTAMP_YEAR_OFFSET 0
#define KVV_EFA_TIMESTAMP_YEAR_WIDTH 4
#define KVV_EFA_MAX_TIMESTAMP_YEAR 9999
#define KVV_EFA_TIMESTAMP_FIELD_WIDTH 2

// Stop sequence: YYYYMMDD HH:MM[:SS].
#define KVV_EFA_SEQUENCE_MONTH_OFFSET 4
#define KVV_EFA_SEQUENCE_DAY_OFFSET 6
#define KVV_EFA_SEQUENCE_DATE_SEPARATOR_OFFSET 8
#define KVV_EFA_SEQUENCE_HOUR_OFFSET 9
#define KVV_EFA_SEQUENCE_HOUR_SEPARATOR_OFFSET 11
#define KVV_EFA_SEQUENCE_MINUTE_OFFSET 12
#define KVV_EFA_SEQUENCE_MINUTE_SEPARATOR_OFFSET 14
#define KVV_EFA_SEQUENCE_SECOND_OFFSET 15
#define KVV_EFA_SEQUENCE_MINUTE_LENGTH                                                             \
    (KVV_EFA_SEQUENCE_MINUTE_OFFSET + KVV_EFA_TIMESTAMP_FIELD_WIDTH)
#define KVV_EFA_SEQUENCE_SECOND_LENGTH                                                             \
    (KVV_EFA_SEQUENCE_SECOND_OFFSET + KVV_EFA_TIMESTAMP_FIELD_WIDTH)

// Server time: YYYY-MM-DDTHH:MM[:SS], optionally followed by a suffix.
#define KVV_EFA_SERVER_YEAR_SEPARATOR_OFFSET 4
#define KVV_EFA_SERVER_MONTH_OFFSET 5
#define KVV_EFA_SERVER_MONTH_SEPARATOR_OFFSET 7
#define KVV_EFA_SERVER_DAY_OFFSET 8
#define KVV_EFA_SERVER_DATE_SEPARATOR_OFFSET 10
#define KVV_EFA_SERVER_HOUR_OFFSET 11
#define KVV_EFA_SERVER_HOUR_SEPARATOR_OFFSET 13
#define KVV_EFA_SERVER_MINUTE_OFFSET 14
#define KVV_EFA_SERVER_MINUTE_SEPARATOR_OFFSET 16
#define KVV_EFA_SERVER_SECOND_OFFSET 17
#define KVV_EFA_SERVER_MINUTE_LENGTH (KVV_EFA_SERVER_MINUTE_OFFSET + KVV_EFA_TIMESTAMP_FIELD_WIDTH)
#define KVV_EFA_SERVER_SECOND_LENGTH (KVV_EFA_SERVER_SECOND_OFFSET + KVV_EFA_TIMESTAMP_FIELD_WIDTH)

#define KVV_EFA_UTC_TIMESTAMP_LENGTH (KVV_EFA_SERVER_SECOND_LENGTH + 1)

#endif // KVV_EFA_CONSTANTS_H
