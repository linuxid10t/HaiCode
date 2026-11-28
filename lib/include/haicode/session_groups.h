#pragma once

#include <cstdint>
#include <string>

namespace haicode {

// Calendar bucket of a session's last-modified time, for the sidebar's
// "Group by day" sections. Ordered oldest → newest so callers can iterate
// groups in display order (newest first) by walking the enum backwards.
enum class DayBucket {
    Older = 0,
    ThisMonth,
    ThisWeek,
    Yesterday,
    Today,
};

// Bucket `time_ms` relative to `now_ms` (both Unix epoch milliseconds), using
// the LOCAL calendar day: a session edited at 23:59 is "Today" until local
// midnight, not UTC midnight. Week starts on Monday. A time in the future
// (clock skew) counts as Today; a non-positive time (never updated) is Older.
DayBucket session_day_bucket(int64_t time_ms, int64_t now_ms);

// Display label for a bucket: "Today", "Yesterday", "This week",
// "This month", "Older".
const char* day_bucket_label(DayBucket bucket);

} // namespace haicode
