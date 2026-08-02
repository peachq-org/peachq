/* q_calendar — the shared civil-calendar core (date/clock composition) both
 * the literal scanner (q_tok/q_parse) and `$` (q_dollar) sit on.  Pure value
 * functions, no runtime state (castcal split, 2026-07-22). */
#ifndef Q_CALENDAR_H
#define Q_CALENDAR_H

#include <stdint.h>

/* kdb date == base RAY_DATE payload: i32 days since 2000.01.01, proleptic
 * Gregorian (qdocs basics/datatypes.md).  q_calendar_days_from_civil is Hinnant's
 * days_from_civil (public domain) rebased from the unix epoch by -10957;
 * q_calendar_date_valid pins the kdb literal domain 0001.01.01..9999.12.31 with real
 * (leap-aware) month lengths.  Shared by the literal scanner (q_parse) and
 * "D"$ Tok (q_dollar_tok) — ONE conversion home, per q_dollar.h's mandate. */
int64_t q_calendar_days_from_civil(int64_t y, int64_t m, int64_t d);
int     q_calendar_date_valid(int64_t y, int64_t m, int64_t d);

/* Timestamp payload composition: days*NS_PER_DAY + tod_ns, computed EXACTLY
 * (__int128).  q_calendar_ts_compose_checked returns 1 and fills *out when the exact
 * value lies in [-INT64_MAX, INT64_MAX] ("P"$ Tok maps failure to 0Np, the
 * ref/tok.md out-of-domain contract); q_calendar_ts_compose is the saturating form
 * for the literal scanner and `timestamp$ casts (out-of-range clamps to the
 * kdb inf sentinels +-INT64_MAX == +-0Wp: datatypes.md pins
 * `timestamp$1666.09.02 -> -0Wp).  The mul must NOT saturate before the add:
 * the doc-pinned minimum 1707.09.22D00:12:43.145224194 has a day component
 * that alone underflows i64 and re-enters range via the time of day. */
int     q_calendar_ts_compose_checked(int64_t days, int64_t tod_ns, int64_t* out);
int64_t q_calendar_ts_compose(int64_t days, int64_t tod_ns);

/* Monday-of-week for a day count since 2000.01.01 (ref/cast.md:138 ``week``):
 * the start of the week the date resides in; a Monday returns unchanged.
 * Calendar (year/mm/dd) and clock (hh/uu/ss) decode reuse the base
 * ray_temporal_extract / a signed inline division; `week` has no base field. */
int64_t q_calendar_week_start(int64_t days);

#endif /* Q_CALENDAR_H */
