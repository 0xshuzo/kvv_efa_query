# kvv_efa_query

C++17 library for querying and processing the public KVV EFA departure monitor
for a chosen station near any WGS84 center. Owns HTTP/replay input, JSON parsing,
real-time delays, stop sequences, cancellations, trip retention and refresh
scheduling. This folder uses **efa**, matching the API and the existing destination.

The library uses nlohmann/json 3.11+, standard C++ threads and the `curl` executable
for TLS requests. CMake accepts an installed JSON package or discovers a source
checkout by its contents, first within this source tree and then under
`SOURCE_DEPENDENCY_ROOTS` (a semicolon-separated CMake setting or environment
variable). Set `JSON_SOURCE_DIR` to select a specific source checkout. Initialize
bundled submodules before configuring if needed.

```bash
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Consumers add this directory with CMake and link `kvv_efa_query`. Public headers
are `kvv_efa_query.h` and `transit_refresh.h`.

- `FindNearestStop({latitude, longitude})` queries nearby EFA stops within 1000 m,
  then selects the geographically closest named rail/tram stop (product classes
  1 or 4). Bus-only stops are excluded because departure estimates cover rail and
  tram services. The result contains the numeric stop ID, provider name and
  coordinates; no usable station or an invalid center raises an exception.
  Resolve once for each center and reuse the ID across refreshes.
- `DownloadDepartureResponse(stop_id)` fetches RapidJSON for that numeric EFA
  stop ID, with a 4-second connection timeout and 10-second total timeout.
  It preserves the provider's HTTP `Date`
  clock as `parameters[{"name":"serverTime","value":"...Z"}]` in the returned
  JSON. Save that returned JSON for deterministic replay; raw RapidJSON bodies
  lack a clock. `KVV_EFA_RESPONSE_FILE=/path/to/feed.json` selects a saved response.
  Existing classic JSON recordings remain supported. In replay mode this call
  accepts an empty stop ID and bypasses all network requests; callers must skip
  `FindNearestStop` when replaying.
- `ParseDepartureResponse(json, elapsed_seconds)` returns `TransitSnapshot`:
  provider time, unique stop coordinates and rail/tram timetable estimates.
- Each `VehicleEstimate` contains its line, destination, `from`/`to` stop
  coordinates and temporal `progress` (0–1). A dwell uses identical endpoints.
  These are **not GPS positions**. The consumer chooses route geometry and
  viewport; the library does not discard stops based on screen bounds.
- `TransitSession` retains recently observed trips for up to three feed minutes,
  replaces revised predictions, suppresses cancellations and rejects older or
  unusable snapshots without replacing the last good response. `View()` returns
  the authoritative timetable estimate; `SmoothedView(elapsed_seconds, now)`
  maintains continuous live motion using a steady clock.
- `TransitRefresh(fetch, replay=false)` requires an explicit fetch callback;
  there is no implicit default station. `Poll(now)` starts at most one background
  request, normally every five seconds. Errors retry after five seconds.
  `Snapshot(now)` preserves
  the last good reading, marks failures or a 15-second-old provider timestamp
  stale, and caps extrapolation at 30 seconds. Live snapshots use the smoothed
  presentation path. Pass `replay=true` to freeze exact estimates at the saved
  response time.

Live timing uses RapidJSON's absolute `arrivalTimeEstimated` and
`departureTimeEstimated` values at whole-second resolution, falling back to the
corresponding planned timestamp when an estimate is missing or invalid. Arrival
and departure predictions stay independent, preserving dwell time and early
running. No conversion to whole-minute delays occurs. All live timestamps and
`TransitSnapshot::server_time` use UTC (`Z`), as specified by the
[MENTZ EFA JSON API](https://mobidata-bw.de/daten/portal/efa-json/EFA_JSON_API_Training_EN_2.9.pdf).

Prediction refreshes preserve each trip's displayed position along its stop
sequence. A prediction ahead of that position temporarily speeds up its
presentation clock; a prediction behind it slows that clock without reversing.
The correction decays over a 15-second time constant with clock rates bounded
between 0.1 and 3 times normal. These settings are named macros in
`src/kvv_efa_constants.h`. Motion uses exact arrival/departure boundaries to avoid
snapping near stops, stays on the consumer's rail geometry, and is independent
of redraw frequency. Replay bypasses smoothing, cancellations remove the train,
and stale data freezes both travel and correction after 30 seconds. If a revised
route cannot be matched unambiguously, its previous path remains in use only
within that original 30-second limit.

Legacy recordings retain EFA's local civil clock: absolute predictions take
precedence, otherwise the parser applies signed minute delays to scheduled
timestamps. Optional seconds in date objects are preserved; invalid, missing
or `-9999` delays use schedule. Switching a session between UTC and legacy local
clocks clears retained trips. Estimates remain limited by the provider's
coverage and prediction accuracy; they do not establish the actual track used.
