#include <time.h>
#include <stdio.h>
#include <string.h>

time_t time(time_t *out)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (out)
        *out = ts.tv_sec;
    return ts.tv_sec;
}

static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    long secs = *t;
    long days = secs / 86400;
    long rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)((rem % 3600) / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((4 + days) % 7);
    if (out->tm_wday < 0) out->tm_wday += 7;
    int year = 1970;
    while (days >= (is_leap(year) ? 366 : 365)) { days -= is_leap(year) ? 366 : 365; year++; }
    while (days < 0) { year--; days += is_leap(year) ? 366 : 365; }
    out->tm_year = year - 1900;
    out->tm_yday = (int)days;
    int m = 0;
    while (m < 12) {
        int dim = days_in_month[m] + (m == 1 && is_leap(year));
        if (days < dim) break;
        days -= dim;
        m++;
    }
    out->tm_mon = m;
    out->tm_mday = (int)days + 1;
    out->tm_isdst = 0;
    return out;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return gmtime_r(t, &tm);
}

static const char *const wday_name[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon_name[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm)
{
    size_t pos = 0;
    char tmp[32];
    for (const char *p = fmt; *p && pos < max - 1; p++) {
        if (*p != '%') { buf[pos++] = *p; continue; }
        p++;
        const char *s = tmp;
        switch (*p) {
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'a': s = wday_name[tm->tm_wday]; break;
        case 'b': s = mon_name[tm->tm_mon]; break;
        case '%': s = "%"; break;
        default: tmp[0] = '%'; tmp[1] = *p; tmp[2] = '\0'; break;
        }
        while (*s && pos < max - 1)
            buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}
