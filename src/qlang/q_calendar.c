/* q_calendar.c — the q calendar home: date/time arithmetic shared by the
 * literal scanner (q_parse), q_fmt display and the cast/Tok paths.  Pure
 * value functions; contracts in q_registry.h (castcal split, 2026-07-22). */
#include "qlang/q_registry.h"
#include <stdint.h>

/* Hinnant days_from_civil (public domain, http://howardhinnant.github.io/
 * date_algorithms.html), rebased to the kdb/base date epoch: the algorithm
 * yields days since 1970-01-01, and 2000.01.01 is unix day 10957 (the same
 * constant base temporal.c uses in the inverse direction). */
int64_t q_calendar_days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                    /* [0,399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* [0,365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0,146096] */
    return era * 146097 + doe - 719468 - 10957;  /* unix days -> 2000.01.01 epoch */
}

/* kdb literal/value domain: 0001.01.01 .. 9999.12.31 (datatypes.md), real
 * month lengths, proleptic-Gregorian leap rule. */
int q_calendar_date_valid(int64_t y, int64_t m, int64_t d) {
    static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return 0;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d <= md[m - 1] + ((m == 2 && leap) ? 1 : 0);
}

/* Timestamp payload composition (see q_registry.h for the contract and the
 * boundary rationale). */
int q_calendar_ts_compose_checked(int64_t days, int64_t tod_ns, int64_t* out) {
    __int128 ns = (__int128)days * 86400000000000LL + tod_ns;
    if (ns > INT64_MAX || ns < -(__int128)INT64_MAX) return 0;
    *out = (int64_t)ns;
    return 1;
}
int64_t q_calendar_ts_compose(int64_t days, int64_t tod_ns) {
    int64_t ns;
    if (q_calendar_ts_compose_checked(days, tod_ns, &ns)) return ns;
    return ((__int128)days * 86400000000000LL + tod_ns) < 0 ? -INT64_MAX
                                                            : INT64_MAX;
}
