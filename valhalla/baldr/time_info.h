#pragma once

#include <chrono>

#include <valhalla/baldr/datetime.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/midgard/logging.h>
#include <valhalla/proto/api.pb.h>

namespace dt = valhalla::baldr::DateTime;
namespace sc = std::chrono;

namespace valhalla {
namespace baldr {
// A structure for tracking time information as the route progresses
struct TimeInfo {
  // whether or not the provided location had valid time information or not
  uint64_t valid : 1;

  // index into the timezone database of the location
  // used to do timezone offset as the route progresses
  uint64_t timezone_index : 9;

  // seconds from epoch adjusted for timezone at the location
  // used to do local time offset as the route progresses
  uint64_t local_time : 54;

  // the ordinal second from the beginning of the week (starting monday at 00:00)
  // used to look up historical traffic as the route progresses
  uint64_t second_of_week : 20;

  // the distance in seconds from now
  uint64_t seconds_from_now : 43;

  // the sign bit for seconds_from_now
  uint64_t negative_seconds_from_now : 1;

  // a timezone offset cache because doing the offset math is expensive
  baldr::DateTime::tz_sys_info_cache_t* tz_cache;

  /**
   * Helper function to initialize the object from a location. Uses the graph to
   * find timezone information about the edge candidates at the location. If the
   * graph has no timezone information or the location has no edge candidates the
   * default timezone will be used (if unspecified UTC is used). If no datetime
   * is provided on the location or an invalid one is provided the TimeInfo will
   * be invalid.
   *
   * @param location                 location for which to initialize the TimeInfo
   * @param reader                   used to get timezone information from the graph
   * @param default_timezone_index   used when no timezone information is available
   * @return the initialized TimeInfo
   */
  static TimeInfo
  make(valhalla::Location& location,
       baldr::GraphReader& reader,
       baldr::DateTime::tz_sys_info_cache_t* tz_cache = nullptr,
       int default_timezone_index = baldr::DateTime::get_tz_db().to_index("Etc/UTC")) {
    // No time to to track
    if (!location.has_date_time()) {
      return {false};
    }

    // Find the first edge whose end node has a valid timezone index and keep it
    int timezone_index = 0;
    for (const auto& pe : location.path_edges()) {
      const baldr::GraphTile* tile = nullptr;
      const auto* edge = reader.directededge(baldr::GraphId(pe.graph_id()), tile);
      timezone_index = reader.GetTimezone(edge->endnode(), tile);
      if (timezone_index != 0)
        break;
    }

    // Set the origin timezone to be the timezone at the end node use this for timezone changes
    if (timezone_index == 0) {
      // Don't use the provided one if its not valid
      if (!dt::get_tz_db().from_index(default_timezone_index)) {
        default_timezone_index = baldr::DateTime::get_tz_db().to_index("Etc/UTC");
      }
      LOG_WARN("No timezone for location using default");
      timezone_index = default_timezone_index;
    }
    const auto* tz = dt::get_tz_db().from_index(timezone_index);

    // Get the current local time and set it if its a current time route
    // NOTE: we set to minute resolution to match the input format
    const auto now_date =
        date::make_zoned(tz, sc::time_point_cast<sc::seconds>(
                                 sc::time_point_cast<sc::minutes>(sc::system_clock::now())));
    if (location.date_time() == "current") {
      std::ostringstream iso_dt;
      iso_dt << date::format("%FT%R", now_date);
      location.set_date_time(iso_dt.str());
    }

    // Convert the requested time into seconds from epoch
    date::local_seconds parsed_date;
    try {
      parsed_date = dt::get_formatted_date(location.date_time(), true);
    } catch (...) {
      LOG_ERROR("Could not parse provided date_time: " + location.date_time());
      return {false};
    }
    const auto then_date = date::make_zoned(tz, parsed_date);
    uint64_t local_time = date::to_utc_time(then_date.get_sys_time()).time_since_epoch().count();

    // What second of the week is this (for historical traffic lookup)
    auto second_of_week = dt::second_of_week(local_time, tz);

    // When is this route with respect to now this will let us appropriately use current flow traffic
    int64_t seconds_from_now = (then_date.get_local_time() - now_date.get_local_time()).count();

    // Make a valid object
    return {true,
            static_cast<uint64_t>(timezone_index),
            local_time,
            second_of_week,
            static_cast<uint64_t>(std::abs(seconds_from_now)),
            seconds_from_now < 0,
            tz_cache};
  }

  /**
   * Offset all the initial time info to reflect the progress along the route to this point
   * @param seconds_offset  the number of seconds to offset the TimeInfo by
   * @param next_tz_index   the timezone index at the new location
   * @return a new TimeInfo object reflecting the offset
   */
  inline TimeInfo forward(float seconds_offset, int next_tz_index) const {
    if (!valid)
      return *this;

    // offset the local time and second of week by the amount traveled to this label
    uint64_t lt = local_time + static_cast<uint64_t>(seconds_offset);
    uint32_t sw = static_cast<uint32_t>(second_of_week + seconds_offset);

    // if the timezone changed we need to account for that offset as well
    if (next_tz_index != timezone_index) {
      namespace dt = baldr::DateTime;
      int tz_diff = dt::timezone_diff(lt, dt::get_tz_db().from_index(timezone_index),
                                      dt::get_tz_db().from_index(next_tz_index), tz_cache);
      lt += tz_diff;
      sw += tz_diff;
    }

    // wrap the week second if it went past the end
    if (sw > valhalla::midgard::kSecondsPerWeek) {
      sw -= valhalla::midgard::kSecondsPerWeek;
    }

    // offset the distance to now handling the sign
    int64_t sign = static_cast<int64_t>(negative_seconds_from_now) * -2 + 1;
    int64_t sfn =
        static_cast<int64_t>(seconds_from_now) * sign + static_cast<int64_t>(seconds_offset);

    // return the shifted object, notice that seconds from now is only useful for
    // date_time type == current
    return {valid,   static_cast<uint64_t>(next_tz_index), lt,
            sw,      static_cast<uint64_t>(std::abs(sfn)), sfn < 0,
            tz_cache};
  }

  /**
   * Offset all the initial time info to reflect the progress along the route to this point
   * @param seconds_offset  the number of seconds to offset the TimeInfo by
   * @param next_tz_index   the timezone index at the new location
   * @return a new TimeInfo object reflecting the offset
   */
  inline TimeInfo reverse(float seconds_offset, int next_tz_index) const {
    if (!valid)
      return *this;

    // offset the local time and second of week by the amount traveled to this label
    uint64_t lt = local_time - static_cast<uint64_t>(seconds_offset); // dont route near the epoch
    int32_t sw = static_cast<int32_t>(second_of_week) - static_cast<int32_t>(seconds_offset);

    // if the timezone changed we need to account for that offset as well
    if (next_tz_index != timezone_index) {
      namespace dt = baldr::DateTime;
      int tz_diff = dt::timezone_diff(lt, dt::get_tz_db().from_index(timezone_index),
                                      dt::get_tz_db().from_index(next_tz_index), tz_cache);
      lt += tz_diff;
      sw += tz_diff;
    }

    // wrap the week second if it went past the beginning
    if (sw < 0) {
      sw = valhalla::midgard::kSecondsPerWeek + sw;
    }

    // offset the distance to now handling the sign
    int64_t sign = static_cast<int64_t>(negative_seconds_from_now) * -2 + 1;
    int64_t sfn =
        static_cast<int64_t>(seconds_from_now) * sign - static_cast<int64_t>(seconds_offset);

    // return the shifted object, notice that seconds from now is negative, this could be useful if
    // we had the ability to arrive_by current time but we dont for the moment
    return {valid,
            static_cast<uint64_t>(next_tz_index),
            lt,
            static_cast<uint32_t>(sw),
            static_cast<uint64_t>(std::abs(sfn)),
            sfn < 0,
            tz_cache};
  }

  // for unit tests
  bool operator==(const TimeInfo& ti) const {
    return valid == ti.valid && timezone_index == ti.timezone_index && local_time == ti.local_time &&
           second_of_week == ti.second_of_week && seconds_from_now == ti.seconds_from_now &&
           negative_seconds_from_now == ti.negative_seconds_from_now;
  }

  // for unit tests
  friend std::ostream& operator<<(std::ostream& os, const TimeInfo& ti) {
    return os << "{valid: " << ti.valid << ", timezone_index: " << ti.timezone_index
              << ", local_time: " << ti.local_time << ", second_of_week: " << ti.second_of_week
              << ", seconds_from_now: " << ti.seconds_from_now
              << ", negative_seconds_from_now: " << ti.negative_seconds_from_now
              << ", tz_cache: " << ti.tz_cache << "}";
  }
};

} // namespace baldr
} // namespace valhalla