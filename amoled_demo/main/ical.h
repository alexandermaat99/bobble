#pragma once
// Minimal iCalendar (.ics) reader: feed it the file in chunks, then expand the events for a
// time window. Handles what Google Calendar exports: DATE / TZID (treated as local time, so
// set TZ) / UTC times, RRULE (DAILY/WEEKLY/MONTHLY/YEARLY with INTERVAL, UNTIL, COUNT,
// BYDAY, BYMONTHDAY, BYMONTH), EXDATE, moved/cancelled single occurrences (RECURRENCE-ID),
// and invitations you declined. Plain C so it can be tested on a desktop too.
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct {
    time_t start, end;
    bool all_day;
    unsigned char cal;    // which calendar it came from (set by the caller)
    bool free;            // marked "Free" (TRANSP:TRANSPARENT): not a meeting you're busy in
    char title[80];
} ical_item_t;

typedef struct ical_parser ical_parser_t;

ical_parser_t *ical_new(void);
void ical_feed(ical_parser_t *p, const char *data, size_t len);
void ical_finish(ical_parser_t *p);
int ical_event_count(const ical_parser_t *p);
bool ical_complete(const ical_parser_t *p);   // false: some events didn't fit in memory (better keep an older copy)
// Occurrences overlapping [from, to), sorted by start (all-day first). Returns how many were written.
int ical_expand(const ical_parser_t *p, time_t from, time_t to, ical_item_t *out, int max);
void ical_free(ical_parser_t *p);
