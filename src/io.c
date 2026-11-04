#include "io.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_FILE ".microcal_last"
#define MAX_DURATION_DAYS 36500 // Clamp for DURATION (about 100 years)

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
                             const RecurrenceRule* rule) {
    unsigned long hash = fnv_append(2166136261UL, title);
    const int dates[] = {sy, sm, sd, ey, em, ed};
    for (size_t i = 0; i < sizeof(dates) / sizeof(dates[0]); i++) {
        hash = fnv_append_int(hash, dates[i]);
    }
    const int fields[] = {rule->type, rule->interval, rule->day_of_week_mask, rule->day_of_month,
                          rule->month_of_year, rule->count, rule->has_until, rule->until_year,
                          rule->until_month, rule->until_day, rule->wkst};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        hash = fnv_append_int(hash, fields[i]);
    }
    for (int i = 0; i < 7; i++) {
        hash = fnv_append_int(hash, rule->ordinal_mask[i]);
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

// Parses an RRULE line into rule. Unsupported parts (BYSETPOS, BYHOUR, ...) are ignored.
static void parse_rrule(const char* line, RecurrenceRule* rule) {
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
        rule->day_of_month = atoi(v);
    }
    if ((v = rrule_value(line, "BYMONTH=")) != NULL) {
        rule->month_of_year = atoi(v);
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
        int y = 0, m = 0, d = 0;
        if (sscanf(v, "%4d%2d%2d", &y, &m, &d) == 3 && m >= 1 && m <= 12 &&
            d >= 1 && d <= days_in_month(y, m)) {
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

// DURATION:P<n>D or P<n>W as whole days. Time components are ignored (the data model is date-only).
static int parse_duration_days(const char* value) {
    if (value[0] != 'P') return 0;
    char* end = NULL;
    long n = strtol(value + 1, &end, 10);
    if (end == value + 1 || n <= 0) return 0;
    if (*end != 'D' && *end != 'W') return 0;
    if (n > MAX_DURATION_DAYS) return MAX_DURATION_DAYS;
    long days = (*end == 'W') ? n * 7 : n;
    return days > MAX_DURATION_DAYS ? MAX_DURATION_DAYS : (int)days;
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

    char value[512];
    snprintf(value, sizeof(value), "FREQ=%s", freq);
    if (r->interval > 1) rule_append(value, sizeof(value), ";INTERVAL=%d", r->interval);
    if (r->type == RECUR_YEARLY && r->month_of_year > 0) {
        rule_append(value, sizeof(value), ";BYMONTH=%d", r->month_of_year);
    }
    if ((r->type == RECUR_MONTHLY || r->type == RECUR_YEARLY) && r->day_of_month > 0) {
        rule_append(value, sizeof(value), ";BYMONTHDAY=%d", r->day_of_month);
    }

    int mask = r->day_of_week_mask;
    if (r->type == RECUR_WEEKLY && mask == 0) {
        mask = 1 << get_day_of_week(ev->year, ev->month, ev->day);
    }
    if (mask != 0) {
        bool ordinals_allowed = r->type == RECUR_MONTHLY || r->type == RECUR_YEARLY;
        bool first = true;
        rule_append(value, sizeof(value), ";BYDAY=");
        for (int wd = 0; wd < 7; wd++) {
            if (!(mask & (1 << wd))) continue;
            int om = r->ordinal_mask[wd];
            if (!ordinals_allowed || om == 0 || (om & ORDINAL_ALL)) {
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

    if (r->has_until) {
        rule_append(value, sizeof(value), ";UNTIL=%04d%02d%02d", r->until_year, r->until_month, r->until_day);
    }
    if (r->count > 0) rule_append(value, sizeof(value), ";COUNT=%d", r->count);
    if (r->wkst >= 0 && r->wkst < 7) rule_append(value, sizeof(value), ";WKST=%s", wday_names[r->wkst]);

    write_property(file, "RRULE", value, false);
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
    bool is_transparent = true;
    int sequence = 0;
    bool in_event = false;
    bool in_alarm = false;
    int dur_days = 0;
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
                end_y = 0; end_m = 0; end_d = 0;
                is_transparent = true;
                sequence = 0;
                dur_days = 0;
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
                    // RFC 5545 §3.3.10: If FREQ=WEEKLY has no BYDAY, default to DTSTART weekday.
                    // Evaluated here so start_y/m/d is guaranteed fully parsed regardless of line ordering.
                    if (rule.type == RECUR_WEEKLY && rule.day_of_week_mask == 0) {
                        rule.day_of_week_mask = 1 << get_day_of_week(start_y, start_m, start_d);
                    }

                    // RFC 5545 §3.6.1: DTEND (or DURATION when DTEND is absent) must be later than DTSTART;
                    // otherwise the event lasts one day
                    int ey = end_y, em = end_m, ed = end_d;
                    if (end_y == 0) {
                        ey = start_y; em = start_m; ed = start_d;
                        for (int i = 0; i < dur_days; i++) calendar_next_day(&ey, &em, &ed);
                    }
                    if (ey * 10000 + em * 100 + ed <= start_y * 10000 + start_m * 100 + start_d) {
                        ey = start_y; em = start_m; ed = start_d;
                        calendar_next_day(&ey, &em, &ed);
                    }

                    // Events without a UID get a content-derived one, so re-importing the file matches them
                    if (uid[0] == '\0') {
                        make_content_uid(uid, sizeof(uid), title,
                                         start_y, start_m, start_d, ey, em, ed, &rule);
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
                    calendar_add_event_full(cal, unique_uid, dtstamp, title, desc, cat,
                                            start_y, start_m, start_d,
                                            ey, em, ed,
                                            is_transparent, sequence,
                                            rule.type != RECUR_NONE ? &rule : NULL);
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
                        int y = 0, m = 0, d = 0;
                        if (sscanf(val, "%4d%2d%2d", &y, &m, &d) == 3) {
                            start_y = y;
                            start_m = m;
                            start_d = d;
                        }
                    }
                }
                else if (strncmp(line, "DTEND", 5) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        int y = 0, m = 0, d = 0;
                        if (sscanf(val, "%4d%2d%2d", &y, &m, &d) == 3) {
                            end_y = y;
                            end_m = m;
                            end_d = d;
                        }
                    }
                }
                else if (strncmp(line, "DURATION", 8) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        dur_days = parse_duration_days(val);
                    }
                }
                else if (strncmp(line, "RRULE:", 6) == 0) {
                    parse_rrule(line, &rule);
                }
            }
        }

        if (raw[0] == '\0') break;
        strcpy(line, raw);
    }

    uid_set_free(&seen);
    fclose(file);
    return ok;
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

    Event* current = cal->events_head;
    while (current != NULL) {
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

        fprintf(file, "DTSTART;VALUE=DATE:%04d%02d%02d\r\n", current->year, current->month, current->day);
        if (current->end_year > 0) {
            fprintf(file, "DTEND;VALUE=DATE:%04d%02d%02d\r\n", current->end_year, current->end_month, current->end_day);
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

        fprintf(file, "END:VEVENT\r\n");
        current = current->next;
    }

    fprintf(file, "END:VCALENDAR\r\n");
    fclose(file);

    calendar_save_last_filepath(clean_path);
    return true;
}