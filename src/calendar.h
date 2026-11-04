#ifndef CALENDAR_H
#define CALENDAR_H

#include <stddef.h>
#include <stdbool.h>

#define MAX_TITLE_LEN 128
#define MAX_DESC_LEN 512
#define MAX_UID_LEN 128
#define MAX_CATEGORY_LEN 64
#define MAX_TIMESTAMP_LEN 32
#define MAX_DAY_EVENTS 64

typedef enum {
    RECUR_NONE = 0,
    RECUR_DAILY,
    RECUR_WEEKLY,
    RECUR_MONTHLY,
    RECUR_YEARLY
} RecurrenceType;

// Set in ordinal_mask[] for a bare BYDAY weekday (e.g. MO rather than 1MO)
#define ORDINAL_ALL (1 << 10)

typedef struct {
    RecurrenceType type;
    int interval; // e.g. every 1 week, every 2 weeks, every 3 months (default 1)
    int day_of_month; // 1-31 (0 if not explicitly specified)
    int month_of_year; // 1-12 (0 if not explicitly specified)
    int day_of_week_mask; // BYDAY bitmask: bit 0 = Sunday, 1 = Monday, ... 6 = Saturday
    int ordinal_mask[7]; // Per weekday (same index as the mask): ORDINAL_ALL or a set of recurrence_ordinal_bit() bits
    int wkst; // 0 = Sunday, 1 = Monday, -1 = unspecified (treated as Monday)
    bool has_until; // UNTIL given (compared at day granularity)
    int until_year;
    int until_month;
    int until_day;
    int count; // COUNT (0 = unbounded)
} RecurrenceRule;

typedef struct Event {
    int id;
    char uid[MAX_UID_LEN];
    char dtstamp[MAX_TIMESTAMP_LEN]; // Mandatory RFC 5545 property
    char title[MAX_TITLE_LEN];
    char description[MAX_DESC_LEN];
    char category[MAX_CATEGORY_LEN];
    int year;
    int month; // 1-12
    int day; // 1-31
    int end_year;
    int end_month;
    int end_day;
    bool is_transparent; // TRANSP:TRANSPARENT vs OPAQUE
    int sequence; // SEQUENCE
    bool is_recurring;
    RecurrenceRule recurrence;
    struct Event* next;
} Event;

typedef struct {
    int current_year;
    int current_month;
    int selected_day;
    Event* events_head;
    Event* events_tail; // Preserves chronological/file insertion order
    char calendar_name[MAX_TITLE_LEN];
} CalendarState;

// Date Math
bool is_leap_year(int year);
int days_in_month(int year, int month);
int get_day_of_week(int year, int month, int day);

// Calendar State & Event Management
void calendar_init(CalendarState* cal, int year, int month);
Event* calendar_add_event(CalendarState* cal, const char* title, const char* desc,
                          int year, int month, int day, RecurrenceRule* rule);
Event* calendar_add_event_full(CalendarState* cal, const char* uid, const char* dtstamp,
                               const char* title, const char* desc, const char* category,
                               int year, int month, int day,
                               int end_year, int end_month, int end_day,
                               bool is_transparent, int sequence,
                               RecurrenceRule* rule);
bool calendar_delete_event(CalendarState* cal, int event_id);
int calendar_get_events_for_day(const CalendarState* cal, int year, int month, int day,
                                Event* out_events[], int max_events);
void calendar_free_events(CalendarState* cal);

// Text copy that never splits a UTF-8 sequence; returns bytes copied
size_t copy_utf8_truncated(char* dst, size_t dst_size, const char* src);

// Recurrence check
bool event_occurs_on(const Event* event, int year, int month, int day);
// Bit for a BYDAY ordinal: +1..+5 -> bits 0..4, -1..-5 -> bits 5..9; 0 if out of range
int recurrence_ordinal_bit(int nth);
void calendar_next_day(int* year, int* month, int* day);

#endif // CALENDAR_H