#include "haicode/session_groups.h"

#include <ctime>

namespace haicode {

namespace {

struct CivilDate {
    int year;
    int month;   // 1..12
    int day;     // 1..31
};

// Days since 1970-01-01 for a civil date (proleptic Gregorian). Howard
// Hinnant's algorithm; the arithmetic is exact for every representable year
// and needs no epoch table.
int64_t days_from_civil(const CivilDate& d)
{
    int y = d.year;
    if (d.month <= 2) --y;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(d.month + (d.month > 2 ? -3 : 9))
                          + 2u) / 5u + static_cast<unsigned>(d.day) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe)
        - 719468;
}

CivilDate civil_from_days(int64_t z)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp + (mp < 10u ? 3u : -9u);
    return {static_cast<int>(y + (m <= 2 ? 1 : 0)), static_cast<int>(m),
            static_cast<int>(d)};
}

// Local calendar date of an epoch-millisecond time. Falls back to the UTC
// date if localtime_r cannot represent it.
CivilDate local_civil(int64_t time_ms)
{
    time_t seconds = static_cast<time_t>(time_ms / 1000);
    tm local{};
    if (!localtime_r(&seconds, &local))
        return civil_from_days(static_cast<int64_t>(seconds) / 86400);
    return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
}

} // namespace

DayBucket
session_day_bucket(int64_t time_ms, int64_t now_ms)
{
    if (time_ms <= 0) return DayBucket::Older;

    const CivilDate then = local_civil(time_ms);
    const CivilDate now = local_civil(now_ms);
    // Negative = in the future relative to `now`; clock skew, call it Today.
    int64_t delta = days_from_civil(now) - days_from_civil(then);
    if (delta <= 0) return DayBucket::Today;
    if (delta == 1) return DayBucket::Yesterday;

    // Days back from today to this week's Monday (tm_wday is Sunday-based:
    // 0 = Sunday). Anything within that span that is not today/yesterday is
    // "this week" -- checked BEFORE the month rule, since Monday can be
    // only two days back and "this week" is the nearer bucket.
    tm now_tm{};
    time_t now_seconds = static_cast<time_t>(now_ms / 1000);
    int days_to_monday = 6;
    if (localtime_r(&now_seconds, &now_tm))
        days_to_monday = (now_tm.tm_wday + 6) % 7;
    if (delta <= days_to_monday) return DayBucket::ThisWeek;

    // Same calendar month (and year) as today, but older than this week.
    if (then.year == now.year && then.month == now.month)
        return DayBucket::ThisMonth;

    return DayBucket::Older;
}

const char*
day_bucket_label(DayBucket bucket)
{
    switch (bucket) {
        case DayBucket::Today:     return "Today";
        case DayBucket::Yesterday: return "Yesterday";
        case DayBucket::ThisWeek:  return "This week";
        case DayBucket::ThisMonth: return "This month";
        case DayBucket::Older:     default: return "Older";
    }
}

} // namespace haicode
