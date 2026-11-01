#include "io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_FILE ".microcal_last"

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

bool calendar_import_ics(CalendarState* cal, const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) return false;

    char line[8192];
    char raw[1024];
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
    bool is_recurring = false;
    RecurrenceRule rule;
    memset(&rule, 0, sizeof(RecurrenceRule));
    rule.wkst = -1;

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

        if (strlen(line) > 0) {
            if (strncmp(line, "NAME:", 5) == 0) {
                strncpy(cal->calendar_name, line + 5, sizeof(cal->calendar_name) - 1);
                cal->calendar_name[sizeof(cal->calendar_name) - 1] = '\0';
            }
            else if (strncmp(line, "X-WR-CALNAME:", 13) == 0 && cal->calendar_name[0] == '\0') {
                strncpy(cal->calendar_name, line + 13, sizeof(cal->calendar_name) - 1);
                cal->calendar_name[sizeof(cal->calendar_name) - 1] = '\0';
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
                is_recurring = false;
                memset(&rule, 0, sizeof(RecurrenceRule));
                rule.interval = 1;
                rule.wkst = -1;
            }
            else if (strcmp(line, "END:VEVENT") == 0) {
                if (in_event) {
                    calendar_add_event_full(cal, uid, dtstamp, title, desc, cat,
                                            start_y, start_m, start_d,
                                            end_y ? end_y : start_y,
                                            end_m ? end_m : start_m,
                                            end_d ? end_d : start_d,
                                            is_transparent, sequence,
                                            is_recurring ? &rule : NULL);
                    in_event = false;
                }
            }
            else if (in_event) {
                if (strncmp(line, "UID", 3) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        strncpy(uid, val, MAX_UID_LEN - 1);
                        uid[MAX_UID_LEN - 1] = '\0';
                    }
                }
                else if (strncmp(line, "DTSTAMP", 7) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        strncpy(dtstamp, val, MAX_TIMESTAMP_LEN - 1);
                        dtstamp[MAX_TIMESTAMP_LEN - 1] = '\0';
                    }
                }
                else if (strncmp(line, "SUMMARY", 7) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        unescape_ics_text(val);
                        strncpy(title, val, MAX_TITLE_LEN - 1);
                        title[MAX_TITLE_LEN - 1] = '\0';
                    }
                }
                else if (strncmp(line, "DESCRIPTION", 11) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        unescape_ics_text(val);
                        strncpy(desc, val, MAX_DESC_LEN - 1);
                        desc[MAX_DESC_LEN - 1] = '\0';
                    }
                }
                else if (strncmp(line, "CATEGORIES", 10) == 0) {
                    char* val = find_property_value_start(line);
                    if (val) {
                        strncpy(cat, val, MAX_CATEGORY_LEN - 1);
                        cat[MAX_CATEGORY_LEN - 1] = '\0';
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
                else if (strncmp(line, "RRULE:", 6) == 0) {
                    is_recurring = true;
                    rule.interval = 1;

                    if (strstr(line, "FREQ=YEARLY")) {
                        rule.type = RECUR_YEARLY;
                    } else if (strstr(line, "FREQ=MONTHLY")) {
                        rule.type = RECUR_MONTHLY;
                    } else if (strstr(line, "FREQ=WEEKLY")) {
                        rule.type = RECUR_WEEKLY;
                    }

                    char* int_ptr = strstr(line, "INTERVAL=");
                    if (int_ptr) {
                        rule.interval = atoi(int_ptr + 9);
                    }

                    char* bmd_ptr = strstr(line, "BYMONTHDAY=");
                    if (bmd_ptr) {
                        rule.day_of_month = atoi(bmd_ptr + 11);
                    }

                    char* bm_ptr = strstr(line, "BYMONTH=");
                    if (bm_ptr) {
                        rule.month_of_year = atoi(bm_ptr + 8);
                    }

                    char* byday_ptr = strstr(line, "BYDAY=");
                    if (byday_ptr) {
                        int wday = parse_weekday(byday_ptr + 6);
                        if (wday >= 0) {
                            rule.day_of_week_mask = (1 << wday);
                        }
                    }

                    char* wkst_ptr = strstr(line, "WKST=");
                    if (wkst_ptr) {
                        rule.wkst = parse_weekday(wkst_ptr + 5);
                    }
                }
            }
        }

        if (raw[0] == '\0') break;
        strcpy(line, raw);
    }

    fclose(file);
    return true;
}

bool calendar_export_ics(const CalendarState* cal, const char* filepath) {
    char clean_path[512];
    strncpy(clean_path, filepath, sizeof(clean_path) - 5);
    clean_path[sizeof(clean_path) - 5] = '\0';

    size_t len = strlen(clean_path);
    if (len < 4 || (strcmp(clean_path + len - 4, ".ics") != 0 && strcmp(clean_path + len - 5, ".ical") != 0)) {
        strcat(clean_path, ".ics");
    }

    FILE* file = fopen(clean_path, "w");
    if (!file) return false;

    // RFC 5545 standard root calendar headers
    fprintf(file, "BEGIN:VCALENDAR\r\n");
    fprintf(file, "VERSION:2.0\r\n");
    fprintf(file, "PRODID:-//Microcal//Ncurses Calendar Linux//EN\r\n");
    fprintf(file, "CALSCALE:GREGORIAN\r\n");

    static const char* wday_names[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};

    Event* current = cal->events_head;
    while (current != NULL) {
        fprintf(file, "BEGIN:VEVENT\r\n");
        fprintf(file, "UID:%s\r\n", current->uid);

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

        fprintf(file, "SUMMARY:%s\r\n", current->title);

        if (strlen(current->category) > 0) {
            fprintf(file, "CATEGORIES:%s\r\n", current->category);
        }

        if (strlen(current->description) > 0) {
            fprintf(file, "DESCRIPTION:");
            for (const char* p = current->description; *p; p++) {
                if (*p == '\n') {
                    fputs("\\n", file);
                } else if (*p == '\\' && (*(p + 1) == 'n' || *(p + 1) == 'N')) {
                    // Prevent double-escaping if a literal \n was preserved
                    fputs("\\n", file);
                    p++;
                } else if (*p == ',' || *p == ';') {
                    fputc('\\', file);
                    fputc(*p, file);
                } else {
                    fputc(*p, file);
                }
            }
            fprintf(file, "\r\n");
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

        if (current->is_recurring) {
            if (current->recurrence.type == RECUR_YEARLY) {
                fprintf(file, "RRULE:FREQ=YEARLY");
                if (current->recurrence.interval > 1) {
                    fprintf(file, ";INTERVAL=%d", current->recurrence.interval);
                }
                if (current->recurrence.month_of_year > 0) {
                    fprintf(file, ";BYMONTH=%d", current->recurrence.month_of_year);
                }
                if (current->recurrence.day_of_month > 0) {
                    fprintf(file, ";BYMONTHDAY=%d", current->recurrence.day_of_month);
                }
                if (current->recurrence.wkst >= 0 && current->recurrence.wkst < 7) {
                    fprintf(file, ";WKST=%s", wday_names[current->recurrence.wkst]);
                }
                fprintf(file, "\r\n");
            } else if (current->recurrence.type == RECUR_MONTHLY) {
                fprintf(file, "RRULE:FREQ=MONTHLY");
                if (current->recurrence.interval > 1) {
                    fprintf(file, ";INTERVAL=%d", current->recurrence.interval);
                }
                if (current->recurrence.day_of_month > 0) {
                    fprintf(file, ";BYMONTHDAY=%d", current->recurrence.day_of_month);
                }
                if (current->recurrence.wkst >= 0 && current->recurrence.wkst < 7) {
                    fprintf(file, ";WKST=%s", wday_names[current->recurrence.wkst]);
                }
                fprintf(file, "\r\n");
            } else if (current->recurrence.type == RECUR_WEEKLY) {
                int w = get_day_of_week(current->year, current->month, current->day);
                for (int i = 0; i < 7; i++) {
                    if (current->recurrence.day_of_week_mask & (1 << i)) {
                        w = i;
                        break;
                    }
                }
                fprintf(file, "RRULE:FREQ=WEEKLY;BYDAY=%s", wday_names[w]);
                if (current->recurrence.wkst >= 0 && current->recurrence.wkst < 7) {
                    fprintf(file, ";WKST=%s", wday_names[current->recurrence.wkst]);
                }
                fprintf(file, "\r\n");
            }
        }

        fprintf(file, "END:VEVENT\r\n");
        current = current->next;
    }

    fprintf(file, "END:VCALENDAR\r\n");
    fclose(file);

    calendar_save_last_filepath(clean_path);
    return true;
}