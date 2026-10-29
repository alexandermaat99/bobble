#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include "ical.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define ICAL_ALLOC(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM)
#define ICAL_REALLOC(p, n) heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM)
#else
#define ICAL_ALLOC(n) malloc(n)
#define ICAL_REALLOC(p, n) realloc(p, n)
#endif

#define MAX_EXDATES 12
#define LINE_MAX 1024

enum { F_NONE, F_DAILY, F_WEEKLY, F_MONTHLY, F_YEARLY };

typedef struct {
    uint32_t uid;
    char title[80];
    time_t start, end, recur_id, until;
    bool all_day, has_end, cancelled, declined, free;
    // RRULE
    uint8_t freq, wkst, bymonth;
    int8_t bymonthday[8];        // BYMONTHDAY list (negative = from the end of the month)
    uint8_t n_bymonthday;
    int8_t bysetpos;             // pick the Nth (or -Nth) of the month's matching days
    uint16_t interval, count;
    uint8_t byday_mask;          // bit per weekday (0 = Sunday), plain "MO,WE"
    int8_t byday_nth;            // e.g. 2 for "2SU", -1 for "-1FR" (single entry)
    uint8_t byday_nth_wday;
    time_t exdates[MAX_EXDATES];
    uint8_t n_exdates;
} ev_t;

struct ical_parser {
    ev_t *ev;
    int n, cap;
    bool oom;                 // an event didn't fit in memory: the calendar's incomplete
    ev_t cur;
    int depth_event, depth_other;   // inside VEVENT / inside a nested component (VALARM, VTIMEZONE)
    char line[LINE_MAX];
    int line_len;
    bool line_pending, at_line_start;
    char me[96];                    // calendar owner e-mail, from X-WR-CALNAME
};

// ---------------------------------------------------------------- dates
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static int wday_of(int64_t days) { return (int)(((days % 7) + 11) % 7); }   // 1970-01-01 was a Thursday

static int days_in_month(int y, int m)
{
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : dm[m - 1];
}

static time_t local_time(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s, .tm_isdst = -1};
    return mktime(&tm);
}

// Local calendar day number of a timestamp
static int64_t local_day(time_t t, int *secs_of_day)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (secs_of_day) *secs_of_day = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

// "20260115", "20260115T090000" (local) or "20260115T160000Z" (UTC)
static time_t parse_dt(const char *v, bool *date_only)
{
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (sscanf(v, "%4d%2d%2d", &y, &mo, &d) != 3) return 0;
    bool has_time = v[8] == 'T';
    if (date_only) *date_only = !has_time;
    if (has_time) sscanf(v + 9, "%2d%2d%2d", &h, &mi, &s);
    if (has_time && v[15] == 'Z') return (time_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
    return local_time(y, mo, d, h, mi, s);
}

// ---------------------------------------------------------------- parsing
static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static int weekday_code(const char *s)
{
    static const char *codes[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; i++)
        if (!strncmp(s, codes[i], 2)) return i;
    return -1;
}

// Unescape TEXT and keep it printable ASCII (the display font has nothing else)
static void copy_text(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 1 < cap; in++) {
        unsigned char c = *in;
        if (c == '\\' && in[1]) {
            in++;
            c = (*in == 'n' || *in == 'N') ? ' ' : *in;
        }
        if (c == 0xE2 && (unsigned char)in[1] == 0x80 && in[2]) {   // typographic punctuation -> ASCII
            unsigned char d = in[2];
            char r = d == 0x98 || d == 0x99 ? '\'' : d == 0x9C || d == 0x9D ? '"' : d == 0x93 || d == 0x94 ? '-' : d == 0xA6 ? '.' : 0;
            if (r) { in += 2; c = r; }
        }
        if (c >= 0x80) continue;                       // drop other UTF-8 (emoji, accents)
        if (c < 0x20) c = ' ';
        if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static void parse_rrule(ev_t *e, const char *v)
{
    e->interval = 1;
    e->wkst = 1;
    char buf[256];
    strncpy(buf, v, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *save = NULL;
    for (char *part = strtok_r(buf, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = part, *val = eq + 1;
        if (!strcmp(k, "FREQ")) {
            e->freq = !strcmp(val, "DAILY") ? F_DAILY : !strcmp(val, "WEEKLY") ? F_WEEKLY
                    : !strcmp(val, "MONTHLY") ? F_MONTHLY : !strcmp(val, "YEARLY") ? F_YEARLY : F_NONE;
        } else if (!strcmp(k, "INTERVAL")) {
            e->interval = atoi(val) > 0 ? atoi(val) : 1;
        } else if (!strcmp(k, "COUNT")) {
            e->count = atoi(val);
        } else if (!strcmp(k, "UNTIL")) {
            bool date_only;
            e->until = parse_dt(val, &date_only);
            if (date_only) e->until += 86399;          // whole day included
        } else if (!strcmp(k, "WKST")) {
            int w = weekday_code(val);
            if (w >= 0) e->wkst = w;
        } else if (!strcmp(k, "BYMONTH")) {
            e->bymonth = atoi(val);
        } else if (!strcmp(k, "BYMONTHDAY")) {
            const char *v = val;
            while (*v && e->n_bymonthday < 8) {
                e->bymonthday[e->n_bymonthday++] = (int8_t)atoi(v);
                while (*v && *v != ',') v++;
                if (*v == ',') v++;
            }
        } else if (!strcmp(k, "BYSETPOS")) {
            e->bysetpos = (int8_t)atoi(val);
        } else if (!strcmp(k, "BYDAY")) {
            char db[64];
            strncpy(db, val, sizeof db - 1);
            db[sizeof db - 1] = 0;
            char *s2 = NULL;
            for (char *d = strtok_r(db, ",", &s2); d; d = strtok_r(NULL, ",", &s2)) {
                int n = 0;
                char *p = d;
                if (*p == '-' || *p == '+' || isdigit((unsigned char)*p)) n = (int)strtol(d, &p, 10);
                int w = weekday_code(p);
                if (w < 0) continue;
                if (n) { e->byday_nth = n; e->byday_nth_wday = w; }
                else e->byday_mask |= 1 << w;
            }
        }
    }
}

static void end_event(ical_parser_t *p)
{
    ev_t *e = &p->cur;
    if (!e->start) return;
    if (!e->has_end) e->end = e->all_day ? e->start + 86400 : e->start;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 128;
        ev_t *grown = ICAL_REALLOC(p->ev, cap * sizeof(ev_t));
        if (!grown) { p->oom = true; return; }
        p->ev = grown;
        p->cap = cap;
    }
    p->ev[p->n++] = *e;
}

static void handle_line(ical_parser_t *p, char *line)
{
    // split "NAME;PARAMS:VALUE" (a ':' inside quoted parameters doesn't count)
    char *value = NULL;
    bool quoted = false;
    for (char *c = line; *c; c++) {
        if (*c == '"') quoted = !quoted;
        else if (*c == ':' && !quoted) { *c = 0; value = c + 1; break; }
    }
    if (!value) return;
    char *params = strchr(line, ';');
    if (params) *params++ = 0;
    const char *name = line;

    if (!strcmp(name, "BEGIN")) {
        if (!strcmp(value, "VEVENT") && !p->depth_event) {
            p->depth_event = 1;
            memset(&p->cur, 0, sizeof p->cur);
        } else if (strcmp(value, "VCALENDAR")) {
            p->depth_other++;
        }
        return;
    }
    if (!strcmp(name, "END")) {
        if (!strcmp(value, "VEVENT") && p->depth_event && !p->depth_other) {
            end_event(p);
            p->depth_event = 0;
        } else if (strcmp(value, "VCALENDAR") && p->depth_other > 0) {
            p->depth_other--;
        }
        return;
    }
    if (!p->depth_event) {
        if (!strcmp(name, "X-WR-CALNAME") && strchr(value, '@')) strncpy(p->me, value, sizeof p->me - 1);
        return;
    }
    if (p->depth_other) return;    // VALARM etc. inside the event

    ev_t *e = &p->cur;
    if (!strcmp(name, "UID")) e->uid = hash(value);
    else if (!strcmp(name, "SUMMARY")) copy_text(e->title, sizeof e->title, value);
    else if (!strcmp(name, "DTSTART")) e->start = parse_dt(value, &e->all_day);
    else if (!strcmp(name, "DTEND")) { e->end = parse_dt(value, NULL); e->has_end = true; }
    else if (!strcmp(name, "RECURRENCE-ID")) e->recur_id = parse_dt(value, NULL);
    else if (!strcmp(name, "RRULE")) parse_rrule(e, value);
    else if (!strcmp(name, "STATUS")) e->cancelled = !strcmp(value, "CANCELLED");
    else if (!strcmp(name, "TRANSP")) e->free = !strcmp(value, "TRANSPARENT");
    else if (!strcmp(name, "EXDATE")) {
        char *s2 = NULL;
        for (char *d = strtok_r(value, ",", &s2); d && e->n_exdates < MAX_EXDATES; d = strtok_r(NULL, ",", &s2))
            e->exdates[e->n_exdates++] = parse_dt(d, NULL);
    } else if (!strcmp(name, "ATTENDEE") && params && p->me[0] && strcasestr(value, p->me)) {
        if (strstr(params, "PARTSTAT=DECLINED")) e->declined = true;
    }
}

static void flush_line(ical_parser_t *p)
{
    if (!p->line_pending) return;
    p->line[p->line_len] = 0;
    handle_line(p, p->line);
    p->line_len = 0;
    p->line_pending = false;
}

ical_parser_t *ical_new(void)
{
    ical_parser_t *p = ICAL_ALLOC(sizeof *p);
    if (p) {
        memset(p, 0, sizeof *p);
        p->at_line_start = true;
    }
    return p;
}

// Lines may be folded: a line starting with a space or tab continues the previous one
void ical_feed(ical_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') { p->at_line_start = true; continue; }
        if (p->at_line_start) {
            p->at_line_start = false;
            if (c == ' ' || c == '\t') continue;     // continuation of the previous line
            flush_line(p);
            p->line_pending = true;
        }
        if (p->line_len < LINE_MAX - 1) p->line[p->line_len++] = c;
    }
}

void ical_finish(ical_parser_t *p) { flush_line(p); }

int ical_event_count(const ical_parser_t *p) { return p->n; }
bool ical_complete(const ical_parser_t *p) { return !p->oom; }

void ical_free(ical_parser_t *p)
{
    if (!p) return;
    free(p->ev);
    free(p);
}

// ---------------------------------------------------------------- recurrence
static bool nth_weekday_match(int y, int m, int d, int64_t day, int nth, int wday)
{
    if (wday_of(day) != wday) return false;
    if (nth > 0) return (d - 1) / 7 + 1 == nth;
    return (days_in_month(y, m) - d) / 7 + 1 == -nth;
}

// Does the rule produce an occurrence on local day `day`? (ignores UNTIL/COUNT)
static bool rule_matches(const ev_t *e, int64_t start_day, int64_t day)
{
    if (day < start_day) return false;
    int sy, sm, sd, y, m, d;
    civil_from_days(start_day, &sy, &sm, &sd);
    civil_from_days(day, &y, &m, &d);
    int wd = wday_of(day);
    switch (e->freq) {
    case F_DAILY:
        if (e->byday_mask && !(e->byday_mask & (1 << wd))) return false;
        return (day - start_day) % e->interval == 0;
    case F_WEEKLY: {
        uint8_t mask = e->byday_mask ? e->byday_mask : (1 << wday_of(start_day));
        if (!(mask & (1 << wd))) return false;
        int64_t ws0 = start_day - (wday_of(start_day) - e->wkst + 7) % 7;
        int64_t ws = day - (wd - e->wkst + 7) % 7;
        return ((ws - ws0) / 7) % e->interval == 0;
    }
    case F_MONTHLY: {
        int months = (y - sy) * 12 + (m - sm);
        if (months % e->interval) return false;
        // the month's candidate days: every filter given must agree (BYMONTHDAY, BYDAY, nth BYDAY)
        int dim = days_in_month(y, m), cand[31], nc = 0;
        int64_t first = day - (d - 1);
        for (int dd = 1; dd <= dim; dd++) {
            int64_t dday = first + dd - 1;
            bool ok = true;
            if (e->n_bymonthday) {
                bool hit = false;
                for (int k = 0; k < e->n_bymonthday && !hit; k++) {
                    int want = e->bymonthday[k] > 0 ? e->bymonthday[k] : dim + e->bymonthday[k] + 1;
                    hit = dd == want;
                }
                ok = hit;
            }
            if (ok && e->byday_mask) ok = e->byday_mask & (1 << wday_of(dday));
            if (ok && e->byday_nth) ok = nth_weekday_match(y, m, dd, dday, e->byday_nth, e->byday_nth_wday);
            if (ok && !e->n_bymonthday && !e->byday_mask && !e->byday_nth) ok = dd == sd;
            if (ok) cand[nc++] = dd;
        }
        if (!nc) return false;
        if (e->bysetpos) {                            // e.g. -1: only the last matching day
            int i = e->bysetpos > 0 ? e->bysetpos - 1 : nc + e->bysetpos;
            return i >= 0 && i < nc && d == cand[i];
        }
        for (int i = 0; i < nc; i++)
            if (cand[i] == d) return true;
        return false;
    }
    case F_YEARLY: {
        if ((y - sy) % e->interval) return false;
        if (m != (e->bymonth ? e->bymonth : sm)) return false;
        if (e->byday_nth) return nth_weekday_match(y, m, d, day, e->byday_nth, e->byday_nth_wday);
        if (e->n_bymonthday) return d == e->bymonthday[0];
        return d == sd;
    }
    }
    return false;
}

static bool overridden(const ical_parser_t *p, uint32_t uid, time_t occ)
{
    for (int i = 0; i < p->n; i++)
        if (p->ev[i].recur_id == occ && p->ev[i].uid == uid) return true;
    return false;
}

static int add_item(ical_item_t *out, int n, int max, const ev_t *e, time_t s, time_t en)
{
    if (n >= max) return n;
    out[n].start = s;
    out[n].end = en;
    out[n].all_day = e->all_day;
    out[n].free = e->free;
    strncpy(out[n].title, e->title[0] ? e->title : "(No title)", sizeof out[n].title - 1);
    out[n].title[sizeof out[n].title - 1] = 0;
    return n + 1;
}

static int cmp_items(const void *a, const void *b)
{
    const ical_item_t *x = a, *y = b;
    if (x->all_day != y->all_day) return x->all_day ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return strcmp(x->title, y->title);
}

int ical_expand(const ical_parser_t *p, time_t from, time_t to, ical_item_t *out, int max)
{
    int n = 0;
    int64_t first_day = local_day(from, NULL) - 1, last_day = local_day(to, NULL);   // -1: events running past midnight
    for (int i = 0; i < p->n; i++) {
        const ev_t *e = &p->ev[i];
        if (e->cancelled || e->declined) continue;
        time_t dur = e->end - e->start;
        if (!e->freq || e->recur_id) {                 // single event or a moved occurrence
            if (e->start < to && (e->end > from || (e->end == e->start && e->start >= from)))
                n = add_item(out, n, max, e, e->start, e->end);
            continue;
        }
        int secs;
        int64_t start_day = local_day(e->start, &secs);
        int64_t from_day = first_day > start_day ? first_day : start_day;
        int count_seen = 0;
        if (e->count)                                  // occurrences before the window count towards COUNT
            for (int64_t d = start_day; d < from_day; d++) count_seen += rule_matches(e, start_day, d);
        for (int64_t d = from_day; d <= last_day; d++) {
            if (!rule_matches(e, start_day, d)) continue;
            if (e->count && ++count_seen > e->count) break;
            int y, m, dd;
            civil_from_days(d, &y, &m, &dd);
            time_t occ = e->all_day ? local_time(y, m, dd, 0, 0, 0)
                                    : local_time(y, m, dd, secs / 3600, secs / 60 % 60, secs % 60);
            if (e->until && occ > e->until) break;
            time_t occ_end = e->all_day ? local_time(y, m, dd + (int)((dur + 43200) / 86400), 0, 0, 0) : occ + dur;
            if (occ >= to || occ_end <= from) continue;
            bool skip = false;
            for (int x = 0; x < e->n_exdates && !skip; x++) skip = e->exdates[x] == occ;
            if (skip || overridden(p, e->uid, occ)) continue;
            n = add_item(out, n, max, e, occ, occ_end);
        }
    }
    qsort(out, n, sizeof out[0], cmp_items);
    return n;
}
