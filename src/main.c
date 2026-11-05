#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "calendar.h"
#include "ui.h"
#include "io.h"

static const char* const weekday_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static const char* ordinal_suffix(int n) {
    if (n % 100 >= 11 && n % 100 <= 13) return "th";
    switch (n % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
    }
}

// Accepts YYYY-MM-DD on or after the start date
static bool parse_repeat_until(const char* text, int start_y, int start_m, int start_d,
                               int* y, int* m, int* d) {
    int ty = 0, tm = 0, td = 0, consumed = 0;
    if (sscanf(text, "%d-%d-%d%n", &ty, &tm, &td, &consumed) != 3 || text[consumed] != '\0') return false;
    if (ty < start_y || ty > 9999) return false;
    if (tm < 1 || tm > 12 || td < 1 || td > days_in_month(ty, tm)) return false;
    if (ty == start_y && (tm < start_m || (tm == start_m && td < start_d))) return false;
    *y = ty;
    *m = tm;
    *d = td;
    return true;
}

// Accepts HH:MM (24-hour); stores minutes since midnight
static bool parse_time_of_day(const char* text, int* minute) {
    int hh = 0, mm = 0, consumed = 0;
    if (sscanf(text, "%d:%d%n", &hh, &mm, &consumed) != 2 || text[consumed] != '\0') return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    *minute = hh * 60 + mm;
    return true;
}

// Prompts for a start time and an optional end time on the same day. Returns false if cancelled.
static bool prompt_event_time(int* start_minute, int* end_minute) {
    char text[8];
    for (;;) {
        text[0] = '\0';
        if (!ui_show_prompt("Start time", "Start time (HH:MM)", text, sizeof(text))) return false;
        if (parse_time_of_day(text, start_minute)) break;
        ui_show_message("Invalid time", "Enter a time as HH:MM, from 00:00 to 23:59.");
    }
    for (;;) {
        text[0] = '\0';
        if (!ui_show_prompt_optional("End time", "End time (HH:MM, blank for none)", text, sizeof(text))) return false;
        if (text[0] == '\0') {
            *end_minute = -1;
            return true;
        }
        int candidate = 0;
        if (parse_time_of_day(text, &candidate) && candidate > *start_minute) {
            *end_minute = candidate;
            return true;
        }
        ui_show_message("Invalid time", "Enter a time later than the start time, or leave it blank.");
    }
}

static void add_event_dialog(CalendarState* cal) {
    char title_buf[MAX_TITLE_LEN] = {0};
    if (!ui_show_prompt("New Event", "Enter event title", title_buf, sizeof(title_buf))) return;

    int year = cal->current_year;
    int month = cal->current_month;
    int day = cal->selected_day;
    int wd = get_day_of_week(year, month, day);
    int nth = (day - 1) / 7 + 1;

    char weekday_opt[64];
    char monthday_opt[64];
    char nth_opt[80];
    snprintf(weekday_opt, sizeof(weekday_opt), "< Repeat on every %s >", weekday_names[wd]);
    snprintf(monthday_opt, sizeof(monthday_opt), "< Repeat on every %d%s of the month >", day, ordinal_suffix(day));
    snprintf(nth_opt, sizeof(nth_opt), "< Repeat on every %d%s %s of the month >",
             nth, ordinal_suffix(nth), weekday_names[wd]);

    enum { OPT_NONE, OPT_WEEKLY, OPT_MONTH_DAY, OPT_MONTH_NTH, OPT_YEARLY, OPT_COUNT };
    const char* options[OPT_COUNT] = {
        "< No repeat >", weekday_opt, monthday_opt, nth_opt, "< Repeat once a year >"
    };

    int choice = ui_show_choice("Repeat", options, OPT_COUNT, OPT_NONE);
    if (choice < 0) return;

    bool repeats = choice != OPT_NONE;

    RecurrenceRule rule;
    memset(&rule, 0, sizeof(rule));
    rule.interval = 1;
    rule.wkst = -1;

    if (choice == OPT_WEEKLY) {
        rule.type = RECUR_WEEKLY;
        rule.day_of_week_mask = 1 << wd;
        rule.ordinal_mask[wd] = ORDINAL_ALL;
    } else if (choice == OPT_MONTH_DAY) {
        rule.type = RECUR_MONTHLY;
        rule.mday_mask = MDAY_BIT(day);
    } else if (choice == OPT_MONTH_NTH) {
        rule.type = RECUR_MONTHLY;
        rule.day_of_week_mask = 1 << wd;
        rule.ordinal_mask[wd] = recurrence_ordinal_bit(nth);
    } else if (choice == OPT_YEARLY) {
        rule.type = RECUR_YEARLY;
        rule.month_mask = MONTH_BIT(month);
        rule.mday_mask = MDAY_BIT(day);
    }

    if (repeats) {
        const char* unit = choice == OPT_WEEKLY ? "weeks" : (choice == OPT_YEARLY ? "years" : "months");
        char every_label[32];
        snprintf(every_label, sizeof(every_label), "Every 2 %s", unit);
        const char* labels[2] = { every_label, "Ends on a date" };
        bool checked[2] = { false, false };
        if (!ui_show_checkboxes("Repeat options", labels, checked, 2)) return;

        if (checked[0]) rule.interval = 2;

        if (checked[1]) {
            char text[16];
            for (;;) {
                text[0] = '\0';
                if (!ui_show_prompt("Repeat", "Ends on YYYY-MM-DD", text, sizeof(text))) return;
                int until_y, until_m, until_d;
                if (parse_repeat_until(text, year, month, day, &until_y, &until_m, &until_d)) {
                    rule.has_until = true;
                    rule.until_year = until_y;
                    rule.until_month = until_m;
                    rule.until_day = until_d;
                    break;
                }
                ui_show_message("Invalid date", "Enter a date as YYYY-MM-DD, on or after the event date.");
            }
        }
    }

    const char* time_labels[1] = { "Set a time (start, optional end)" };
    bool time_checked[1] = { false };
    if (!ui_show_checkboxes("Time", time_labels, time_checked, 1)) return;

    int start_minute = 0;
    int end_minute = -1;
    if (time_checked[0] && !prompt_event_time(&start_minute, &end_minute)) return;

    Event* event = calendar_add_event(cal, title_buf, "", year, month, day, repeats ? &rule : NULL);
    if (!event) {
        ui_show_message("Error", "Could not add the event.");
        return;
    }
    if (time_checked[0]) {
        calendar_event_set_timed(event, start_minute, year, month, day, end_minute);
    }
    ui_show_message("Success", "Event added successfully!");
}

int main(void) {
    ui_init();

    time_t raw_time = time(NULL);
    struct tm* tm_now = localtime(&raw_time);
    int start_year = tm_now ? tm_now->tm_year + 1900 : 2026;
    int start_month = tm_now ? tm_now->tm_mon + 1 : 11;
    int start_day = tm_now ? tm_now->tm_mday : 1;

    CalendarState cal;
    calendar_init(&cal, start_year, start_month);
    cal.selected_day = start_day;

    char last_file[512] = {0};
    bool loaded = false;
    if (calendar_get_last_filepath(last_file, sizeof(last_file))) {
        if (calendar_import_ics(&cal, last_file)) {
            loaded = true;
        }
    }

    if (!loaded) {
        calendar_add_event(&cal, "Project Kickoff", "Initial release of Microcal", 2026, 11, 15, NULL);
    }

    ColorTheme current_theme = THEME_DEFAULT;
    bool running = true;
    int ch;

    while (running) {
        ui_refresh_all(&cal);
        ch = getch();

        switch (ch) {
        case KEY_RESIZE:
            break;

        case 'f':
        case 'F':
            ui_toggle_details();
            break;

        case KEY_UP:
            if (cal.selected_day > 7) {
                cal.selected_day -= 7;
            }
            break;
        case KEY_DOWN: {
            int max_days = days_in_month(cal.current_year, cal.current_month);
            if (cal.selected_day + 7 <= max_days) {
                cal.selected_day += 7;
            }
            break;
        }
        case KEY_LEFT:
            if (cal.selected_day > 1) {
                cal.selected_day--;
            }
            break;
        case KEY_RIGHT: {
            int max_days = days_in_month(cal.current_year, cal.current_month);
            if (cal.selected_day < max_days) {
                cal.selected_day++;
            }
            break;
        }
        case KEY_PPAGE: // Previous Month
            cal.current_month--;
            if (cal.current_month < 1) {
                cal.current_month = 12;
                cal.current_year--;
            }
            if (cal.selected_day > days_in_month(cal.current_year, cal.current_month)) {
                cal.selected_day = days_in_month(cal.current_year, cal.current_month);
            }
            break;
        case KEY_NPAGE: // Next Month
            cal.current_month++;
            if (cal.current_month > 12) {
                cal.current_month = 1;
                cal.current_year++;
            }
            if (cal.selected_day > days_in_month(cal.current_year, cal.current_month)) {
                cal.selected_day = days_in_month(cal.current_year, cal.current_month);
            }
            break;
        case '\n':
        case KEY_ENTER:
        case 'v':
        case 'V': {
            EventOccurrence day_events[MAX_DAY_EVENTS];
            int count = calendar_get_events_for_day(&cal, cal.current_year, cal.current_month, cal.selected_day, day_events, MAX_DAY_EVENTS);
            int del_id = ui_show_day_events(&cal, day_events, count);
            if (del_id > 0) {
                if (calendar_delete_event(&cal, del_id)) {
                    ui_show_message("Deleted", "Event was deleted successfully.");
                }
            }
            break;
        }
        case 5: // Ctrl + E fallback
        case 'e':
        case 'E':
            add_event_dialog(&cal);
            break;
        case 19: // Ctrl + S fallback
        case 's':
        case 'S': {
            char path_buf[256];
            if (!calendar_get_last_filepath(path_buf, sizeof(path_buf))) {
                strncpy(path_buf, "calendar_export.ics", sizeof(path_buf) - 1);
            }

            if (ui_show_prompt("Export Calendar", "Enter filename to save (.ics)", path_buf, sizeof(path_buf))) {
                if (calendar_export_ics(&cal, path_buf)) {
                    ui_show_message("Success", "Calendar saved successfully!");
                } else {
                    ui_show_message("Error", "Failed to export calendar file.");
                }
            }
            break;
        }
        case KEY_F(1):
            ui_show_message("Help & Keyguide",
                "Arrow Keys: Navigate days\n"
                "Mouse Click / Double-Click: Select day / View events\n"
                "Mouse Scroll: Previous / Next month\n"
                "PageUp/PageDown: Switch month\n"
                "Enter / v: View and delete events on selected day\n"
                "e: Add event on selected day (with optional repeat)\n"
                "s: Save / Export to ICS\n"
                "f: Toggle expanded event details (Type/Tags)\n"
                "F2: Import ICS file\n"
                "F3: Export ICS file\n"
                "t: Cycle color theme\n"
                "q: Quit application");
            break;
        case KEY_F(2): {
            char path_buf[256] = "import.ics";
            if (ui_show_prompt("Import Calendar", "Enter filename to import (.ics)", path_buf, sizeof(path_buf))) {
                if (calendar_import_ics(&cal, path_buf)) {
                    calendar_save_last_filepath(path_buf);
                    ui_show_message("Success", "Calendar imported successfully!");
                } else {
                    ui_show_message("Error", "Failed to import file (check if file exists).");
                }
            }
            break;
        }
        case KEY_F(3): {
            char path_buf[256];
            if (!calendar_get_last_filepath(path_buf, sizeof(path_buf))) {
                strncpy(path_buf, "calendar_export.ics", sizeof(path_buf) - 1);
            }
            if (ui_show_prompt("Export Calendar", "Enter filename (.ics)", path_buf, sizeof(path_buf))) {
                if (calendar_export_ics(&cal, path_buf)) {
                    ui_show_message("Success", "Exported successfully!");
                } else {
                    ui_show_message("Error", "Failed to export calendar file.");
                }
            }
            break;
        }
        case 20: // Ctrl + T fallback
        case 't':
        case 'T':
            current_theme = (current_theme % 3) + 1;
            ui_set_theme(current_theme);
            break;
        case 17: // Ctrl + Q fallback
        case 'q':
        case 'Q':
            running = false;
            break;

        case KEY_MOUSE: {
            MEVENT event;
            if (getmouse(&event) == OK) {
                #if defined(BUTTON4_PRESSED) || defined(BUTTON5_PRESSED)
                if (event.bstate & BUTTON4_PRESSED) {
                    cal.current_month--;
                    if (cal.current_month < 1) {
                        cal.current_month = 12;
                        cal.current_year--;
                    }
                    if (cal.selected_day > days_in_month(cal.current_year, cal.current_month)) {
                        cal.selected_day = days_in_month(cal.current_year, cal.current_month);
                    }
                    break;
                } else if (event.bstate & BUTTON5_PRESSED) {
                    cal.current_month++;
                    if (cal.current_month > 12) {
                        cal.current_month = 1;
                        cal.current_year++;
                    }
                    if (cal.selected_day > days_in_month(cal.current_year, cal.current_month)) {
                        cal.selected_day = days_in_month(cal.current_year, cal.current_month);
                    }
                    break;
                }
                #endif

                if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED)) {
                    int clicked_day = ui_get_day_at_coords(&cal, event.y, event.x);
                    if (clicked_day > 0) {
                        cal.selected_day = clicked_day;

                        if (event.bstate & BUTTON1_DOUBLE_CLICKED) {
                            EventOccurrence day_events[MAX_DAY_EVENTS];
                            int count = calendar_get_events_for_day(&cal, cal.current_year, cal.current_month, cal.selected_day, day_events, MAX_DAY_EVENTS);
                            int del_id = ui_show_day_events(&cal, day_events, count);
                            if (del_id > 0) {
                                if (calendar_delete_event(&cal, del_id)) {
                                    ui_show_message("Deleted", "Event was deleted successfully.");
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        }
    }

    calendar_free_events(&cal);
    ui_cleanup();
    return 0;
}