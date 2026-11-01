#include "calendar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int days_in_month(int year, int month) {
    if (month < 1 || month > 12) return 0;
    static const int days[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && is_leap_year(year)) return 29;
    return days[month];
}

int get_day_of_week(int year, int month, int day) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

void calendar_init(CalendarState* cal, int year, int month) {
    cal->current_year = year;
    cal->current_month = month;
    cal->selected_day = 1;
    cal->events_head = NULL;
    cal->events_tail = NULL;
    cal->calendar_name[0] = '\0';
}

static void generate_utc_timestamp(char* buf, size_t max_len) {
    time_t now = time(NULL);
    struct tm* gmt = gmtime(&now);
    if (gmt) {
        strftime(buf, max_len, "%Y%m%dT%H%M%SZ", gmt);
    } else {
        snprintf(buf, max_len, "20261101T000000Z");
    }
}

Event* calendar_add_event_full(CalendarState* cal, const char* uid, const char* dtstamp,
                               const char* title, const char* desc, const char* category,
                               int year, int month, int day,
                               int end_year, int end_month, int end_day,
                               bool is_transparent, int sequence,
                               RecurrenceRule* rule) {
    Event* new_event = (Event*)malloc(sizeof(Event));
    if (!new_event) return NULL;

    static int next_id = 1;
    new_event->id = next_id++;

    if (uid && strlen(uid) > 0) {
        strncpy(new_event->uid, uid, MAX_UID_LEN - 1);
        new_event->uid[MAX_UID_LEN - 1] = '\0';
    } else {
        snprintf(new_event->uid, sizeof(new_event->uid), "microcal-%ld-%d@local", (long)time(NULL), new_event->id);
    }

    if (dtstamp && strlen(dtstamp) > 0) {
        strncpy(new_event->dtstamp, dtstamp, MAX_TIMESTAMP_LEN - 1);
        new_event->dtstamp[MAX_TIMESTAMP_LEN - 1] = '\0';
    } else {
        generate_utc_timestamp(new_event->dtstamp, sizeof(new_event->dtstamp));
    }

    strncpy(new_event->title, title ? title : "Untitled Event", MAX_TITLE_LEN - 1);
    new_event->title[MAX_TITLE_LEN - 1] = '\0';

    strncpy(new_event->description, desc ? desc : "", MAX_DESC_LEN - 1);
    new_event->description[MAX_DESC_LEN - 1] = '\0';

    strncpy(new_event->category, category ? category : "", MAX_CATEGORY_LEN - 1);
    new_event->category[MAX_CATEGORY_LEN - 1] = '\0';

    new_event->year = year;
    new_event->month = month;
    new_event->day = day;

    new_event->end_year = end_year;
    new_event->end_month = end_month;
    new_event->end_day = end_day;

    new_event->is_transparent = is_transparent;
    new_event->sequence = sequence;

    if (rule) {
        new_event->is_recurring = true;
        new_event->recurrence = *rule;
        if (new_event->recurrence.interval <= 0) {
            new_event->recurrence.interval = 1;
        }
    } else {
        new_event->is_recurring = false;
        memset(&new_event->recurrence, 0, sizeof(RecurrenceRule));
        new_event->recurrence.type = RECUR_NONE;
    }

    new_event->next = NULL;

    if (cal->events_tail == NULL) {
        cal->events_head = new_event;
        cal->events_tail = new_event;
    } else {
        cal->events_tail->next = new_event;
        cal->events_tail = new_event;
    }

    return new_event;
}

Event* calendar_add_event(CalendarState* cal, const char* title, const char* desc,
                          int year, int month, int day, RecurrenceRule* rule) {
    // RFC 5545 §3.8.2.2: DTEND for date-only values is non-inclusive (exclusive)
    int next_d = day + 1;
    int next_m = month;
    int next_y = year;
    if (next_d > days_in_month(year, month)) {
        next_d = 1;
        next_m++;
        if (next_m > 12) {
            next_m = 1;
            next_y++;
        }
    }

    return calendar_add_event_full(cal, NULL, NULL, title, desc, NULL,
                                   year, month, day, next_y, next_m, next_d,
                                   true, 0, rule);
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

bool event_occurs_on(const Event* event, int year, int month, int day) {
    if (year < event->year) return false;
    if (year == event->year && month < event->month) return false;
    if (year == event->year && month == event->month && day < event->day) return false;

    if (!event->is_recurring) {
        return (event->year == year && event->month == month && event->day == day);
    }

    int interval = event->recurrence.interval > 0 ? event->recurrence.interval : 1;

    switch (event->recurrence.type) {
    case RECUR_DAILY:
        return true;
    case RECUR_WEEKLY: {
        int target_wday = get_day_of_week(year, month, day);
        if (event->recurrence.day_of_week_mask & (1 << target_wday)) {
            return true;
        }
        return false;
    }
    case RECUR_MONTHLY: {
        int month_diff = (year - event->year) * 12 + (month - event->month);
        if (month_diff % interval != 0) return false;

        int target_day = event->recurrence.day_of_month > 0 ? event->recurrence.day_of_month : event->day;
        int max_d = days_in_month(year, month);
        if (target_day > max_d) target_day = max_d;
        return (day == target_day);
    }
    case RECUR_YEARLY: {
        int year_diff = year - event->year;
        if (year_diff % interval != 0) return false;

        int target_month = event->recurrence.month_of_year > 0 ? event->recurrence.month_of_year : event->month;
        if (month != target_month) return false;

        int target_day = event->recurrence.day_of_month > 0 ? event->recurrence.day_of_month : event->day;
        int max_d = days_in_month(year, month);
        if (target_day > max_d) target_day = max_d;
        return (day == target_day);
    }
    default:
        return false;
    }
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