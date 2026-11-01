#include "ui.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

void ui_init(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);

    if (has_colors()) {
        start_color();
        ui_set_theme(THEME_DEFAULT);
    }
}

void ui_cleanup(void) {
    endwin();
}

void ui_set_theme(ColorTheme theme) {
    if (!has_colors()) return;
    switch (theme) {
    case THEME_OCEAN:
        init_pair(1, COLOR_CYAN, COLOR_BLUE);
        init_pair(2, COLOR_WHITE, COLOR_BLUE);
        init_pair(3, COLOR_RED, COLOR_BLUE);
        init_pair(4, COLOR_BLACK, COLOR_CYAN);
        init_pair(5, COLOR_YELLOW, COLOR_CYAN);
        break;
    case THEME_FOREGROUND_POP:
        init_pair(1, COLOR_YELLOW, COLOR_BLACK);
        init_pair(2, COLOR_GREEN, COLOR_BLACK);
        init_pair(3, COLOR_RED, COLOR_BLACK);
        init_pair(4, COLOR_WHITE, COLOR_RED);
        init_pair(5, COLOR_BLACK, COLOR_MAGENTA);
        break;
    case THEME_DEFAULT:
    default:
        init_pair(1, COLOR_WHITE, COLOR_BLUE);
        init_pair(2, COLOR_WHITE, COLOR_BLACK);
        init_pair(3, COLOR_RED, COLOR_BLACK);
        init_pair(4, COLOR_WHITE, COLOR_BLACK);
        init_pair(5, COLOR_BLACK, COLOR_CYAN);
        break;
    }
}

void ui_draw_header(const CalendarState* cal) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_y;

    attron(COLOR_PAIR(1));
    mvprintw(0, 0, "%-*s", max_x, " Microcal - Linux 64-bit Ncurses Calendar");

    const char* months[] = {
        "", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    char month_year[64];
    if (cal->current_month >= 1 && cal->current_month <= 12) {
        snprintf(month_year, sizeof(month_year), " %s %d ", months[cal->current_month], cal->current_year);
    } else {
        snprintf(month_year, sizeof(month_year), " Month %d %d ", cal->current_month, cal->current_year);
    }

    mvprintw(1, 0, "%-*s", max_x, month_year);
    attroff(COLOR_PAIR(1));
}

void ui_draw_grid(const CalendarState* cal) {
    attron(COLOR_PAIR(2));
    // Each column is exactly 7 characters wide: 3-char name + 4 spacing/divider chars
    mvprintw(3, 2, "  Sun  |  Mon  |  Tue  |  Wed  |  Thu  |  Fri  |  Sat  ");

    time_t raw_time = time(NULL);
    struct tm* tm_now = localtime(&raw_time);
    int real_year = tm_now ? tm_now->tm_year + 1900 : 0;
    int real_month = tm_now ? tm_now->tm_mon + 1 : 0;
    int real_day = tm_now ? tm_now->tm_mday : 0;

    int start_row = 5;
    int start_col = 2;
    const int col_width = 8; // "  Sun  |" is 8 characters with divider

    int total_days = days_in_month(cal->current_year, cal->current_month);
    int first_day_wday = get_day_of_week(cal->current_year, cal->current_month, 1);

    int row = start_row;
    int col = start_col + (first_day_wday * col_width);

    for (int day = 1; day <= total_days; day++) {
        bool is_today = (cal->current_year == real_year &&
                         cal->current_month == real_month &&
                         day == real_day);
        bool is_cursor = (day == cal->selected_day);

        if (is_cursor) {
            attron(COLOR_PAIR(5) | A_BOLD);
        } else if (is_today) {
            attron(COLOR_PAIR(3) | A_BOLD);
        }

        bool has_events = false;
        Event* curr = cal->events_head;
        while (curr) {
            if (event_occurs_on(curr, cal->current_year, cal->current_month, day)) {
                has_events = true;
                break;
            }
            curr = curr->next;
        }

        // Print centered under each 7-char day name (column slot)
        if (has_events) {
            mvprintw(row, col, " [%02d*] ", day);
        } else {
            mvprintw(row, col, "  %02d   ", day);
        }

        if (is_cursor) {
            attroff(COLOR_PAIR(5) | A_BOLD);
        } else if (is_today) {
            attroff(COLOR_PAIR(3) | A_BOLD);
        }

        col += col_width;
        if ((first_day_wday + day) % 7 == 0) {
            col = start_col;
            row += 2;
        }
    }
    attroff(COLOR_PAIR(2));
}

void ui_draw_status_bar(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(COLOR_PAIR(4));
    mvprintw(max_y - 1, 0, "%-*s", max_x, " F1:Help ENTER:View/Del e:Add s:Save F2:Import F3:Export t:Theme q:Quit");
    attroff(COLOR_PAIR(4));
}

void ui_refresh_all(const CalendarState* cal) {
    clear();
    ui_draw_header(cal);
    ui_draw_grid(cal);
    ui_draw_status_bar();
    refresh();
}

void ui_show_message(const char* title, const char* message) {
    int line_count = 1;
    int max_len = (int)strlen(title);
    int cur_len = 0;

    for (const char* p = message; *p; p++) {
        if (*p == '\n') {
            line_count++;
            if (cur_len > max_len) max_len = cur_len;
            cur_len = 0;
        } else {
            cur_len++;
        }
    }
    if (cur_len > max_len) max_len = cur_len;

    int win_h = line_count + 6;
    int win_w = max_len + 6;
    if (win_w < 50) win_w = 50;
    if (win_h > LINES - 2) win_h = LINES - 2;
    if (win_w > COLS - 2) win_w = COLS - 2;

    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;

    WINDOW* win = newwin(win_h, win_w, start_y, start_x);
    if (!win) return;

    box(win, 0, 0);
    mvwprintw(win, 1, 2, "[ %s ]", title);

    int cur_row = 3;
    char buffer[512];
    const char* start = message;
    while (*start && cur_row < win_h - 2) {
        const char* next_newline = strchr(start, '\n');
        size_t len = next_newline ? (size_t)(next_newline - start) : strlen(start);
        if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
        strncpy(buffer, start, len);
        buffer[len] = '\0';

        mvwprintw(win, cur_row++, 2, "%s", buffer);
        if (!next_newline) break;
        start = next_newline + 1;
    }

    mvwprintw(win, win_h - 2, 2, "Press any key to close...");
    wrefresh(win);
    wgetch(win);
    delwin(win);

    touchwin(stdscr);
    refresh();
}

bool ui_show_prompt(const char* title, const char* prompt, char* buf, int max_len) {
    int win_h = 7;
    int win_w = 60;

    if (LINES < win_h || COLS < win_w) {
        return false;
    }

    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;

    WINDOW* win = newwin(win_h, win_w, start_y, start_x);
    if (!win) {
        return false;
    }

    box(win, 0, 0);
    mvwprintw(win, 1, 2, "%s", title);
    mvwprintw(win, 3, 2, "%s: ", prompt);
    wrefresh(win);

    echo();
    curs_set(1);
    wgetnstr(win, buf, max_len - 1);
    noecho();
    curs_set(0);
    delwin(win);

    touchwin(stdscr);
    refresh();

    return strlen(buf) > 0;
}

static int count_desc_lines(const char* desc) {
    if (!desc || strlen(desc) == 0) return 0;
    int count = 1;
    for (const char* p = desc; *p; p++) {
        if (*p == '\n') count++;
    }
    return count;
}

int ui_show_day_events(const CalendarState* cal, Event* events[], int count) {
    int win_w = COLS - 10;
    if (win_w > 80) win_w = 80;
    if (win_w < 50) win_w = 50;

    int total_content_lines = 0;
    for (int i = 0; i < count; i++) {
        total_content_lines += 1;
        total_content_lines += count_desc_lines(events[i]->description);
        total_content_lines += 1;
    }

    int win_h = (count == 0) ? 7 : (total_content_lines + 5);
    if (win_h > LINES - 2) win_h = LINES - 2;

    WINDOW* win = newwin(win_h, win_w, (LINES - win_h) / 2, (COLS - win_w) / 2);
    if (!win) return 0;

    keypad(win, TRUE);

    char header[64];
    snprintf(header, sizeof(header), " Events for %04d-%02d-%02d (%d) ",
             cal->current_year, cal->current_month, cal->selected_day, count);

    if (count == 0) {
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%s", header);
        mvwprintw(win, 3, 2, "No events scheduled for this day.");
        mvwprintw(win, win_h - 2, 2, "Press any key to return...");
        wrefresh(win);
        wgetch(win);
        delwin(win);
        touchwin(stdscr);
        refresh();
        return 0;
    }

    int selected_idx = 0;
    int scroll_offset = 0;
    int to_delete_id = 0;
    bool done = false;
    int max_text_w = win_w - 6;
    int viewable_height = win_h - 4;

    while (!done) {
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%s", header);

        int item_start_y[16];
        int curr_y = 0;
        for (int i = 0; i < count; i++) {
            item_start_y[i] = curr_y;
            curr_y += 1 + count_desc_lines(events[i]->description) + 1;
        }

        if (item_start_y[selected_idx] < scroll_offset) {
            scroll_offset = item_start_y[selected_idx];
        }
        int sel_end_y = item_start_y[selected_idx] + 1 + count_desc_lines(events[selected_idx]->description);
        if (sel_end_y - scroll_offset > viewable_height) {
            scroll_offset = sel_end_y - viewable_height;
        }
        if (scroll_offset < 0) scroll_offset = 0;

        for (int i = 0; i < count; i++) {
            int base_screen_y = 2 + (item_start_y[i] - scroll_offset);

            if (base_screen_y >= 2 && base_screen_y < win_h - 2) {
                if (i == selected_idx) wattron(win, A_REVERSE | A_BOLD);
                mvwprintw(win, base_screen_y, 2, "#%d: %-*.*s", events[i]->id,
                          max_text_w - 6, max_text_w - 6, events[i]->title);
                if (i == selected_idx) wattroff(win, A_REVERSE | A_BOLD);
            }

            if (strlen(events[i]->description) > 0) {
                const char* p = events[i]->description;
                int desc_line_idx = 0;
                while (*p) {
                    char desc_buf[MAX_DESC_LEN];
                    const char* next_nl = strchr(p, '\n');
                    size_t seg_len = next_nl ? (size_t)(next_nl - p) : strlen(p);
                    if (seg_len >= sizeof(desc_buf)) seg_len = sizeof(desc_buf) - 1;
                    strncpy(desc_buf, p, seg_len);
                    desc_buf[seg_len] = '\0';

                    int desc_y = base_screen_y + 1 + desc_line_idx;
                    if (desc_y >= 2 && desc_y < win_h - 2) {
                        mvwprintw(win, desc_y, 4, "  %-*.*s", max_text_w - 4, max_text_w - 4, desc_buf);
                    }

                    desc_line_idx++;
                    if (!next_nl) break;
                    p = next_nl + 1;
                }
            }
        }

        mvwprintw(win, win_h - 2, 2, "UP/DN:Select D/DEL:Delete ENTER/ESC:Close");
        wrefresh(win);

        int ch = wgetch(win);
        switch (ch) {
        case KEY_UP:
            if (selected_idx > 0) selected_idx--;
            break;
        case KEY_DOWN:
            if (selected_idx < count - 1) selected_idx++;
            break;
        case 'd':
        case 'D':
        case KEY_DC:
            to_delete_id = events[selected_idx]->id;
            done = true;
            break;
        case 27: // ESC
        case 'q':
        case 'Q':
        case '\n':
        case KEY_ENTER:
            done = true;
            break;
        }
    }

    delwin(win);
    touchwin(stdscr);
    refresh();
    return to_delete_id;
}