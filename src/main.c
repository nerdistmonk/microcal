#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "calendar.h"
#include "ui.h"
#include "io.h"

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
            Event* day_events[16];
            int count = calendar_get_events_for_day(&cal, cal.current_year, cal.current_month, cal.selected_day, day_events, 16);
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
        case 'E': {
            char title_buf[MAX_TITLE_LEN] = {0};
            if (ui_show_prompt("New Event", "Enter event title", title_buf, sizeof(title_buf))) {
                calendar_add_event(&cal, title_buf, "Created from Microcal UI", cal.current_year, cal.current_month, cal.selected_day, NULL);
                ui_show_message("Success", "Event added successfully!");
            }
            break;
        }
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
                "PageUp/PageDown: Switch month\n"
                "Enter / v: View and delete events on selected day\n"
                "e: Add event on selected day\n"
                "s: Save / Export to ICS\n"
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
                // Mouse handling hook
            }
            break;
        }
        }
    }

    calendar_free_events(&cal);
    ui_cleanup();
    return 0;
}