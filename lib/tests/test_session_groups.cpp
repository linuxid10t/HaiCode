// Day-bucket classification for the sidebar's "Group by day" sections.
// Boundary cases are built from local civil dates with mktime so the test is
// independent of the machine's timezone and DST state.
#include <haicode/session_groups.h>
#include "test_check.h"

#include <ctime>
#include <iostream>
#include <string>

using haicode::DayBucket;
using haicode::day_bucket_label;
using haicode::session_day_bucket;

#define CHECK(cond, msg) TEST_REQUIRE(cond, msg)

static const int64_t kDayMs = 24LL * 60 * 60 * 1000;
static const int64_t kHourMs = 60LL * 60 * 1000;

// Local epoch-ms at a civil date and time. mktime normalizes out-of-range
// fields (day 0 = last day of the previous month, day -2 = two days earlier),
// which the edge cases below rely on for month rollover.
static int64_t at(int year, int month, int day, int hour, int minute = 0) {
    tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&t)) * 1000;
}

// Weekday of a civil date, Monday = 0.
static int weekday_mon0(int year, int month, int day) {
    tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    std::mktime(&t);
    return (t.tm_wday + 6) % 7;
}

// A 2026 date (1..28 so every month has it) falling on `wanted` (Mon = 0).
static void find_date(int wanted, int& year, int& month, int& day) {
    year = 2026;
    for (month = 1; month <= 12; ++month)
        for (day = 1; day <= 28; ++day)
            if (weekday_mon0(year, month, day) == wanted) return;
    TEST_REQUIRE(false, "no such weekday in 2026");
}

// Whether the (possibly out-of-range) day `d` of year/month `y`/`m` falls in
// calendar month `m` after mktime normalizes it (day 0 = last day before).
static bool same_month(int y, int m, int d) {
    tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    std::mktime(&t);
    return t.tm_mon + 1 == m && t.tm_year + 1900 == y;
}

static void check(DayBucket got, DayBucket want, const char* what) {
    if (got != want) {
        std::cerr << "[FAIL] " << what << ": got \"" << day_bucket_label(got)
                  << "\", want \"" << day_bucket_label(want) << "\"\n";
        std::exit(1);
    }
}

int main() {
    CHECK(std::string(day_bucket_label(DayBucket::Today)) == "Today", "Today label");
    CHECK(std::string(day_bucket_label(DayBucket::Yesterday)) == "Yesterday", "Yesterday label");
    CHECK(std::string(day_bucket_label(DayBucket::ThisWeek)) == "This week", "This week label");
    CHECK(std::string(day_bucket_label(DayBucket::ThisMonth)) == "This month", "This month label");
    CHECK(std::string(day_bucket_label(DayBucket::Older)) == "Older", "Older label");
    // Enum order is display order: the newest bucket has the largest value.
    CHECK(DayBucket::Older < DayBucket::ThisMonth, "Older sorts before ThisMonth");
    CHECK(DayBucket::ThisMonth < DayBucket::ThisWeek, "ThisMonth sorts before ThisWeek");
    CHECK(DayBucket::ThisWeek < DayBucket::Yesterday, "ThisWeek sorts before Yesterday");
    CHECK(DayBucket::Yesterday < DayBucket::Today, "Yesterday sorts before Today");

    // Anchor to a Wednesday: Monday/Tuesday are this week; Sunday and
    // earlier are last week.
    int y, m, d;
    find_date(2 /* Wednesday */, y, m, d);
    const int64_t noon = at(y, m, d, 12);

    // --- Today: same civil day at any hour, plus clock skew ---
    check(session_day_bucket(at(y, m, d, 0, 0), noon), DayBucket::Today, "00:00 today");
    check(session_day_bucket(at(y, m, d, 11, 59), noon), DayBucket::Today, "11:59 today");
    check(session_day_bucket(noon, noon), DayBucket::Today, "now itself");
    check(session_day_bucket(noon + kHourMs, noon), DayBucket::Today, "one hour ahead");
    check(session_day_bucket(noon + 30 * kDayMs, noon), DayBucket::Today, "far future");

    // --- Yesterday: the previous civil day, any hour ---
    check(session_day_bucket(at(y, m, d, 0, 0) - kHourMs, noon), DayBucket::Yesterday,
          "23:00 yesterday");
    check(session_day_bucket(at(y, m, d - 1, 0, 0), noon), DayBucket::Yesterday, "00:00 yesterday");
    // Only the calendar day counts: 23:59 two days back is This week.
    check(session_day_bucket(at(y, m, d - 2, 23, 59), noon), DayBucket::ThisWeek,
          "day before yesterday is This week");

    // --- This week: Monday-based, excluding today and yesterday ---
    check(session_day_bucket(at(y, m, d - 2, 9), noon), DayBucket::ThisWeek, "Monday");
    // From Wednesday only Monday qualifies; exercise Tuesday-of-this-week
    // from a Friday anchor so the bucket has more than one member.
    {
        int fy, fm, fd;
        find_date(4 /* Friday */, fy, fm, fd);
        int64_t fri = at(fy, fm, fd, 12);
        check(session_day_bucket(at(fy, fm, fd - 1, 9), fri), DayBucket::Yesterday, "Fri: Thu");
        check(session_day_bucket(at(fy, fm, fd - 2, 9), fri), DayBucket::ThisWeek, "Fri: Wed");
        check(session_day_bucket(at(fy, fm, fd - 3, 9), fri), DayBucket::ThisWeek, "Fri: Tue");
        check(session_day_bucket(at(fy, fm, fd - 4, 9), fri), DayBucket::ThisWeek, "Fri: Mon");
        DayBucket sun = same_month(fy, fm, fd - 5) ? DayBucket::ThisMonth : DayBucket::Older;
        check(session_day_bucket(at(fy, fm, fd - 5, 9), fri), sun, "Fri: last Sunday");
    }
    // Regression: on a Wednesday, Monday is only two days back and the old
    // month-first ordering labeled it "This month" instead of "This week".
    check(session_day_bucket(at(y, m, d - 2, 23, 59), noon), DayBucket::ThisWeek,
          "Monday late night is This week, not This month");
    // Wednesday - 3 = Sunday of LAST week (this week's Monday is 2 days
    // back). Never This week; This month only if that Sunday is still in
    // the current calendar month, else Older.
    {
        DayBucket want = same_month(y, m, d - 3) ? DayBucket::ThisMonth : DayBucket::Older;
        check(session_day_bucket(at(y, m, d - 3, 9), noon), want, "last Sunday");
    }

    // --- This month: same calendar month, before this week ---
    {
        int64_t first = at(y, m, 1, 12);
        if (first < at(y, m, d - 2, 9))
            check(session_day_bucket(first, noon), DayBucket::ThisMonth, "the 1st, before Monday");
    }

    // --- Older ---
    check(session_day_bucket(at(y, m, d, 12) - 400 * kDayMs, noon), DayBucket::Older,
          "400 days ago");
    check(session_day_bucket(1, noon), DayBucket::Older, "1 ms after the epoch");
    check(session_day_bucket(0, noon), DayBucket::Older, "time 0 (never updated)");
    check(session_day_bucket(-1, noon), DayBucket::Older, "negative time");

    // --- Month boundary: on the 1st, the day before is still Yesterday ---
    {
        int64_t first_noon = at(y, m, 1, 12);
        check(session_day_bucket(at(y, m, 1, 0, 0) - kHourMs, first_noon), DayBucket::Yesterday,
              "yesterday across a month boundary");
    }

    // --- Week boundary on a Monday: Saturday and Sunday are last week ---
    {
        int wy, wm, wd;
        find_date(0 /* Monday */, wy, wm, wd);
        int64_t mon_noon = at(wy, wm, wd, 12);
        check(session_day_bucket(at(wy, wm, wd - 1, 12), mon_noon), DayBucket::Yesterday,
              "Monday: Sunday is Yesterday");
        DayBucket want = same_month(wy, wm, wd - 2) ? DayBucket::ThisMonth : DayBucket::Older;
        check(session_day_bucket(at(wy, wm, wd - 2, 12), mon_noon), want,
              "Monday: Saturday is never This week");
    }

    // --- DST: the calendar-day rule survives a 23- or 25-hour day ---
    {
        const int64_t kMinMs = 60 * 1000;
        for (int64_t probe = at(2026, 1, 1, 12); probe < at(2026, 12, 31, 12);
             probe += kDayMs) {
            time_t ta = static_cast<time_t>(probe / 1000);
            tm a{};
            localtime_r(&ta, &a);
            int64_t midnight = at(a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, 0, 0);
            int64_t midnight_next = at(a.tm_year + 1900, a.tm_mon + 1, a.tm_mday + 1, 0, 0);
            if (midnight_next - midnight == kDayMs) continue;  // not a DST day
            // Within the short/long day: 23:30 is Today relative to 23:45.
            check(session_day_bucket(midnight_next - 30 * kMinMs,
                                     midnight_next - 15 * kMinMs),
                  DayBucket::Today, "DST day: late evening is still Today");
            // The following day: 00:30 is Today relative to its 23:45.
            check(session_day_bucket(midnight_next + 30 * kMinMs,
                                     midnight_next + 23 * kHourMs + 45 * kMinMs),
                  DayBucket::Today, "day after DST: early morning is Today");
            // The DST day is Yesterday from noon the next day.
            check(session_day_bucket(midnight + 12 * kHourMs, midnight_next + 12 * kHourMs),
                  DayBucket::Yesterday, "DST day is Yesterday the day after");
            break;
        }
    }

    std::cout << "session_groups: all checks passed\n";
    return 0;
}
