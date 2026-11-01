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

bool event_occurs_on(const Event* event, int year, int month, int day) {
    if (year < event->year) return false;
    if (year == event->year && month < event->month) return false;
    if (year == event->year && month == event->month && day < event->day) return false;

    if (!event->is_recurring) {
        return (event->year == year && event->month == month && event->day == day);
    }

    int interval = event->recurrence.interval > 0 ? event->recurrence.interval : 1;

    switch (event->recurrence.type) {
    case RECUR_DAILY: {
        if (interval > 1) {
            long days_diff = days_since_epoch(year, month, day) -
                             days_since_epoch(event->year, event->month, event->day);
            if (days_diff < 0 || (days_diff % interval != 0)) return false;
        }
        return true;
    }
    case RECUR_WEEKLY: {
        int target_wday = get_day_of_week(year, month, day);
        if (!(event->recurrence.day_of_week_mask & (1 << target_wday))) {
            return false;
        }
        if (interval > 1) {
            long days_diff = days_since_epoch(year, month, day) -
                             days_since_epoch(event->year, event->month, event->day);
            if (days_diff < 0) return false;
            long weeks_diff = days_diff / 7;
            if (weeks_diff % interval != 0) return false;
        }
        return true;
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