#include "calendar.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static int g_event_id_counter = 1;

bool is_leap_year(int year) {
    if (year % 400 == 0) return true;
    if (year % 100 == 0) return false;
    return (year % 4 == 0);
}

int days_in_month(int year, int month) {
    static const int days_normal[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && is_leap_year(year)) return 29;
    return days_normal[month];
}

// Sakamoto's algorithm: returns 0 = Sunday, 1 = Monday, ..., 6 = Saturday
int get_day_of_week(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// Days from a fixed reference epoch (0001-01-01) for exact day/week intervals
static long days_since_epoch(int y, int m, int d) {
    long y_m1 = y - 1;
    long total_days = y_m1 * 365 + y_m1 / 4 - y_m1 / 100 + y_m1 / 400;
    for (int month_idx = 1; month_idx < m; month_idx++) {
        total_days += days_in_month(y, month_idx);
    }
    total_days += (d - 1);
    return total_days;
}

void calendar_init(CalendarState* cal, int year, int month) {
    cal->current_year = year;
    cal->current_month = month;
    cal->selected_day = 1;
    cal->events_head = NULL;
    cal->events_tail = NULL;
    cal->calendar_name[0] = '\0';
}

Event* calendar_add_event(CalendarState* cal, const char* title, const char* desc,
                          int year, int month, int day, RecurrenceRule* rule) {
    char default_uid[MAX_UID_LEN];
    snprintf(default_uid, sizeof(default_uid), "microcal-%u-%d@local",
             (unsigned int)time(NULL), g_event_id_counter);

    char default_stamp[MAX_TIMESTAMP_LEN];
    time_t now = time(NULL);
    struct tm* gmt = gmtime(&now);
    strftime(default_stamp, sizeof(default_stamp), "%Y%m%dT%H%M%SZ", gmt);

    // Default all-day exclusive end date is day + 1
    int end_y = year, end_m = month, end_d = day + 1;
    int max_d = days_in_month(year, month);
    if (end_d > max_d) {
        end_d = 1;
        end_m++;
        if (end_m > 12) {
            end_m = 1;
            end_y++;
        }
    }

    return calendar_add_event_full(cal, default_uid, default_stamp, title, desc, "",
                                  year, month, day, end_y, end_m, end_d,
                                  true, 0, rule);
}

Event* calendar_add_event_full(CalendarState* cal, const char* uid, const char* dtstamp,
                               const char* title, const char* desc, const char* category,
                               int year, int month, int day,
                               int end_year, int end_month, int end_day,
                               bool is_transparent, int sequence,
                               RecurrenceRule* rule) {
    Event* event = (Event*)malloc(sizeof(Event));
    if (!event) return NULL;

    event->id = g_event_id_counter++;
    strncpy(event->uid, uid, MAX_UID_LEN - 1);
    event->uid[MAX_UID_LEN - 1] = '\0';

    strncpy(event->dtstamp, dtstamp, MAX_TIMESTAMP_LEN - 1);
    event->dtstamp[MAX_TIMESTAMP_LEN - 1] = '\0';

    strncpy(event->title, title, MAX_TITLE_LEN - 1);
    event->title[MAX_TITLE_LEN - 1] = '\0';

    if (desc) {
        strncpy(event->description, desc, MAX_DESC_LEN - 1);
        event->description[MAX_DESC_LEN - 1] = '\0';
    } else {
        event->description[0] = '\0';
    }

    if (category) {
        strncpy(event->category, category, MAX_CATEGORY_LEN - 1);
        event->category[MAX_CATEGORY_LEN - 1] = '\0';
    } else {
        event->category[0] = '\0';
    }

    event->year = year;
    event->month = month;
    event->day = day;
    event->end_year = end_year;
    event->end_month = end_month;
    event->end_day = end_day;
    event->is_transparent = is_transparent;
    event->sequence = sequence;

    if (rule) {
        event->is_recurring = true;
        event->recurrence = *rule;
        if (event->recurrence.interval <= 0) event->recurrence.interval = 1;
    } else {
        event->is_recurring = false;
        memset(&event->recurrence, 0, sizeof(RecurrenceRule));
    }

    event->next = NULL;

    if (cal->events_tail) {
        cal->events_tail->next = event;
        cal->events_tail = event;
    } else {
        cal->events_head = event;
        cal->events_tail = event;
    }

    return event;
}

bool calendar_delete_event(CalendarState* cal, int event_id) {
    Event* curr = cal->events_head;
    Event* prev = NULL;

    while (curr != NULL) {
        if (curr->id == event_id) {
            if (prev == NULL) {
                cal->events_head = curr->next;
            } else {
                prev->next = curr->next;
            }
            if (curr == cal->events_tail) {
                cal->events_tail = prev;
            }
            free(curr);
            return true;
        }
        prev = curr;
        curr = curr->next;
    }
    return false;
}

int calendar_get_events_for_day(const CalendarState* cal, int year, int month, int day,
                                Event* out_events[], int max_events) {
    int count = 0;
    Event* curr = cal->events_head;
    while (curr && count < max_events) {
        if (event_occurs_on(curr, year, month, day)) {
            out_events[count++] = curr;
        }
        curr = curr->next;
    }
    return count;
}

// Day numbers are days since 0001-01-01 (days_since_epoch); 0001-01-01 was a Monday
#define UNIX_EPOCH_DAY_NUMBER 719162L

static void ymd_from_day_number(long n, int* y, int* m, int* d) {
    // Howard Hinnant's civil_from_days, shifted to the 0001-01-01 origin
    long z = n - UNIX_EPOCH_DAY_NUMBER + 719468L;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int weekday_of_day_number(long n) {
    return (int)((n + 1) % 7);
}

static long week_start_day_number(long n, int wkst) {
    return n - ((weekday_of_day_number(n) - wkst + 7) % 7);
}

static int interval_of(const Event* ev) {
    return ev->recurrence.interval > 0 ? ev->recurrence.interval : 1;
}

// RFC 5545 default WKST is Monday when unspecified
static int resolved_wkst(const Event* ev) {
    return ev->recurrence.wkst == 0 ? 0 : 1;
}

static bool date_after(int y1, int m1, int d1, int y2, int m2, int d2) {
    if (y1 != y2) return y1 > y2;
    if (m1 != m2) return m1 > m2;
    return d1 > d2;
}

// Index of the recurrence period containing (y, m, d), counted from the DTSTART period
static long period_index(const Event* ev, int y, int m, int d) {
    const RecurrenceRule* r = &ev->recurrence;
    switch (r->type) {
    case RECUR_DAILY:
        return days_since_epoch(y, m, d) - days_since_epoch(ev->year, ev->month, ev->day);
    case RECUR_WEEKLY: {
        int wk = resolved_wkst(ev);
        long start_ws = week_start_day_number(days_since_epoch(ev->year, ev->month, ev->day), wk);
        long target_ws = week_start_day_number(days_since_epoch(y, m, d), wk);
        return (target_ws - start_ws) / 7;
    }
    case RECUR_MONTHLY:
        return (long)(y - ev->year) * 12 + (m - ev->month);
    case RECUR_YEARLY:
        return (long)(y - ev->year);
    default:
        return 0;
    }
}

// Day-number bounds [lo, hi] of recurrence period k (k counted from the DTSTART period)
static void period_bounds(const Event* ev, long k, long* lo, long* hi) {
    const RecurrenceRule* r = &ev->recurrence;
    long start = days_since_epoch(ev->year, ev->month, ev->day);
    switch (r->type) {
    case RECUR_DAILY:
        *lo = start + k;
        *hi = *lo;
        break;
    case RECUR_WEEKLY:
        *lo = week_start_day_number(start, resolved_wkst(ev)) + 7 * k;
        *hi = *lo + 6;
        break;
    case RECUR_MONTHLY: {
        long total = (long)ev->year * 12 + (ev->month - 1) + k;
        int y = (int)(total / 12);
        int m = (int)(total % 12) + 1;
        *lo = days_since_epoch(y, m, 1);
        *hi = days_since_epoch(y, m, days_in_month(y, m));
        break;
    }
    case RECUR_YEARLY: {
        int y = ev->year + (int)k;
        *lo = days_since_epoch(y, 1, 1);
        *hi = days_since_epoch(y, 12, 31);
        break;
    }
    default:
        *lo = start;
        *hi = start - 1;
        break;
    }
}

int recurrence_ordinal_bit(int nth) {
    if (nth >= 1 && nth <= 5) return 1 << (nth - 1);
    if (nth <= -1 && nth >= -5) return 1 << (5 + (-nth - 1));
    return 0;
}

void calendar_next_day(int* year, int* month, int* day) {
    if (*day < days_in_month(*year, *month)) {
        (*day)++;
        return;
    }
    *day = 1;
    if (*month < 12) {
        (*month)++;
    } else {
        *month = 1;
        (*year)++;
    }
}

// BYDAY ordinal check (e.g. 2TU, -1FR) within the month, or within the year when year_scope is set
static bool weekday_ordinal_matches(const Event* ev, int y, int m, int d, bool year_scope) {
    const RecurrenceRule* r = &ev->recurrence;
    int wd = get_day_of_week(y, m, d);
    if (!(r->day_of_week_mask & (1 << wd))) return false;

    int mask = r->ordinal_mask[wd];
    if (mask == 0 || (mask & ORDINAL_ALL)) return true;

    int doy = d;
    int len = days_in_month(y, m);
    if (year_scope) {
        doy = (int)(days_since_epoch(y, m, d) - days_since_epoch(y, 1, 1)) + 1;
        len = is_leap_year(y) ? 366 : 365;
    }
    int from_start = (doy - 1) / 7 + 1;
    int from_end = (len - doy) / 7 + 1;
    return (mask & recurrence_ordinal_bit(from_start)) || (mask & recurrence_ordinal_bit(-from_end));
}

// BYMONTHDAY and BYDAY both limit the set (intersection); with neither, the DTSTART day is used
static bool month_day_filter(const Event* ev, int y, int m, int d, bool year_scope) {
    const RecurrenceRule* r = &ev->recurrence;
    bool has_mday = r->day_of_month > 0;
    bool has_byday = r->day_of_week_mask != 0;

    if (!has_mday && !has_byday) return d == ev->day;
    if (has_mday && d != r->day_of_month) return false;
    if (has_byday && !weekday_ordinal_matches(ev, y, m, d, year_scope)) return false;
    return true;
}

static bool day_passes_filter(const Event* ev, int y, int m, int d) {
    const RecurrenceRule* r = &ev->recurrence;
    switch (r->type) {
    case RECUR_DAILY:
        return r->day_of_week_mask == 0 || (r->day_of_week_mask & (1 << get_day_of_week(y, m, d)));
    case RECUR_WEEKLY: {
        int wd = get_day_of_week(y, m, d);
        if (r->day_of_week_mask == 0) {
            return wd == get_day_of_week(ev->year, ev->month, ev->day);
        }
        return (r->day_of_week_mask & (1 << wd)) != 0;
    }
    case RECUR_MONTHLY:
        return month_day_filter(ev, y, m, d, false);
    case RECUR_YEARLY: {
        bool constrained = r->day_of_month > 0 || r->day_of_week_mask != 0;
        if (r->month_of_year > 0 || !constrained) {
            int target_month = r->month_of_year > 0 ? r->month_of_year : ev->month;
            if (m != target_month) return false;
        }
        return month_day_filter(ev, y, m, d, r->month_of_year == 0);
    }
    default:
        return false;
    }
}

// Occurrences in [DTSTART, target] up to (and including) period p_target; stops early once COUNT is exceeded
static long count_occurrences_through(const Event* ev, int y, int m, int d, long p_target) {
    const RecurrenceRule* r = &ev->recurrence;
    long interval = interval_of(ev);
    long start = days_since_epoch(ev->year, ev->month, ev->day);
    long target = days_since_epoch(y, m, d);

    if (r->type == RECUR_DAILY && r->day_of_week_mask == 0) {
        return p_target / interval + 1;
    }

    long total = 0;
    for (long k = 0; k <= p_target; k += interval) {
        long lo, hi;
        period_bounds(ev, k, &lo, &hi);
        if (lo < start) lo = start;
        if (hi > target) hi = target;
        for (long n = lo; n <= hi; n++) {
            int yy, mm, dd;
            ymd_from_day_number(n, &yy, &mm, &dd);
            if (day_passes_filter(ev, yy, mm, dd)) total++;
        }
        if (total > r->count) break;
    }
    return total;
}

bool event_occurs_on(const Event* event, int year, int month, int day) {
    if (!event->is_recurring) {
        return (event->year == year && event->month == month && event->day == day);
    }

    const RecurrenceRule* r = &event->recurrence;
    if (r->type == RECUR_NONE) return false;
    if (days_since_epoch(year, month, day) < days_since_epoch(event->year, event->month, event->day)) {
        return false;
    }
    if (r->has_until && date_after(year, month, day, r->until_year, r->until_month, r->until_day)) {
        return false;
    }

    long p = period_index(event, year, month, day);
    if (p % interval_of(event) != 0) return false;
    if (!day_passes_filter(event, year, month, day)) return false;
    if (r->count <= 0) return true;

    // DTSTART is occurrence 1 when it matches the rule, so the target's occurrence number must be within COUNT
    return count_occurrences_through(event, year, month, day, p) <= r->count;
}

void calendar_free_events(CalendarState* cal) {
    Event* current = cal->events_head;
    while (current != NULL) {
        Event* next = current->next;
        free(current);
        current = next;
    }
    cal->events_head = NULL;
    cal->events_tail = NULL;
}