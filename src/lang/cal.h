/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_CAL_H
#define RAY_CAL_H

#include <math.h>
#include <stdint.h>

/* ===== Calendar and temporal-split primitives ===== */

#define RAY_DATE_EPOCH 2000
#define NS_PER_DAY 86400000000000LL

/* Cumulative days-in-month lookup: [leap][month].
 * Index 0 = Jan start (0 days), index 12 = Dec end (365 or 366). */
static const uint32_t MONTHDAYS[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

static inline int date_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static inline int32_t date_years_by_days(int yy) {
    return (int32_t)((int64_t)yy * 365 + yy / 4 - yy / 100 + yy / 400);
}

/* Decode: days-since-epoch → year/month/day */
static inline void date_to_ymd(int32_t days, int* y, int* m, int* d) {
    int32_t offset = (int32_t)((uint32_t)days + (uint32_t)date_years_by_days(RAY_DATE_EPOCH - 1));
    double approx = (double)offset / 365.2425;
    int32_t years = (int32_t)(approx >= 0.0 ? approx + 0.5 : approx - 0.5);

    if (date_years_by_days(years) > offset)
        years -= 1;

    int32_t rem = offset - date_years_by_days(years);
    int yy = years + 1;
    int leap = date_leap_year(yy);
    int mid = 0;

    for (mid = 12; mid > 0; mid--)
        if (MONTHDAYS[leap][mid] != 0 && rem / (int32_t)MONTHDAYS[leap][mid] != 0)
            break;

    if (mid == 12 || mid < 0)
        mid = 0;

    *y = yy;
    *m = 1 + mid % 12;
    *d = 1 + rem - (int32_t)MONTHDAYS[leap][mid];
}

/* Split ns-since-epoch into (day, ns-of-day in [0,NS_PER_DAY)).  C's / and %
 * truncate toward zero, which puts ns=-1 on 2000.01.01 instead of 1999.12.31. */
static inline int64_t ts_days_floor(int64_t ns) {
    int64_t q = ns / NS_PER_DAY;
    return ns - q * NS_PER_DAY < 0 ? q - 1 : q;
}

static inline int64_t ts_ns_in_day(int64_t ns) {
    int64_t r = ns % NS_PER_DAY;
    return r < 0 ? r + NS_PER_DAY : r;
}

/* days*NS_PER_DAY + tod_ns, computed exactly; 1 and *out when it fits i64. */
static inline int ts_compose_checked(int64_t days, int64_t tod_ns, int64_t* out) {
    __int128 ns = (__int128)days * NS_PER_DAY + tod_ns;
    if (ns > INT64_MAX || ns < -(__int128)INT64_MAX) return 0;
    *out = (int64_t)ns;
    return 1;
}

/* THE datetime split: an f64 day count -> (day, ns of day in [0,NS_PER_DAY)).
 * Round the ns product at the VALUE's magnitude — an f64 datetime resolves to
 * ~79ns at present-day scale, so that rounding is what recovers the instant
 * written (2022.03.14T12:30:00.000 is stored 26ns light) — then take every
 * narrower grain as the [) floor of it.  ref/cast.md:166 (narrowing truncates)
 * rules out rounding at ms; basics/precision.md:268 (a float 0.015ns below a ms
 * boundary still displays as that ms) rules out truncating the raw float.  The
 * day base is subtracted in __int128, so unlike a plain i64 ns the split keeps
 * the datetime's own 0001..9999 range (a timestamp reaches only 1707..2292).
 * A non-finite value has no day and takes the ±0Wp instant. */
#define DATETIME_DAY_MAX 9.0e15   /* past 2^53 a double no longer counts days one by one */

static inline void datetime_to_day_ns(double val, int64_t* day, int64_t* tod_ns) {
    if (!(val > -DATETIME_DAY_MAX && val < DATETIME_DAY_MAX)) {
        int64_t sat = val < 0 ? -INT64_MAX : INT64_MAX;   /* NaN takes +0Wp */
        *day = ts_days_floor(sat);
        *tod_ns = ts_ns_in_day(sat);
        return;
    }
    int64_t d = (int64_t)floor(val);
    __int128 tod = (__int128)round(val * (double)NS_PER_DAY) - (__int128)d * NS_PER_DAY;
    if (tod < 0) { d -= 1; tod += NS_PER_DAY; }                 /* rounded back a day */
    else if (tod >= NS_PER_DAY) { d += 1; tod -= NS_PER_DAY; }  /* rounded on a day */
    *day = d;
    *tod_ns = (int64_t)tod;
}

/* The datetime's integer form, saturating at ±0Wp (datatypes.md:146). */
static inline int64_t datetime_to_ns(double val) {
    int64_t day, tod, ns;
    datetime_to_day_ns(val, &day, &tod);
    if (ts_compose_checked(day, tod, &ns)) return ns;
    return day < 0 ? -INT64_MAX : INT64_MAX;
}

/* Encode: year/month/day → days-since-epoch */
static inline int32_t ymd_to_date(int year, int month, int day) {
    int yy = (year > 0) ? year - 1 : 0;
    int32_t ydays = date_years_by_days(yy);
    int leap = date_leap_year(year);
    int mm = (month > 0) ? month - 1 : 0;
    int32_t mdays = (int32_t)MONTHDAYS[leap][mm];
    return ydays - date_years_by_days(RAY_DATE_EPOCH - 1) + mdays + day - 1;
}

#endif /* RAY_CAL_H */
