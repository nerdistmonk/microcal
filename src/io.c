// setenv, unsetenv, tzset, localtime_r and strdup are POSIX, not C99
#define _POSIX_C_SOURCE 200809L

#include "io.h"
#include "zone.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_FILE ".microcal_last"
#define MAX_DURATION_DAYS 36500 // Clamp for DURATION (about 100 years)
#define MAX_DURATION_SECONDS ((long long)MAX_DURATION_DAYS * 86400)

static void trim_newline(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[len - 1] = '\0';
        len--;
    }
}

// Unescape RFC 5545 text sequences
static void unescape_ics_text(char* str) {
    char* src = str;
    char* dst = str;
    while (*src) {
        if (*src == '\\') {
            src++;
            if (*src == 'n' || *src == 'N') {
                *dst++ = '\n';
            } else if (*src == ',' || *src == ';' || *src == '\\') {
                *dst++ = *src;
            } else if (*src) {
                *dst++ = *src;
            }
        } else {
            *dst++ = *src;
        }
        if (*src) src++;
    }
    *dst = '\0';
}

// Find property value start skipping quoted parameter strings
static char* find_property_value_start(char* line) {
    bool in_quotes = false;
    for (char* p = line; *p; p++) {
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (*p == ':' && !in_quotes) {
            return p + 1;
        }
    }
    return NULL;
}

void calendar_save_last_filepath(const char* filepath) {
    FILE* cfg = fopen(CONFIG_FILE, "w");
    if (cfg) {
        fprintf(cfg, "%s\n", filepath);
        fclose(cfg);
    }
}

bool calendar_get_last_filepath(char* out_path, size_t max_len) {
    FILE* cfg = fopen(CONFIG_FILE, "r");
    if (!cfg) return false;
    if (fgets(out_path, (int)max_len, cfg)) {
        trim_newline(out_path);
        fclose(cfg);
        return strlen(out_path) > 0;
    }
    fclose(cfg);
    return false;
}

static int parse_weekday(const char* str) {
    if (strncmp(str, "SU", 2) == 0) return 0;
    if (strncmp(str, "MO", 2) == 0) return 1;
    if (strncmp(str, "TU", 2) == 0) return 2;
    if (strncmp(str, "WE", 2) == 0) return 3;
    if (strncmp(str, "TH", 2) == 0) return 4;
    if (strncmp(str, "FR", 2) == 0) return 5;
    if (strncmp(str, "SA", 2) == 0) return 6;
    return -1;
}

static unsigned long fnv_append(unsigned long hash, const char* text) {
    for (const char* p = text; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619UL;
    }
    return hash;
}

static unsigned long fnv_append_int(unsigned long hash, int value) {
    char text[16];
    snprintf(text, sizeof(text), "|%d", value);
    return fnv_append(hash, text);
}

// FNV-1a hash of the event's identifying fields and full rule, so the same event always yields the same UID
static void make_content_uid(char* out, size_t out_size, const char* title,
                             int sy, int sm, int sd, int ey, int em, int ed,
                             bool is_timed, int start_minute, int end_minute,
                             const RecurrenceRule* rule) {
    unsigned long hash = fnv_append(2166136261UL, title);
    const int dates[] = {sy, sm, sd, ey, em, ed};
    for (size_t i = 0; i < sizeof(dates) / sizeof(dates[0]); i++) {
        hash = fnv_append_int(hash, dates[i]);
    }
    if (is_timed) {
        hash = fnv_append_int(hash, start_minute);
        hash = fnv_append_int(hash, end_minute);
    }
    const int fields[] = {rule->type, rule->interval, rule->day_of_week_mask, rule->month_mask,
                          rule->mday_mask, rule->mday_neg_mask, rule->setpos_count, rule->count,
                          rule->has_until, rule->until_year, rule->until_month, rule->until_day,
                          rule->wkst};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        hash = fnv_append_int(hash, fields[i]);
    }
    for (int i = 0; i < 7; i++) {
        hash = fnv_append_int(hash, rule->ordinal_mask[i]);
    }
    for (int i = 0; i < rule->setpos_count; i++) {
        hash = fnv_append_int(hash, rule->setpos[i]);
    }
    snprintf(out, out_size, "microcal-%08lx@local", hash & 0xffffffffUL);
}

// UIDs already used by the file being imported
typedef struct {
    char (*items)[MAX_UID_LEN];
    size_t count;
    size_t capacity;
} UidSet;

static bool uid_set_contains(const UidSet* set, const char* uid) {
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->items[i], uid) == 0) return true;
    }
    return false;
}

static bool uid_set_add(UidSet* set, const char* uid) {
    if (set->count == set->capacity) {
        size_t new_capacity = set->capacity ? set->capacity * 2 : 64;
        char (*grown)[MAX_UID_LEN] = realloc(set->items, new_capacity * sizeof(*grown));
        if (!grown) return false;
        set->items = grown;
        set->capacity = new_capacity;
    }
    strcpy(set->items[set->count++], uid);
    return true;
}

static void uid_set_free(UidSet* set) {
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static Event* find_event_by_uid(CalendarState* cal, const char* uid) {
    for (Event* e = cal->events_head; e; e = e->next) {
        if (strcmp(e->uid, uid) == 0) return e;
    }
    return NULL;
}

// Value of KEY= in an RRULE line; KEY must follow ':' or ';' so that e.g. NOTBYDAY= never matches BYDAY=
static const char* rrule_value(const char* line, const char* key) {
    size_t key_len = strlen(key);
    for (const char* p = strstr(line, key); p; p = strstr(p + key_len, key)) {
        if (p > line && (p[-1] == ':' || p[-1] == ';')) return p + key_len;
    }
    return NULL;
}

// True when value starts with word and the word ends at ';' or the end of the value
static bool value_is(const char* value, const char* word) {
    size_t len = strlen(word);
    return strncmp(value, word, len) == 0 && (value[len] == '\0' || value[len] == ';');
}

// One BYDAY token: "MO" (every such weekday) or an ordinal such as "2TU" / "-1FR" (MONTHLY/YEARLY scope)
static void add_byday_token(RecurrenceRule* rule, const char* token) {
    size_t len = strlen(token);
    if (len < 2) return;
    int wday = parse_weekday(token + len - 2);
    if (wday < 0) return;

    if (len == 2) {
        rule->day_of_week_mask |= (1 << wday);
        rule->ordinal_mask[wday] |= ORDINAL_ALL;
        return;
    }

    int bit = recurrence_ordinal_bit(atoi(token));
    if (bit == 0) return; // Ordinals beyond ±5 are not supported
    rule->day_of_week_mask |= (1 << wday);
    rule->ordinal_mask[wday] |= bit;
}

static void parse_byday_list(RecurrenceRule* rule, const char* list) {
    char buf[128];
    size_t len = strcspn(list, ";");
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, list, len);
    buf[len] = '\0';
    for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        add_byday_token(rule, tok);
    }
}

// Reads exactly `count` ASCII digits from the start of s
static bool read_digits(const char* s, int count, int* out) {
    int value = 0;
    for (int i = 0; i < count; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return true;
}

// Parses YYYYMMDD with an optional THHMM[SS][Z] time, keeping the wall-clock digits as written. An invalid time
// is dropped, leaving a date-only result. Returns false for an invalid date.
static bool parse_ics_datetime(const char* text, int* y, int* m, int* d, bool* has_time, int* minute) {
    int yy, mm, dd;
    if (!read_digits(text, 4, &yy) || !read_digits(text + 4, 2, &mm) || !read_digits(text + 6, 2, &dd)) {
        return false;
    }
    if (mm < 1 || mm > 12 || dd < 1 || dd > days_in_month(yy, mm)) return false;
    *y = yy;
    *m = mm;
    *d = dd;
    *has_time = false;
    *minute = 0;
    if (text[8] == 'T') {
        int hh, mi;
        if (read_digits(text + 9, 2, &hh) && read_digits(text + 11, 2, &mi) && hh < 24 && mi < 60) {
            *has_time = true;
            *minute = hh * 60 + mi;
        }
    }
    return true;
}

// The zone a timed value is written in: "UTC" for a trailing Z, the TZID for a known zone, or NULL for floating
// values and unknown zones.
static const char* ics_value_zone(const char* text, const char* tzid) {
    if (strchr(text, 'Z') != NULL) return "UTC";
    if (tzid != NULL && zone_is_known(tzid)) return tzid;
    return NULL;
}

// Copies the TZID parameter of a property into zone. Returns NULL when the property has none. value is where the
// property value starts, so a TZID inside the value is ignored.
static const char* property_tzid(const char* line, const char* value, char* zone, size_t size) {
    const char* param = strstr(line, "TZID=");
    if (param == NULL || param >= value) return NULL;
    param += 5;
    if (*param == '"') param++;
    size_t len = strcspn(param, ";:\"");
    if (len == 0 || len >= size) return NULL;
    memcpy(zone, param, len);
    zone[len] = '\0';
    return zone;
}

// Parses a date-time like parse_ics_datetime. A timed value with a trailing Z or a known TZID is an instant in that
// zone. When frame is given, the instant is converted to wall-clock time in frame ("" for the viewer's local time).
// When frame is NULL, or the value is a date or floating, the digits are kept as written.
static bool parse_ics_value(const char* text, const char* tzid, const char* frame,
                            int* y, int* m, int* d, bool* has_time, int* minute) {
    if (!parse_ics_datetime(text, y, m, d, has_time, minute)) return false;
    if (!*has_time || frame == NULL) return true;

    const char* source = ics_value_zone(text, tzid);
    time_t t;
    if (source == NULL || !zone_to_instant(*y, *m, *d, *minute, source, &t)) return true;
    zone_wall_clock(t, frame, y, m, d, minute);
    return true;
}

// Orders floating date-times: the date first, then the minute of day
static long long timed_key(int y, int m, int d, int minute) {
    return (long long)YMD_KEY(y, m, d) * 1440 + minute;
}

// Moves a floating date-time forward by seconds. Seconds below one minute are dropped.
static void add_seconds_to_datetime(int* y, int* m, int* d, int* minute, long long seconds) {
    long long minutes = seconds / 60;
    long long days = minutes / 1440;
    *minute += (int)(minutes % 1440);
    if (*minute >= 1440) {
        *minute -= 1440;
        days++;
    }
    for (long long i = 0; i < days; i++) calendar_next_day(y, m, d);
}

// Reads one comma-separated integer of an RRULE list. Returns false at the end of the list.
// *valid is false for an item that is not a plain integer; the caller skips it.
static bool next_rrule_int(const char** p, int* value, bool* valid) {
    const char* start = *p;
    if (*start == '\0' || *start == ';') return false;
    const char* end = start;
    while (*end != '\0' && *end != ',' && *end != ';') end++;

    char buf[16];
    size_t len = (size_t)(end - start);
    *valid = false;
    if (len > 0 && len < sizeof(buf)) {
        memcpy(buf, start, len);
        buf[len] = '\0';
        char* after = NULL;
        long v = strtol(buf, &after, 10);
        if (after != buf && *after == '\0' && v >= -1000 && v <= 1000) {
            *value = (int)v;
            *valid = true;
        }
    }
    *p = (*end == ',') ? end + 1 : end;
    return true;
}

// Parses an RRULE line into rule. Unsupported parts (BYHOUR, BYWEEKNO, ...) are ignored. UNTIL is converted into
// frame, the zone of the event's start.
static void parse_rrule(const char* line, RecurrenceRule* rule, const char* frame) {
    bool sub_daily = false;
    const char* v = rrule_value(line, "FREQ=");
    if (v) {
        if (value_is(v, "YEARLY")) {
            rule->type = RECUR_YEARLY;
        } else if (value_is(v, "MONTHLY")) {
            rule->type = RECUR_MONTHLY;
        } else if (value_is(v, "WEEKLY")) {
            rule->type = RECUR_WEEKLY;
        } else if (value_is(v, "DAILY")) {
            rule->type = RECUR_DAILY;
        } else if (value_is(v, "HOURLY") || value_is(v, "MINUTELY") || value_is(v, "SECONDLY")) {
            // The data model has no time of day, so sub-daily rules become a daily rule
            rule->type = RECUR_DAILY;
            sub_daily = true;
        }
    }

    if ((v = rrule_value(line, "INTERVAL=")) != NULL) {
        int interval = atoi(v);
        rule->interval = interval > 0 ? interval : 1;
    }
    if ((v = rrule_value(line, "BYMONTHDAY=")) != NULL) {
        int x = 0;
        bool valid = false;
        const char* p = v;
        while (next_rrule_int(&p, &x, &valid)) {
            if (!valid) continue;
            if (x >= 1 && x <= 31) rule->mday_mask |= MDAY_BIT(x);
            else if (x <= -1 && x >= -31) rule->mday_neg_mask |= MDAY_BIT(-x);
        }
    }
    if ((v = rrule_value(line, "BYMONTH=")) != NULL) {
        int x = 0;
        bool valid = false;
        const char* p = v;
        while (next_rrule_int(&p, &x, &valid)) {
            if (valid && x >= 1 && x <= 12) rule->month_mask |= MONTH_BIT(x);
        }
    }
    if ((v = rrule_value(line, "BYSETPOS=")) != NULL) {
        int x = 0;
        bool valid = false;
        const char* p = v;
        while (next_rrule_int(&p, &x, &valid)) {
            bool in_range = (x >= 1 && x <= MAX_PERIOD_DAYS) || (x <= -1 && x >= -MAX_PERIOD_DAYS);
            // Positions beyond MAX_SETPOS are dropped (see readme)
            if (valid && in_range && rule->setpos_count < MAX_SETPOS) {
                rule->setpos[rule->setpos_count++] = x;
            }
        }
    }
    if ((v = rrule_value(line, "BYDAY=")) != NULL) {
        parse_byday_list(rule, v);
    }
    if ((v = rrule_value(line, "WKST=")) != NULL) {
        rule->wkst = parse_weekday(v);
    }
    if ((v = rrule_value(line, "COUNT=")) != NULL) {
        int count = atoi(v);
        rule->count = count > 0 ? count : 0;
    }
    if ((v = rrule_value(line, "UNTIL=")) != NULL) {
        int y = 0, m = 0, d = 0, minute = 0;
        bool has_time = false;
        if (parse_ics_value(v, NULL, frame, &y, &m, &d, &has_time, &minute)) {
            rule->has_until = true;
            rule->until_year = y;
            rule->until_month = m;
            rule->until_day = d;
        }
    }

    if (sub_daily) {
        rule->interval = 1;
        rule->count = 0;
    }
}

// Parses an RFC 5545 DURATION into seconds. Weeks and days come from the date part; hours, minutes and seconds
// come after 'T'. Negative or malformed values return false. Results above MAX_DURATION_SECONDS are clamped.
static bool parse_duration_seconds(const char* value, long long* out) {
    const char* p = value;
    if (*p == '+') p++;
    if (*p != 'P') return false;
    p++;

    long long total = 0;
    bool in_time = false;
    bool any = false;
    while (*p) {
        if (*p == 'T') {
            if (in_time) return false;
            in_time = true;
            p++;
            continue;
        }
        int digits = 0;
        long long n = 0;
        while (p[digits] >= '0' && p[digits] <= '9') {
            if (digits >= 9) return false;
            n = n * 10 + (p[digits] - '0');
            digits++;
        }
        if (digits == 0) return false;

        char unit = p[digits];
        long long unit_seconds = 0;
        if (!in_time) {
            if (unit == 'W') unit_seconds = 7 * 86400;
            else if (unit == 'D') unit_seconds = 86400;
            else return false;
        } else {
            if (unit == 'H') unit_seconds = 3600;
            else if (unit == 'M') unit_seconds = 60;
            else if (unit == 'S') unit_seconds = 1;
            else return false;
        }
        total += n * unit_seconds;
        any = true;
        p += digits + 1;
    }
    if (!any) return false;
    *out = total > MAX_DURATION_SECONDS ? MAX_DURATION_SECONDS : total;
    return true;
}

// Appends `text` escaped as an RFC 5545 TEXT value (backslash, newline, comma, semicolon) to `out`.
// `out` needs room for twice the input length. Returns the number of bytes written.
static size_t append_ics_text(char* out, const char* text) {
    size_t n = 0;
    for (const char* p = text; *p; p++) {
        if (*p == '\r') continue; // A raw CR would break the CRLF line framing
        if (*p == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (*p == '\\' || *p == ',' || *p == ';') {
            out[n++] = '\\';
            out[n++] = *p;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return n;
}

// Writes a content line folded at 75 octets (RFC 5545 §3.1). Continuation lines begin with a space.
// Folds never split a UTF-8 sequence.
static void write_folded_line(FILE* file, const char* line, size_t len) {
    size_t start = 0;
    size_t limit = 75;
    while (len - start > limit) {
        size_t end = start + limit;
        while (end > start && ((unsigned char)line[end] & 0xC0) == 0x80) end--;
        fwrite(line + start, 1, end - start, file);
        fputs("\r\n ", file);
        start = end;
        limit = 74;
    }
    fwrite(line + start, 1, len - start, file);
    fputs("\r\n", file);
}

// Writes NAME:value. Text values are escaped; other values are written unchanged.
static void write_property(FILE* file, const char* name, const char* value, bool is_text) {
    char* line = malloc(strlen(name) + 2 + 2 * strlen(value));
    if (!line) return;
    int n = sprintf(line, "%s:", name);
    if (is_text) {
        n += (int)append_ics_text(line + n, value);
    } else {
        strcpy(line + n, value);
        n += (int)strlen(value);
    }
    write_folded_line(file, line, (size_t)n);
    free(line);
}

static void rule_append(char* out, size_t cap, const char* fmt, ...) {
    size_t len = strlen(out);
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + len, cap - len, fmt, args);
    va_end(args);
}

// Writes the RRULE for daily, weekly, monthly, and yearly events
static void write_rrule(FILE* file, const Event* ev) {
    static const char* wday_names[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    const RecurrenceRule* r = &ev->recurrence;

    const char* freq = NULL;
    switch (r->type) {
    case RECUR_DAILY: freq = "DAILY"; break;
    case RECUR_WEEKLY: freq = "WEEKLY"; break;
    case RECUR_MONTHLY: freq = "MONTHLY"; break;
    case RECUR_YEARLY: freq = "YEARLY"; break;
    default: return;
    }

    char value[1024];
    snprintf(value, sizeof(value), "FREQ=%s", freq);
    if (r->interval > 1) rule_append(value, sizeof(value), ";INTERVAL=%d", r->interval);
    if (r->month_mask != 0) {
        bool first = true;
        rule_append(value, sizeof(value), ";BYMONTH=");
        for (int m = 1; m <= 12; m++) {
            if (r->month_mask & MONTH_BIT(m)) {
                rule_append(value, sizeof(value), "%s%d", first ? "" : ",", m);
                first = false;
            }
        }
    }
    if (r->mday_mask != 0 || r->mday_neg_mask != 0) {
        bool first = true;
        rule_append(value, sizeof(value), ";BYMONTHDAY=");
        for (int d = 1; d <= 31; d++) {
            if (r->mday_mask & MDAY_BIT(d)) {
                rule_append(value, sizeof(value), "%s%d", first ? "" : ",", d);
                first = false;
            }
        }
        for (int d = 1; d <= 31; d++) {
            if (r->mday_neg_mask & MDAY_BIT(d)) {
                rule_append(value, sizeof(value), "%s-%d", first ? "" : ",", d);
                first = false;
            }
        }
    }

    int mask = r->day_of_week_mask;
    // Matches the engine default: a WEEKLY rule with no BYDAY or BYMONTHDAY repeats on the DTSTART weekday
    if (r->type == RECUR_WEEKLY && mask == 0 && r->mday_mask == 0 && r->mday_neg_mask == 0) {
        mask = 1 << get_day_of_week(ev->year, ev->month, ev->day);
    }
    if (mask != 0) {
        bool first = true;
        rule_append(value, sizeof(value), ";BYDAY=");
        for (int wd = 0; wd < 7; wd++) {
            if (!(mask & (1 << wd))) continue;
            int om = r->ordinal_mask[wd];
            if (om == 0 || (om & ORDINAL_ALL)) {
                rule_append(value, sizeof(value), "%s%s", first ? "" : ",", wday_names[wd]);
                first = false;
            } else {
                for (int nth = -5; nth <= 5; nth++) {
                    if (nth == 0 || !(om & recurrence_ordinal_bit(nth))) continue;
                    rule_append(value, sizeof(value), "%s%d%s", first ? "" : ",", nth, wday_names[wd]);
                    first = false;
                }
            }
        }
    }

    if (r->setpos_count > 0) {
        rule_append(value, sizeof(value), ";BYSETPOS=");
        for (int i = 0; i < r->setpos_count; i++) {
            rule_append(value, sizeof(value), "%s%d", i ? "," : "", r->setpos[i]);
        }
    }
    if (r->has_until) {
        rule_append(value, sizeof(value), ";UNTIL=%04d%02d%02d", r->until_year, r->until_month, r->until_day);
        // Timed events use floating times, so UNTIL is the end of its day
        if (ev->is_timed) rule_append(value, sizeof(value), "T235959");
    }
    if (r->count > 0) rule_append(value, sizeof(value), ";COUNT=%d", r->count);
    if (r->wkst >= 0 && r->wkst < 7) rule_append(value, sizeof(value), ";WKST=%s", wday_names[r->wkst]);

    write_property(file, "RRULE", value, false);
}

// Adds each date in an EXDATE/RDATE value to list. Values may be comma-separated; a "/period" suffix is ignored.
// tzid is the zone of the values' times, or NULL. frame is the zone of the event's start (see parse_ics_value).
static void add_ics_date_values(DateList* list, const char* value, const char* tzid, const char* frame) {
    const char* p = value;
    while (*p) {
        size_t item_len = strcspn(p, ",");
        size_t date_len = strcspn(p, ",/");
        char token[32];
        if (date_len < sizeof(token)) {
            memcpy(token, p, date_len);
            token[date_len] = '\0';
            int y = 0, m = 0, d = 0, minute = 0;
            bool has_time = false;
            if (parse_ics_value(token, tzid, frame, &y, &m, &d, &has_time, &minute)) {
                date_list_add(list, YMD_KEY(y, m, d));
            }
        }
        p += item_len;
        if (*p == ',') p++;
    }
}

// A RECURRENCE-ID override whose master event may appear later in the file
typedef struct {
    char uid[MAX_UID_LEN]; // UID of the master event, as written in the file
    int key; // YMD_KEY of the occurrence the override replaces
} PendingExclusion;

typedef struct {
    PendingExclusion* items;
    size_t count;
    size_t capacity;
} PendingExclusions;

static bool pending_exclusions_add(PendingExclusions* list, const char* uid, int key) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity ? list->capacity * 2 : 16;
        PendingExclusion* grown = realloc(list->items, new_capacity * sizeof(*grown));
        if (!grown) return false;
        list->items = grown;
        list->capacity = new_capacity;
    }
    copy_utf8_truncated(list->items[list->count].uid, sizeof(list->items[0].uid), uid);
    list->items[list->count].key = key;
    list->count++;
    return true;
}

// Growable list of owned strings: deferred property lines of one event, and zone names when exporting
typedef struct {
    char** items;
    size_t count;
    size_t capacity;
} StringList;

static bool string_list_add(StringList* list, const char* line) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity ? list->capacity * 2 : 8;
        char** grown = realloc(list->items, new_capacity * sizeof(*grown));
        if (!grown) return false;
        list->items = grown;
        list->capacity = new_capacity;
    }
    size_t len = strlen(line);
    char* copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, line, len + 1);
    list->items[list->count++] = copy;
    return true;
}

static void string_list_clear(StringList* list) {
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    list->count = 0;
}

bool calendar_import_ics(CalendarState* cal, const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) return false;

    char line[8192];
    char raw[8192];
    char uid[MAX_UID_LEN] = "";
    char dtstamp[MAX_TIMESTAMP_LEN] = "";
    char title[MAX_TITLE_LEN] = "Untitled Event";
    char desc[MAX_DESC_LEN] = "";
    char cat[MAX_CATEGORY_LEN] = "";
    int start_y = 2026, start_m = 1, start_d = 1;
    int end_y = 0, end_m = 0, end_d = 0;
    bool start_timed = false;
    int start_minute = 0;
    // The zone of a timed start ("" when floating). Other timed values of the event are kept in this zone.
    char start_zone[MAX_ZONE_LEN] = "";
    bool end_timed = false;
    int end_minute = 0;
    bool is_transparent = true;
    int sequence = 0;
    bool in_event = false;
    bool in_alarm = false;
    bool has_duration = false;
    long long duration_seconds = 0;
    bool has_recid = false;
    int recid_key = 0;
    DateList exdates = {NULL, 0, 0};
    DateList rdates = {NULL, 0, 0};
    PendingExclusions pending = {NULL, 0, 0};
    // DTEND, EXDATE, RDATE, RECURRENCE-ID, and RRULE depend on the start's zone, so they are read at END:VEVENT
    StringList later = {NULL, 0, 0};
    RecurrenceRule rule;
    memset(&rule, 0, sizeof(RecurrenceRule));
    rule.wkst = -1;
    UidSet seen = {NULL, 0, 0};
    bool ok = true;

    line[0] = '\0';

    while (1) {
        if (!fgets(raw, sizeof(raw), file)) {
            raw[0] = '\0';
        }
        trim_newline(raw);

        // Handle unfolded lines (RFC 5545 §3.1)
        if (raw[0] == ' ' || raw[0] == '\t') {
            size_t cur_len = strlen(line);
            if (cur_len + strlen(raw + 1) < sizeof(line) - 1) {
                strcat(line, raw + 1);
            }
            continue;
        }

        if (in_alarm) {
            // VALARM blocks carry their own SUMMARY/DESCRIPTION, which must not overwrite the event's
            if (strcmp(line, "END:VALARM") == 0) in_alarm = false;
        }
        else if (strlen(line) > 0) {
            if (strncmp(line, "NAME:", 5) == 0) {
                copy_utf8_truncated(cal->calendar_name, sizeof(cal->calendar_name), line + 5);
            }
            else if (strncmp(line, "X-WR-CALNAME:", 13) == 0 && cal->calendar_name[0] == '\0') {
                copy_utf8_truncated(cal->calendar_name, sizeof(cal->calendar_name), line + 13);
            }
            else if (strcmp(line, "BEGIN:VEVENT") == 0) {
                in_event = true;
                uid[0] = '\0';
                dtstamp[0] = '\0';
                strcpy(title, "Untitled Event");
                desc[0] = '\0';
                cat[0] = '\0';
                start_y = 2026; start_m = 1; start_d = 1;
                start_timed = false; start_minute = 0;
                start_zone[0] = '\0';
                end_y = 0; end_m = 0; end_d = 0;
                end_timed = false; end_minute = 0;
                is_transparent = true;
                sequence = 0;
                has_duration = false; duration_seconds = 0;
                has_recid = false; recid_key = 0;
                date_list_free(&exdates);
                date_list_free(&rdates);
                string_list_clear(&later);
                in_alarm = false;
                memset(&rule, 0, sizeof(RecurrenceRule));
                rule.interval = 1;
                rule.wkst = -1;
            }
            else if (in_event && strcmp(line, "BEGIN:VALARM") == 0) {
                in_alarm = true;
            }
            else if (strcmp(line, "END:VEVENT") == 0) {
                if (in_event) {
                    // Read now, so the start's zone is known whatever the property order
                    for (size_t i = 0; i < later.count; i++) {
                        char* prop = later.items[i];
                        char* val = find_property_value_start(prop);
                        if (!val) continue;
                        char zone[MAX_ZONE_LEN];
                        const char* tzid = property_tzid(prop, val, zone, sizeof(zone));
                        int y = 0, m = 0, d = 0, minute = 0;
                        bool has_time = false;
                        if (strncmp(prop, "DTEND", 5) == 0) {
                            if (parse_ics_value(val, tzid, start_zone, &y, &m, &d, &has_time, &minute)) {
                                end_y = y;
                                end_m = m;
                                end_d = d;
                                end_timed = has_time;
                                end_minute = minute;
                            }
                        }
                        else if (strncmp(prop, "EXDATE", 6) == 0) {
                            add_ics_date_values(&exdates, val, tzid, start_zone);
                        }
                        else if (strncmp(prop, "RDATE", 5) == 0) {
                            add_ics_date_values(&rdates, val, tzid, start_zone);
                        }
                        else if (strncmp(prop, "RECURRENCE-ID", 13) == 0) {
                            has_recid = true;
                            recid_key = parse_ics_value(val, tzid, start_zone, &y, &m, &d, &has_time, &minute)
                                            ? YMD_KEY(y, m, d) : 0;
                        }
                        else if (strncmp(prop, "RRULE:", 6) == 0) {
                            parse_rrule(prop, &rule, start_zone);
                        }
                    }
                    string_list_clear(&later);

                    // RFC 5545 §3.3.10: If FREQ=WEEKLY has no BYDAY, default to DTSTART weekday.
                    // Evaluated here so start_y/m/d is guaranteed fully parsed regardless of line ordering.
                    if (rule.type == RECUR_WEEKLY && rule.day_of_week_mask == 0 &&
                        rule.mday_mask == 0 && rule.mday_neg_mask == 0) {
                        rule.day_of_week_mask = 1 << get_day_of_week(start_y, start_m, start_d);
                    }

                    // End of the event (DTEND, or DTSTART plus DURATION). Timed events store the end instant;
                    // all-day events store the exclusive DTEND date.
                    int ey = start_y, em = start_m, ed = start_d;
                    int end_min = -1;
                    // A zoned start has an exact instant. zone_span is the elapsed seconds to the end, or -1 if unknown.
                    bool start_zoned = start_timed && start_zone[0] != '\0';
                    time_t start_instant = 0;
                    long long zone_span = -1;
                    if (start_zoned && !zone_to_instant(start_y, start_m, start_d, start_minute, start_zone, &start_instant)) {
                        start_zoned = false;
                    }
                    if (start_timed) {
                        bool has_end = false;
                        if (end_y > 0 && end_timed) {
                            ey = end_y; em = end_m; ed = end_d; end_min = end_minute;
                            has_end = true;
                        } else if (has_duration) {
                            int y2 = start_y, m2 = start_m, d2 = start_d, min2 = start_minute;
                            // A zoned start adds the exact elapsed time, so the end stays correct across DST changes.
                            // A floating start adds wall-clock time, as RFC 5545 specifies.
                            if (start_zoned && zone_wall_clock(start_instant + (time_t)duration_seconds, start_zone,
                                                               &y2, &m2, &d2, &min2)) {
                                zone_span = duration_seconds;
                            } else {
                                y2 = start_y; m2 = start_m; d2 = start_d; min2 = start_minute;
                                add_seconds_to_datetime(&y2, &m2, &d2, &min2, duration_seconds);
                            }
                            ey = y2; em = m2; ed = d2; end_min = min2;
                            has_end = true;
                        }
                        // The end must be later than the start; otherwise the event has no end time
                        if (has_end && timed_key(ey, em, ed, end_min) <= timed_key(start_y, start_m, start_d, start_minute)) {
                            has_end = false;
                        }
                        if (has_end && start_zoned) {
                            if (zone_span < 0) {
                                time_t end_instant = 0;
                                if (zone_to_instant(ey, em, ed, end_min, start_zone, &end_instant)) {
                                    zone_span = (long long)(end_instant - start_instant);
                                }
                            }
                            if (zone_span <= 0) has_end = false;
                        }
                        if (!has_end) {
                            ey = start_y; em = start_m; ed = start_d; end_min = -1;
                        }
                    } else {
                        if (end_y > 0) {
                            ey = end_y; em = end_m; ed = end_d;
                        } else if (has_duration) {
                            long long days = duration_seconds / 86400;
                            for (long long i = 0; i < days; i++) calendar_next_day(&ey, &em, &ed);
                        }
                        // RFC 5545 §3.6.1: an end that is not after the start means the event lasts one day
                        if (YMD_KEY(ey, em, ed) <= YMD_KEY(start_y, start_m, start_d)) {
                            ey = start_y; em = start_m; ed = start_d;
                            calendar_next_day(&ey, &em, &ed);
                        }
                    }

                    // An override of a single occurrence keeps its own event, under a UID derived from the
                    // master's UID and the occurrence date. The master excludes that date once the file is read.
                    bool is_override = has_recid && recid_key != 0 && uid[0] != '\0';
                    if (is_override) {
                        if (!pending_exclusions_add(&pending, uid, recid_key)) ok = false;
                        char base_uid[MAX_UID_LEN - 12];
                        copy_utf8_truncated(base_uid, sizeof(base_uid), uid);
                        snprintf(uid, sizeof(uid), "%s#%08d", base_uid, recid_key);
                    }

                    // Events without a UID get a content-derived one, so re-importing the file matches them
                    if (uid[0] == '\0') {
                        make_content_uid(uid, sizeof(uid), title,
                                         start_y, start_m, start_d, ey, em, ed,
                                         start_timed, start_minute, end_min, &rule);
                    }

                    // A UID repeated within the same file gets a "#N" suffix so each event is kept
                    char unique_uid[MAX_UID_LEN];
                    char uid_base[MAX_UID_LEN - 12];
                    copy_utf8_truncated(uid_base, sizeof(uid_base), uid);
                    strcpy(unique_uid, uid);
                    for (int n = 2; uid_set_contains(&seen, unique_uid); n++) {
                        snprintf(unique_uid, sizeof(unique_uid), "%s#%d", uid_base, n);
                    }
                    if (!uid_set_add(&seen, unique_uid)) ok = false;

                    // Re-importing replaces the event with the same UID rather than duplicating it
                    Event* previous;
                    while ((previous = find_event_by_uid(cal, unique_uid)) != NULL) {
                        calendar_delete_event(cal, previous->id);
                    }
                    Event* event = calendar_add_event_full(cal, unique_uid, dtstamp, title, desc, cat,
                                                           start_y, start_m, start_d,
                                                           ey, em, ed,
                                                           is_transparent, sequence,
                                                           rule.type != RECUR_NONE ? &rule : NULL);
                    if (!event) {
                        ok = false;
                    } else {
                        if (start_timed) {
                            calendar_event_set_timed(event, start_minute, ey, em, ed, end_min);
                        }
                        if (start_zoned && !calendar_event_set_zone(event, start_zone, zone_span < 0 ? 0 : zone_span)) {
                            ok = false;
                        }
                        for (int i = 0; i < exdates.count; i++) {
                            int k = exdates.items[i];
                            calendar_event_add_exdate(event, k / 10000, (k / 100) % 100, k % 100);
                        }
                        for (int i = 0; i < rdates.count; i++) {
                            int k = rdates.items[i];
                            calendar_event_add_rdate(event, k / 10000, (k / 100) % 100, k % 100);
                        }
                    }
                    date_list_free(&exdates);
                    date_list_free(&rdates);
                    in_event = false;
                }
            }
            else if (in_event) {
                if (strncmp(line, "UID", 3) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        copy_utf8_truncated(uid, sizeof(uid), val);
                    }
                }
                else if (strncmp(line, "DTSTAMP", 7) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        copy_utf8_truncated(dtstamp, sizeof(dtstamp), val);
                    }
                }
                else if (strncmp(line, "SUMMARY", 7) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        unescape_ics_text(val);
                        copy_utf8_truncated(title, sizeof(title), val);
                    }
                }
                else if (strncmp(line, "DESCRIPTION", 11) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        unescape_ics_text(val);
                        copy_utf8_truncated(desc, sizeof(desc), val);
                    }
                }
                else if (strncmp(line, "CATEGORIES", 10) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        copy_utf8_truncated(cat, sizeof(cat), val);
                    }
                }
                else if (strncmp(line, "TRANSP", 6) == 0) {
                    char* val = find_property_value_start(line);
                    if (val && strstr(val, "OPAQUE")) {
                        is_transparent = false;
                    } else {
                        is_transparent = true;
                    }
                }
                else if (strncmp(line, "SEQUENCE", 8) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        sequence = atoi(val);
                    }
                }
                else if (strncmp(line, "DTSTART", 7) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        int y = 0, m = 0, d = 0, minute = 0;
                        bool has_time = false;
                        char zone[MAX_ZONE_LEN];
                        const char* tzid = property_tzid(line, val, zone, sizeof(zone));
                        // The start keeps its own wall-clock time; its zone is what the event recurs in
                        if (parse_ics_value(val, NULL, NULL, &y, &m, &d, &has_time, &minute)) {
                            start_y = y;
                            start_m = m;
                            start_d = d;
                            start_timed = has_time;
                            start_minute = minute;
                            const char* source = has_time ? ics_value_zone(val, tzid) : NULL;
                            copy_utf8_truncated(start_zone, sizeof(start_zone), source ? source : "");
                        }
                    }
                }
                else if (strncmp(line, "DURATION", 8) == 0) {
                    char* val = find_property_value_start(line);
                    if (val && parse_duration_seconds(val, &duration_seconds)) {
                        has_duration = true;
                    }
                }
                else if (strncmp(line, "DTEND", 5) == 0 || strncmp(line, "EXDATE", 6) == 0 ||
                         strncmp(line, "RDATE", 5) == 0 || strncmp(line, "RECURRENCE-ID", 13) == 0 ||
                         strncmp(line, "RRULE:", 6) == 0) {
                    if (!string_list_add(&later, line)) ok = false;
                }
            }
        }

        if (raw[0] == '\0') break;
        strcpy(line, raw);
    }

    // Each override's occurrence is excluded from its master, which may have appeared after the override
    for (size_t i = 0; i < pending.count; i++) {
        Event* master = find_event_by_uid(cal, pending.items[i].uid);
        if (master) {
            calendar_event_add_exdate(master, pending.items[i].key / 10000,
                                      (pending.items[i].key / 100) % 100, pending.items[i].key % 100);
        }
    }
    free(pending.items);
    string_list_clear(&later);
    free(later.items);
    date_list_free(&exdates);
    date_list_free(&rdates);
    uid_set_free(&seen);
    fclose(file);
    return ok;
}

// Writes a date, or a timed value in zone. An empty zone is floating; "UTC" is written with a trailing Z.
static void write_date_property(FILE* file, const char* name, bool is_timed, const char* zone,
                                int year, int month, int day, int minute) {
    if (!is_timed) {
        fprintf(file, "%s;VALUE=DATE:%04d%02d%02d\r\n", name, year, month, day);
    } else if (zone[0] == '\0') {
        fprintf(file, "%s:%04d%02d%02dT%02d%02d00\r\n", name, year, month, day, minute / 60, minute % 60);
    } else if (strcmp(zone, "UTC") == 0) {
        fprintf(file, "%s:%04d%02d%02dT%02d%02d00Z\r\n", name, year, month, day, minute / 60, minute % 60);
    } else {
        fprintf(file, "%s;TZID=%s:%04d%02d%02dT%02d%02d00\r\n", name, zone, year, month, day, minute / 60, minute % 60);
    }
}

static void write_date_list(FILE* file, const char* name, const Event* ev, const char* zone, const DateList* list) {
    for (int i = 0; i < list->count; i++) {
        int key = list->items[i];
        write_date_property(file, name, ev->is_timed, zone, key / 10000, (key / 100) % 100, key % 100, ev->start_minute);
    }
}

// Writes a UTC offset as +HHMM, or +HHMMSS when it has seconds
static void format_utc_offset(char* out, size_t size, long offset) {
    char sign = offset < 0 ? '-' : '+';
    long magnitude = offset < 0 ? -offset : offset;
    if (magnitude % 60 == 0) {
        snprintf(out, size, "%c%02ld%02ld", sign, magnitude / 3600, (magnitude % 3600) / 60);
    } else {
        snprintf(out, size, "%c%02ld%02ld%02ld", sign, magnitude / 3600, (magnitude % 3600) / 60, magnitude % 60);
    }
}

// Writes an instant as a local DATE-TIME (no zone) in the given UTC offset
static void write_local_datetime(FILE* file, const char* name, time_t instant, long offset) {
    time_t local = instant + offset;
    struct tm* wall = gmtime(&local);
    if (wall == NULL) return;
    fprintf(file, "%s:%04d%02d%02dT%02d%02d%02d\r\n", name, wall->tm_year + 1900, wall->tm_mon + 1, wall->tm_mday,
            wall->tm_hour, wall->tm_min, wall->tm_sec);
}

// Offset that the state at index i changes from: the previous state's offset, or its own for the first state
static long observance_from(const ZoneOffset* states, size_t i) {
    return i == 0 ? states[0].offset : states[i - 1].offset;
}

// True when the states at i and k are changes into the same observance (same kind, same offsets)
static bool same_observance(const ZoneOffset* states, size_t i, size_t k) {
    return states[i].is_dst == states[k].is_dst && states[i].offset == states[k].offset &&
           observance_from(states, i) == observance_from(states, k);
}

// Writes STANDARD and DAYLIGHT observances. Each distinct observance starts at its first change (DTSTART) and lists
// its later changes as RDATE.
static void write_zone_observances(FILE* file, const ZoneOffset* states, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bool written = false;
        for (size_t j = 0; j < i && !written; j++) {
            written = same_observance(states, j, i);
        }
        if (written) continue;

        long from = observance_from(states, i);
        char from_text[32];
        char to_text[32];
        format_utc_offset(from_text, sizeof(from_text), from);
        format_utc_offset(to_text, sizeof(to_text), states[i].offset);
        const char* kind = states[i].is_dst ? "DAYLIGHT" : "STANDARD";

        fprintf(file, "BEGIN:%s\r\n", kind);
        write_local_datetime(file, "DTSTART", states[i].instant, from);
        fprintf(file, "TZOFFSETFROM:%s\r\n", from_text);
        fprintf(file, "TZOFFSETTO:%s\r\n", to_text);
        for (size_t k = i + 1; k < count; k++) {
            if (same_observance(states, i, k)) write_local_datetime(file, "RDATE", states[k].instant, from);
        }
        fprintf(file, "END:%s\r\n", kind);
    }
}

// Writes a VTIMEZONE for zone, covering from_year through to_year
static void write_timezone(FILE* file, const char* zone, int from_year, int to_year) {
    size_t count = 0;
    ZoneOffset* states = zone_offset_states(zone, from_year, to_year, &count);
    if (states == NULL) return;
    fprintf(file, "BEGIN:VTIMEZONE\r\nTZID:%s\r\n", zone);
    write_zone_observances(file, states, count);
    fprintf(file, "END:VTIMEZONE\r\n");
    free(states);
}

// Widens the VTIMEZONE year range so it includes year, with a year of margin on each side
static void include_year(int* from_year, int* to_year, int year) {
    if (year - 1 < *from_year) *from_year = year - 1;
    if (year + 1 > *to_year) *to_year = year + 1;
}

static void include_date_list_years(int* from_year, int* to_year, const DateList* list) {
    for (int i = 0; i < list->count; i++) {
        include_year(from_year, to_year, list->items[i] / 10000);
    }
}

bool calendar_export_ics(const CalendarState* cal, const char* filepath) {
    char clean_path[512];
    strncpy(clean_path, filepath, sizeof(clean_path) - 5);
    clean_path[sizeof(clean_path) - 5] = '\0';

    size_t len = strlen(clean_path);
    bool has_ics = len >= 4 && strcmp(clean_path + len - 4, ".ics") == 0;
    bool has_ical = len >= 5 && strcmp(clean_path + len - 5, ".ical") == 0;
    if (!has_ics && !has_ical) {
        strcat(clean_path, ".ics");
    }

    FILE* file = fopen(clean_path, "w");
    if (!file) return false;

    // RFC 5545 standard root calendar headers
    fprintf(file, "BEGIN:VCALENDAR\r\n");
    fprintf(file, "VERSION:2.0\r\n");
    fprintf(file, "PRODID:-//Microcal//Ncurses Calendar Linux//EN\r\n");
    fprintf(file, "CALSCALE:GREGORIAN\r\n");

    // Floating timed events are written in this machine's zone, so each timed value names its zone
    char local_zone[MAX_ZONE_LEN];
    bool has_local_zone = zone_local_name(local_zone, sizeof(local_zone));

    // Each zone used by a timed event gets a VTIMEZONE covering the years the events touch
    StringList zones = {NULL, 0, 0};
    int from_year = 1970;
    int to_year = 2037;
    for (const Event* ev = cal->events_head; ev != NULL; ev = ev->next) {
        if (!ev->is_timed) continue;
        include_year(&from_year, &to_year, ev->year);
        if (ev->end_year > 0) include_year(&from_year, &to_year, ev->end_year);
        include_date_list_years(&from_year, &to_year, &ev->exdates);
        include_date_list_years(&from_year, &to_year, &ev->rdates);
        if (ev->recurrence.has_until) include_year(&from_year, &to_year, ev->recurrence.until_year);

        const char* zone = ev->zone[0] != '\0' ? ev->zone : (has_local_zone ? local_zone : "");
        if (zone[0] == '\0' || strcmp(zone, "UTC") == 0 || !zone_is_known(zone)) continue;
        bool listed = false;
        for (size_t i = 0; i < zones.count && !listed; i++) {
            listed = strcmp(zones.items[i], zone) == 0;
        }
        if (!listed && !string_list_add(&zones, zone)) break;
    }
    for (size_t i = 0; i < zones.count; i++) {
        write_timezone(file, zones.items[i], from_year, to_year);
    }
    string_list_clear(&zones);
    free(zones.items);

    Event* current = cal->events_head;
    while (current != NULL) {
        const char* zone = current->zone[0] != '\0' ? current->zone : (has_local_zone ? local_zone : "");
        fprintf(file, "BEGIN:VEVENT\r\n");
        write_property(file, "UID", current->uid, false);

        // DTSTAMP is mandatory per RFC 5545 §3.6.1
        if (strlen(current->dtstamp) > 0) {
            fprintf(file, "DTSTAMP:%s\r\n", current->dtstamp);
        } else {
            time_t now = time(NULL);
            struct tm* gmt = gmtime(&now);
            char stamp[32];
            strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", gmt);
            fprintf(file, "DTSTAMP:%s\r\n", stamp);
        }

        write_property(file, "SUMMARY", current->title, true);

        if (strlen(current->category) > 0) {
            write_property(file, "CATEGORIES", current->category, false);
        }

        if (strlen(current->description) > 0) {
            write_property(file, "DESCRIPTION", current->description, true);
        }

        write_date_property(file, "DTSTART", current->is_timed, zone,
                            current->year, current->month, current->day, current->start_minute);
        if (current->is_timed ? current->end_minute >= 0 : current->end_year > 0) {
            write_date_property(file, "DTEND", current->is_timed, zone,
                                current->end_year, current->end_month, current->end_day, current->end_minute);
        }

        if (current->is_transparent) {
            fprintf(file, "TRANSP:TRANSPARENT\r\n");
        } else {
            fprintf(file, "TRANSP:OPAQUE\r\n");
        }

        if (current->sequence > 0) {
            fprintf(file, "SEQUENCE:%d\r\n", current->sequence);
        }

        write_rrule(file, current);
        write_date_list(file, "EXDATE", current, zone, &current->exdates);
        write_date_list(file, "RDATE", current, zone, &current->rdates);

        fprintf(file, "END:VEVENT\r\n");
        current = current->next;
    }

    fprintf(file, "END:VCALENDAR\r\n");
    fclose(file);

    calendar_save_last_filepath(clean_path);
    return true;
}