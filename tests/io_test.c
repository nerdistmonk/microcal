#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calendar.h"
#include "io.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

// Files the tests create in their scratch directory
static const char* const scratch_files[] = {
    "in.ics", "rt.ics", "vt.ics", "plain.ics", "folding.ics", "full.ics", "rules.ics", ".microcal_last", NULL
};

static bool write_lines(const char* path, const char* const* lines) {
    FILE* f = fopen(path, "w");
    if (f == NULL) return false;
    for (size_t i = 0; lines[i] != NULL; i++) fprintf(f, "%s\r\n", lines[i]);
    return fclose(f) == 0;
}

static bool import_lines(CalendarState* cal, const char* const* lines) {
    return write_lines("in.ics", lines) && calendar_import_ics(cal, "in.ics");
}

static size_t read_file(const char* path, char* buf, size_t size) {
    FILE* f = fopen(path, "r");
    if (f == NULL) return 0;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

static int count_substr(const char* text, const char* needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char* p = strstr(text, needle); p != NULL; p = strstr(p + len, needle)) n++;
    return n;
}

static const Event* find_uid(const CalendarState* cal, const char* uid) {
    for (const Event* e = cal->events_head; e != NULL; e = e->next) {
        if (strcmp(e->uid, uid) == 0) return e;
    }
    return NULL;
}

static int count_events(const CalendarState* cal) {
    int n = 0;
    for (const Event* e = cal->events_head; e != NULL; e = e->next) n++;
    return n;
}

// Occurrence count for each day of 2026, so two calendars can be compared day by day
static void occurrence_profile(const Event* e, int* profile) {
    int y = 2026, m = 1, d = 1;
    EventOccurrence out[8];
    for (int i = 0; i < 365; i++) {
        profile[i] = calendar_event_occurrences_on(e, y, m, d, out, 8);
        calendar_next_day(&y, &m, &d);
    }
}

// Compares the stored fields of two events. A floating event is written in this machine's zone on export,
// so its zone is only compared when the original had one.
static bool same_event(const Event* a, const Event* b) {
    bool zone_ok = a->zone[0] == '\0' ||
                   (strcmp(a->zone, b->zone) == 0 && a->zone_span == b->zone_span);
    return strcmp(a->title, b->title) == 0 && strcmp(a->description, b->description) == 0 &&
           strcmp(a->category, b->category) == 0 && a->is_timed == b->is_timed &&
           a->year == b->year && a->month == b->month && a->day == b->day &&
           a->end_year == b->end_year && a->end_month == b->end_month && a->end_day == b->end_day &&
           a->start_minute == b->start_minute && a->end_minute == b->end_minute &&
           zone_ok && a->is_recurring == b->is_recurring &&
           a->exdates.count == b->exdates.count && a->rdates.count == b->rdates.count;
}

static void test_recurrence_exdate_rdate(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:rec-1", "DTSTAMP:20260101T000000Z",
        "DTSTART;VALUE=DATE:20260105", "RRULE:FREQ=WEEKLY;UNTIL=20260131",
        "EXDATE;VALUE=DATE:20260112", "RDATE;VALUE=DATE:20260201",
        "SUMMARY:Weekly", "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    const Event* e = find_uid(&cal, "rec-1");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(event_occurs_on(e, 2026, 1, 5));
        CHECK(!event_occurs_on(e, 2026, 1, 12));
        CHECK(event_occurs_on(e, 2026, 1, 19));
        CHECK(event_occurs_on(e, 2026, 2, 1));
        CHECK(!event_occurs_on(e, 2026, 2, 2));
    }
    calendar_free_events(&cal);
}

static void test_property_order(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:ord-1", "DTSTAMP:20260101T000000Z",
        "DTEND;TZID=America/New_York:20260601T100000",
        "RRULE:FREQ=DAILY;COUNT=3",
        "SUMMARY:Ordered",
        "DTSTART;TZID=America/New_York:20260601T090000",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    const Event* e = find_uid(&cal, "ord-1");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(e->start_minute == 540);
        CHECK(e->end_minute == 600);
        CHECK(strcmp(e->zone, "America/New_York") == 0);
        CHECK(event_occurs_on(e, 2026, 6, 1));
        CHECK(event_occurs_on(e, 2026, 6, 3));
        CHECK(!event_occurs_on(e, 2026, 6, 4));
    }
    calendar_free_events(&cal);
}

static void test_duration(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        // 01:00 EST plus two elapsed hours is 04:00 EDT, because 02:00-03:00 does not exist that day
        "BEGIN:VEVENT", "UID:dst-dur", "DTSTAMP:20260101T000000Z",
        "DTSTART;TZID=America/New_York:20260308T010000", "DURATION:PT2H", "SUMMARY:Across DST", "END:VEVENT",
        "BEGIN:VEVENT", "UID:float-dur", "DTSTAMP:20260101T000000Z",
        "DTSTART:20260301T090000", "DURATION:PT90M", "SUMMARY:Floating", "END:VEVENT",
        "BEGIN:VEVENT", "UID:day-dur", "DTSTAMP:20260101T000000Z",
        "DTSTART;VALUE=DATE:20260110", "DURATION:P2D", "SUMMARY:Two days", "END:VEVENT",
        "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    const Event* e = find_uid(&cal, "dst-dur");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(e->end_month == 3 && e->end_day == 8);
        CHECK(e->end_minute == 240);
        CHECK(e->zone_span == 7200);
    }
    e = find_uid(&cal, "float-dur");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(e->zone[0] == '\0');
        CHECK(e->end_minute == 630);
    }
    e = find_uid(&cal, "day-dur");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(event_occurs_on(e, 2026, 1, 10));
        CHECK(event_occurs_on(e, 2026, 1, 11));
        CHECK(!event_occurs_on(e, 2026, 1, 12));
    }
    calendar_free_events(&cal);
}

static void test_all_day_multi_day(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:trip", "DTSTAMP:20260101T000000Z",
        "DTSTART;VALUE=DATE:20260110", "DTEND;VALUE=DATE:20260113", "SUMMARY:Trip",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    const Event* e = find_uid(&cal, "trip");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(!event_occurs_on(e, 2026, 1, 9));
        CHECK(event_occurs_on(e, 2026, 1, 10));
        CHECK(event_occurs_on(e, 2026, 1, 11));
        CHECK(event_occurs_on(e, 2026, 1, 12));
        CHECK(!event_occurs_on(e, 2026, 1, 13));
    }
    calendar_free_events(&cal);
}

static void test_zones_and_utc(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:kolkata", "DTSTAMP:20260101T000000Z",
        "DTSTART;TZID=Asia/Kolkata:20260601T090000", "DTEND;TZID=Asia/Kolkata:20260601T100000",
        "SUMMARY:Kolkata", "END:VEVENT",
        "BEGIN:VEVENT", "UID:utc", "DTSTAMP:20260101T000000Z",
        "DTSTART:20260601T120000Z", "SUMMARY:UTC", "END:VEVENT",
        "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    const Event* e = find_uid(&cal, "kolkata");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(strcmp(e->zone, "Asia/Kolkata") == 0);
        CHECK(e->start_minute == 540);
        CHECK(e->end_minute == 600);
    }
    e = find_uid(&cal, "utc");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(strcmp(e->zone, "UTC") == 0);
        CHECK(e->start_minute == 720);
    }
    calendar_free_events(&cal);
}

static void test_import_unfolding_utf8(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    // "Café" split across a folded line: the continuation starts with a space, so the octets rejoin
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:fold-1", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201",
        "SUMMARY:Caf",
        " \xc3\xa9 au lait",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    const Event* e = find_uid(&cal, "fold-1");
    CHECK(e != NULL);
    if (e != NULL) CHECK(strcmp(e->title, "Caf\xc3\xa9 au lait") == 0);
    calendar_free_events(&cal);
}

static void test_uid_rules(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* no_uid[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201", "SUMMARY:No uid",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, no_uid));
    CHECK(count_events(&cal) == 1);
    if (cal.events_head != NULL) {
        char uid[MAX_UID_LEN];
        strcpy(uid, cal.events_head->uid);
        CHECK(uid[0] != '\0');
        // Importing the same file again matches the generated UID and replaces the event
        CHECK(import_lines(&cal, no_uid));
        CHECK(count_events(&cal) == 1);
        CHECK(cal.events_head != NULL && strcmp(cal.events_head->uid, uid) == 0);
    }
    calendar_free_events(&cal);

    calendar_init(&cal, 2026, 1);
    const char* dup[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:dup-1", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201", "SUMMARY:First",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:dup-1", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260202", "SUMMARY:Second",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, dup));
    CHECK(count_events(&cal) == 2);
    const Event* first = find_uid(&cal, "dup-1");
    const Event* second = find_uid(&cal, "dup-1#2");
    CHECK(first != NULL && strcmp(first->title, "First") == 0);
    CHECK(second != NULL && strcmp(second->title, "Second") == 0);
    calendar_free_events(&cal);

    calendar_init(&cal, 2026, 1);
    const char* stable[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:stable-1", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201", "SUMMARY:Once",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, stable));
    CHECK(import_lines(&cal, stable));
    CHECK(count_events(&cal) == 1);
    calendar_free_events(&cal);
}

static void test_valarm_skipped(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:alarm-1", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201",
        "SUMMARY:Real title", "DESCRIPTION:Real desc",
        "BEGIN:VALARM", "ACTION:DISPLAY", "SUMMARY:Alarm summary", "DESCRIPTION:Alarm text", "TRIGGER:-PT15M",
        "END:VALARM",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    const Event* e = find_uid(&cal, "alarm-1");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(strcmp(e->title, "Real title") == 0);
        CHECK(strcmp(e->description, "Real desc") == 0);
    }
    calendar_free_events(&cal);
}

static void test_recurrence_id_override(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:series-1", "DTSTAMP:20260101T000000Z",
        "DTSTART;VALUE=DATE:20260101", "RRULE:FREQ=DAILY;UNTIL=20260105", "SUMMARY:Series",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:series-1", "DTSTAMP:20260101T000000Z",
        "RECURRENCE-ID;VALUE=DATE:20260103", "DTSTART;VALUE=DATE:20260104", "SUMMARY:Moved",
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    CHECK(count_events(&cal) == 2);
    const Event* master = find_uid(&cal, "series-1");
    const Event* moved = find_uid(&cal, "series-1#20260103");
    CHECK(master != NULL && moved != NULL);
    if (master != NULL) {
        CHECK(event_occurs_on(master, 2026, 1, 2));
        CHECK(!event_occurs_on(master, 2026, 1, 3));
    }
    if (moved != NULL) {
        CHECK(strcmp(moved->title, "Moved") == 0);
        CHECK(event_occurs_on(moved, 2026, 1, 4));
    }
    calendar_free_events(&cal);
}

static void test_export_folding_utf8(void) {
    // 66 ASCII bytes, then a two-byte character that would straddle the 75-octet fold, then "bc"
    char summary[160];
    strcpy(summary, "SUMMARY:");
    memset(summary + 8, 'a', 66);
    strcpy(summary + 74, "\xc3\xa9" "bc");
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:wrap-1", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201",
        summary,
        "END:VEVENT", "END:VCALENDAR", NULL
    };
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    CHECK(import_lines(&cal, lines));
    CHECK(calendar_export_ics(&cal, "folding.ics"));

    FILE* f = fopen("folding.ics", "r");
    CHECK(f != NULL);
    if (f != NULL) {
        char line[1024];
        bool all_short = true;
        while (fgets(line, sizeof line, f) != NULL) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
            if (len > 75) all_short = false;
        }
        fclose(f);
        CHECK(all_short);
    }

    CalendarState back;
    calendar_init(&back, 2026, 1);
    CHECK(calendar_import_ics(&back, "folding.ics"));
    const Event* e = find_uid(&back, "wrap-1");
    CHECK(e != NULL);
    if (e != NULL) CHECK(strcmp(e->title, summary + 8) == 0);
    calendar_free_events(&back);
    calendar_free_events(&cal);
}

static void test_export_vtimezone(void) {
    static char text[65536];
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:vt-1", "DTSTAMP:20260101T000000Z",
        "DTSTART;TZID=America/New_York:20260601T090000", "DTEND;TZID=America/New_York:20260601T100000",
        "SUMMARY:NY", "END:VEVENT", "END:VCALENDAR", NULL
    };
    CHECK(import_lines(&cal, lines));
    CHECK(calendar_export_ics(&cal, "vt.ics"));
    size_t n = read_file("vt.ics", text, sizeof text);
    CHECK(n > 0);
    CHECK(count_substr(text, "BEGIN:VTIMEZONE") == 1);
    CHECK(count_substr(text, "END:VTIMEZONE") == 1);
    CHECK(strstr(text, "TZID:America/New_York") != NULL);
    CHECK(strstr(text, "RRULE:FREQ=YEARLY") != NULL);
    CHECK(strstr(text, "DTSTART;TZID=America/New_York:20260601T090000") != NULL);
    calendar_free_events(&cal);
}

static void test_export_round_trip(void) {
    const char* input[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:rt-weekly-ny", "DTSTAMP:20260101T000000Z",
        "DTSTART;TZID=America/New_York:20260302T090000", "DTEND;TZID=America/New_York:20260302T100000",
        "RRULE:FREQ=WEEKLY;UNTIL=20261231", "EXDATE;TZID=America/New_York:20260309T090000",
        "RDATE;TZID=America/New_York:20260401T090000",
        "SUMMARY:Standup", "DESCRIPTION:Daily\\, team sync", "CATEGORIES:Work",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:rt-allday", "DTSTAMP:20260101T000000Z",
        "DTSTART;VALUE=DATE:20260110", "DTEND;VALUE=DATE:20260113", "SUMMARY:Trip",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:rt-utc", "DTSTAMP:20260101T000000Z",
        "DTSTART:20260601T120000Z", "DTEND:20260601T130000Z", "SUMMARY:UTC meeting",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:rt-float", "DTSTAMP:20260101T000000Z",
        "DTSTART:20260701T083000", "DURATION:PT90M", "SUMMARY:Floating",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:rt-folded", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201",
        "SUMMARY:Caf", " \xc3\xa9 au lait with a long tail of extra words to force folding across lines",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:series-1", "DTSTAMP:20260101T000000Z",
        "DTSTART;VALUE=DATE:20260101", "RRULE:FREQ=DAILY;UNTIL=20260105", "SUMMARY:Series",
        "END:VEVENT",
        "BEGIN:VEVENT", "UID:series-1", "DTSTAMP:20260101T000000Z",
        "RECURRENCE-ID;VALUE=DATE:20260103", "DTSTART;VALUE=DATE:20260104", "SUMMARY:Moved",
        "END:VEVENT",
        "END:VCALENDAR", NULL
    };
    CalendarState a, b;
    calendar_init(&a, 2026, 1);
    calendar_init(&b, 2026, 1);
    CHECK(import_lines(&a, input));
    CHECK(count_events(&a) == 7);
    CHECK(calendar_export_ics(&a, "rt.ics"));
    CHECK(calendar_import_ics(&b, "rt.ics"));
    CHECK(count_events(&b) == count_events(&a));

    for (const Event* ea = a.events_head; ea != NULL; ea = ea->next) {
        const Event* eb = find_uid(&b, ea->uid);
        CHECK(eb != NULL);
        if (eb == NULL) continue;
        CHECK(same_event(ea, eb));
        int pa[365], pb[365];
        occurrence_profile(ea, pa);
        occurrence_profile(eb, pb);
        CHECK(memcmp(pa, pb, sizeof pa) == 0);
    }
    const Event* weekly = find_uid(&b, "rt-weekly-ny");
    CHECK(weekly != NULL && event_occurs_on(weekly, 2026, 3, 16));
    CHECK(weekly != NULL && !event_occurs_on(weekly, 2026, 3, 9));
    const Event* folded = find_uid(&b, "rt-folded");
    CHECK(folded != NULL && strcmp(folded->title, "Caf\xc3\xa9 au lait with a long tail of extra words to force folding across lines") == 0);

    calendar_free_events(&a);
    calendar_free_events(&b);
}

static void test_export_appends_ics(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    CHECK(calendar_export_ics(&cal, "plain"));
    CHECK(access("plain.ics", F_OK) == 0);
    calendar_free_events(&cal);
}

static void test_failure_paths(void) {
    CalendarState cal;
    calendar_init(&cal, 2026, 1);
    CHECK(!calendar_import_ics(&cal, "does-not-exist.ics"));
    CHECK(!calendar_export_ics(&cal, "/nonexistent-microcal-dir/x.ics"));
    // /dev/full fails every write with ENOSPC. A symlink lets the export open it under an .ics name.
    if (access("/dev/full", W_OK) == 0 && symlink("/dev/full", "full.ics") == 0) {
        const char* lines[] = {
            "BEGIN:VCALENDAR", "VERSION:2.0",
            "BEGIN:VEVENT", "UID:full-1", "DTSTAMP:20260101T000000Z", "DTSTART;VALUE=DATE:20260201",
            "SUMMARY:Full disk", "END:VEVENT", "END:VCALENDAR", NULL
        };
        CHECK(import_lines(&cal, lines));
        CHECK(!calendar_export_ics(&cal, "full.ics"));
        CHECK(access("full.ics", F_OK) != 0);
    }
    calendar_free_events(&cal);
}

static void test_rule_check(void) {
    const char* lines[] = {
        "BEGIN:VCALENDAR", "VERSION:2.0",
        "BEGIN:VEVENT", "UID:byhour-1", "DTSTAMP:20260101T000000Z", "DTSTART:20260105T090000",
        "SUMMARY:Standup", "RRULE:FREQ=WEEKLY;BYHOUR=9,17", "END:VEVENT",
        "BEGIN:VEVENT", "UID:plain-1", "DTSTAMP:20260101T000000Z", "DTSTART:20260106T090000",
        "SUMMARY:Plain", "RRULE:FREQ=WEEKLY;BYDAY=TU", "END:VEVENT",
        "BEGIN:VEVENT", "UID:subdaily-1", "DTSTAMP:20260101T000000Z", "DTSTART:20260107T090000",
        "SUMMARY:Ticks", "RRULE:FREQ=HOURLY;COUNT=3", "END:VEVENT",
        "BEGIN:VEVENT", "UID:range-1", "DTSTAMP:20260101T000000Z", "DTSTART:20260108T090000",
        "SUMMARY:Override", "RECURRENCE-ID;RANGE=THISANDFUTURE:20260108T090000", "END:VEVENT",
        "BEGIN:VEVENT", "UID:setpos-1", "DTSTAMP:20260101T000000Z", "DTSTART:20260109T090000",
        "SUMMARY:Many", "RRULE:FREQ=MONTHLY;BYDAY=MO;BYSETPOS=1,2,3,4,5,6,7,8,9", "END:VEVENT",
        "BEGIN:VEVENT", "UID:zone-1", "DTSTAMP:20260101T000000Z", "DTSTART;TZID=Custom Zone:20260110T090000",
        "SUMMARY:Custom", "END:VEVENT",
        "BEGIN:VEVENT", "UID:known-zone-1", "DTSTAMP:20260101T000000Z",
        "DTSTART;TZID=America/New_York:20260111T090000", "SUMMARY:Known", "END:VEVENT",
        "END:VCALENDAR", NULL
    };
    CHECK(write_lines("rules.ics", lines));
    char report[1024];
    CHECK(calendar_check_ics_rules("rules.ics", report, sizeof(report)) == 5);
    CHECK(strstr(report, "- Standup: BYHOUR\n") != NULL);
    CHECK(strstr(report, "- Ticks: sub-daily FREQ\n") != NULL);
    CHECK(strstr(report, "- Override: RANGE=THISANDFUTURE\n") != NULL);
    CHECK(strstr(report, "- Many: BYSETPOS beyond 8\n") != NULL);
    CHECK(strstr(report, "- Custom: unrecognised TZID\n") != NULL);
    CHECK(strstr(report, "Plain") == NULL);
    CHECK(strstr(report, "Known") == NULL);
    CHECK(calendar_check_ics_rules("missing.ics", report, sizeof(report)) == -1);
}

int main(void) {
    char dir[] = "/tmp/microcal_io_test_XXXXXX";
    if (mkdtemp(dir) == NULL || chdir(dir) != 0) {
        printf("could not create a scratch directory\n");
        return 1;
    }

    test_recurrence_exdate_rdate();
    test_property_order();
    test_duration();
    test_all_day_multi_day();
    test_zones_and_utc();
    test_import_unfolding_utf8();
    test_uid_rules();
    test_valarm_skipped();
    test_recurrence_id_override();
    test_export_folding_utf8();
    test_export_vtimezone();
    test_export_round_trip();
    test_export_appends_ics();
    test_failure_paths();
    test_rule_check();

    for (size_t i = 0; scratch_files[i] != NULL; i++) remove(scratch_files[i]);
    if (chdir("/") != 0) printf("could not leave scratch directory\n");
    rmdir(dir);

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
