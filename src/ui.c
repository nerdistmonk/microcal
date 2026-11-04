#include "ui.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define MIN_PANEL_COLS 92
#define MIN_PANEL_LINES 16
#define DAY_MIN_COLS 50
#define DAY_MIN_LINES 9
#define CAL_PANE_WIDTH 58

static bool g_detailed_view = false;

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

void ui_toggle_details(void) {
    g_detailed_view = !g_detailed_view;
}

bool ui_is_details_enabled(void) {
    return g_detailed_view;
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

void ui_draw_status_bar(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(COLOR_PAIR(4));
    mvprintw(max_y - 1, 0, "%-*s", max_x, " F1:Help ENTER:View e:Add s:Save F2:Import F3:Export f:Details t:Theme q:Quit");
    attroff(COLOR_PAIR(4));
}

static void draw_calendar_grid_window(WINDOW* win, const CalendarState* cal, int win_w, int win_h) {
    wattron(win, COLOR_PAIR(2));
    box(win, 0, 0);

    const int col_width = 8;
    const int grid_w = 7 * col_width;
    int start_col = (win_w - grid_w) / 2;
    if (start_col < 1) start_col = 1;

    mvwprintw(win, 1, start_col, "  Sun  |  Mon  |  Tue  |  Wed  |  Thu  |  Fri  |  Sat  ");
    mvwhline(win, 2, 1, ACS_HLINE, win_w - 2);

    time_t raw_time = time(NULL);
    struct tm* tm_now = localtime(&raw_time);
    int real_year = tm_now ? tm_now->tm_year + 1900 : 0;
    int real_month = tm_now ? tm_now->tm_mon + 1 : 0;
    int real_day = tm_now ? tm_now->tm_mday : 0;

    int total_days = days_in_month(cal->current_year, cal->current_month);
    int first_day_wday = get_day_of_week(cal->current_year, cal->current_month, 1);

    int row = 4;
    int col = start_col + (first_day_wday * col_width);

    for (int day = 1; day <= total_days; day++) {
        if (row >= win_h - 1) break;

        bool is_today = (cal->current_year == real_year &&
                         cal->current_month == real_month &&
                         day == real_day);
        bool is_cursor = (day == cal->selected_day);

        if (is_cursor) {
            wattron(win, COLOR_PAIR(5) | A_BOLD);
        } else if (is_today) {
            wattron(win, COLOR_PAIR(3) | A_BOLD);
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

        if (has_events) {
            mvwprintw(win, row, col, " [%02d*] ", day);
        } else {
            mvwprintw(win, row, col, "  %02d   ", day);
        }

        if (is_cursor) {
            wattroff(win, COLOR_PAIR(5) | A_BOLD);
        } else if (is_today) {
            wattroff(win, COLOR_PAIR(3) | A_BOLD);
        }

        col += col_width;
        if ((first_day_wday + day) % 7 == 0) {
            col = start_col;
            row += 2;
        }
    }
    wattroff(win, COLOR_PAIR(2));
}

static void draw_side_event_panel(WINDOW* win, const CalendarState* cal, int win_w, int win_h) {
    wattron(win, COLOR_PAIR(2));
    box(win, 0, 0);

    char title_buf[64];
    snprintf(title_buf, sizeof(title_buf), " Events for %04d-%02d-%02d %s",
             cal->current_year, cal->current_month, cal->selected_day,
             g_detailed_view ? "[Expanded]" : "");
    wattron(win, A_BOLD);
    mvwprintw(win, 1, 2, "%-*.*s", win_w - 4, win_w - 4, title_buf);
    wattroff(win, A_BOLD);
    mvwhline(win, 2, 1, ACS_HLINE, win_w - 2);

    Event* day_events[MAX_DAY_EVENTS];
    int count = calendar_get_events_for_day(cal, cal->current_year, cal->current_month, cal->selected_day, day_events, MAX_DAY_EVENTS);

    if (count == 0) {
        mvwprintw(win, 4, 3, "No events scheduled.");
        mvwprintw(win, 5, 3, "Press 'e' to add one.");
    } else {
        int cur_line = 3;
        int max_w = win_w - 4;

        for (int i = 0; i < count; i++) {
            if (cur_line >= win_h - 2) {
                mvwprintw(win, win_h - 2, 2, "[More events hidden...]");
                break;
            }

            char item_line[MAX_TITLE_LEN + 16];
            snprintf(item_line, sizeof(item_line), "- %s", day_events[i]->title);

            wattron(win, COLOR_PAIR(1) | A_BOLD);
            mvwprintw(win, cur_line++, 2, "%-*.*s", max_w, max_w, item_line);
            wattroff(win, COLOR_PAIR(1) | A_BOLD);

            if (g_detailed_view && strlen(day_events[i]->category) > 0 && cur_line < win_h - 2) {
                mvwprintw(win, cur_line++, 4, "Type: %-*.*s", max_w - 8, max_w - 8, day_events[i]->category);
            }

            if (strlen(day_events[i]->description) > 0) {
                const char* p = day_events[i]->description;
                while (*p && cur_line < win_h - 2) {
                    char desc_buf[MAX_DESC_LEN];
                    const char* next_nl = strchr(p, '\n');
                    size_t seg_len = next_nl ? (size_t)(next_nl - p) : strlen(p);
                    if (seg_len >= sizeof(desc_buf)) seg_len = sizeof(desc_buf) - 1;
                    strncpy(desc_buf, p, seg_len);
                    desc_buf[seg_len] = '\0';

                    mvwprintw(win, cur_line++, 4, "%-*.*s", max_w - 4, max_w - 4, desc_buf);

                    if (!next_nl) break;
                    p = next_nl + 1;
                }
            }

            cur_line++;
        }
    }
    wattroff(win, COLOR_PAIR(2));
}

void ui_refresh_all(const CalendarState* cal) {
    clear();
    ui_draw_header(cal);
    ui_draw_status_bar();
    refresh();

    int usable_lines = LINES - 3;
    if (usable_lines < 8) usable_lines = 8;

    if (COLS >= MIN_PANEL_COLS && LINES >= MIN_PANEL_LINES) {
        int cal_w = CAL_PANE_WIDTH;
        int panel_w = COLS - cal_w - 1;

        WINDOW* cal_win = newwin(usable_lines, cal_w, 2, 0);
        WINDOW* panel_win = newwin(usable_lines, panel_w, 2, cal_w);

        if (cal_win && panel_win) {
            draw_calendar_grid_window(cal_win, cal, cal_w, usable_lines);
            draw_side_event_panel(panel_win, cal, panel_w, usable_lines);
            wrefresh(cal_win);
            wrefresh(panel_win);
            delwin(cal_win);
            delwin(panel_win);
        }
    } else {
        WINDOW* cal_win = newwin(usable_lines, COLS, 2, 0);
        if (cal_win) {
            draw_calendar_grid_window(cal_win, cal, COLS, usable_lines);
            wrefresh(cal_win);
            delwin(cal_win);
        }
    }
}

int ui_get_day_at_coords(const CalendarState* cal, int my, int mx) {
    int total_days = days_in_month(cal->current_year, cal->current_month);
    int first_day_wday = get_day_of_week(cal->current_year, cal->current_month, 1);

    int win_w = (COLS >= MIN_PANEL_COLS && LINES >= MIN_PANEL_LINES) ? CAL_PANE_WIDTH : COLS;
    const int col_width = 8;
    const int grid_w = 7 * col_width;
    int start_col = (win_w - grid_w) / 2;
    if (start_col < 1) start_col = 1;

    int row = 6;
    int col = start_col + (first_day_wday * col_width);

    for (int day = 1; day <= total_days; day++) {
        if (my == row && mx >= col && mx < col + col_width) {
            return day;
        }

        col += col_width;
        if ((first_day_wday + day) % 7 == 0) {
            col = start_col;
            row += 2;
        }
    }
    return -1;
}

static int ui_wait_key(WINDOW* win) {
    int ch;
    do {
        ch = wgetch(win);
    } while (ch == KEY_RESIZE);
    return ch;
}

static void ui_show_too_small(void) {
    ui_show_message("Terminal too small", "Resize the terminal and try again.");
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
    keypad(win, TRUE);
    wrefresh(win);
    ui_wait_key(win);
    delwin(win);

    touchwin(stdscr);
    refresh();
}

static void ui_draw_input(WINDOW* win, int y, int x, int avail, const char* buf) {
    int len = (int)strlen(buf);
    int off = len > avail ? len - avail : 0;
    mvwprintw(win, y, x, "%-*.*s", avail, avail, buf + off);
    wmove(win, y, x + (len - off));
}

bool ui_show_prompt(const char* title, const char* prompt, char* buf, int max_len) {
    const int win_h = 7;
    const int win_w = 60;

    if (LINES < win_h || COLS < win_w) {
        ui_show_too_small();
        return false;
    }

    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;

    WINDOW* win = newwin(win_h, win_w, start_y, start_x);
    if (!win) {
        return false;
    }

    keypad(win, TRUE);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "%s", title);
    mvwprintw(win, 3, 2, "%s: ", prompt);

    int input_x = 2 + (int)strlen(prompt) + 2;
    int avail = win_w - input_x - 2;
    if (avail < 1) avail = 1;

    buf[0] = '\0';
    bool submitted = false;
    curs_set(1);

    for (;;) {
        ui_draw_input(win, 3, input_x, avail, buf);
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == ERR) break;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            submitted = true;
            break;
        }
        if (ch == 27) break;

        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            int len = (int)strlen(buf);
            if (len > 0) {
                len--;
                while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;
                buf[len] = '\0';
            }
        } else if ((ch >= 32 && ch < 127) || (ch >= 128 && ch <= 255)) {
            int len = (int)strlen(buf);
            if (len < max_len - 1) {
                buf[len] = (char)ch;
                buf[len + 1] = '\0';
            }
        }
    }

    curs_set(0);
    delwin(win);

    touchwin(stdscr);
    refresh();

    return submitted && buf[0] != '\0';
}

static int count_desc_lines(const char* desc) {
    if (!desc || strlen(desc) == 0) return 0;
    int count = 1;
    for (const char* p = desc; *p; p++) {
        if (*p == '\n') count++;
    }
    return count;
}

static bool ui_confirm_delete(const char* title) {
    const int win_h = 7;
    int win_w = COLS < 60 ? COLS : 60;

    if (LINES < win_h || win_w < 42) {
        ui_show_too_small();
        return false;
    }

    WINDOW* win = newwin(win_h, win_w, (LINES - win_h) / 2, (COLS - win_w) / 2);
    if (!win) {
        return false;
    }

    keypad(win, TRUE);
    box(win, 0, 0);
    int title_max = win_w - 13;
    if (title_max > 40) title_max = 40;
    mvwprintw(win, 1, 2, "Delete \"%.*s\"?", title_max, title);
    mvwprintw(win, 3, 2, "Press Y to delete, N or ESC to cancel.");
    wrefresh(win);

    int ch;
    do {
        ch = wgetch(win);
    } while (ch != ERR && ch != 'y' && ch != 'Y' && ch != 'n' && ch != 'N' &&
             ch != 'q' && ch != 'Q' && ch != 27);

    delwin(win);
    return ch == 'y' || ch == 'Y';
}

static bool day_window_geometry(int content_lines, int count, int* w, int* h, int* y, int* x) {
    if (COLS < DAY_MIN_COLS || LINES < DAY_MIN_LINES) return false;

    *w = COLS - 10;
    if (*w > 80) *w = 80;
    if (*w < 50) *w = 50;

    *h = (count == 0) ? 7 : (content_lines + 5);
    if (*h > LINES - 2) *h = LINES - 2;

    *y = (LINES - *h) / 2;
    *x = (COLS - *w) / 2;
    return true;
}

int ui_show_day_events(const CalendarState* cal, Event* events[], int count) {
    int total_content_lines = 0;
    for (int i = 0; i < count; i++) {
        total_content_lines += 1;
        total_content_lines += count_desc_lines(events[i]->description);
        total_content_lines += 1;
    }

    char header[64];
    snprintf(header, sizeof(header), " Events for %04d-%02d-%02d (%d) ",
             cal->current_year, cal->current_month, cal->selected_day, count);

    int win_w, win_h, win_y, win_x;
    if (!day_window_geometry(total_content_lines, count, &win_w, &win_h, &win_y, &win_x)) {
        ui_show_too_small();
        return 0;
    }

    WINDOW* win = NULL;
    int selected_idx = 0;
    int scroll_offset = 0;
    int to_delete_id = 0;
    bool done = false;
    bool too_small = false;

    while (!done) {
        int new_w, new_h, new_y, new_x;
        if (!day_window_geometry(total_content_lines, count, &new_w, &new_h, &new_y, &new_x)) {
            too_small = true;
            break;
        }
        if (!win || new_w != win_w || new_h != win_h || new_y != win_y || new_x != win_x) {
            if (win) {
                delwin(win);
                clear();
            }
            win_w = new_w;
            win_h = new_h;
            win_y = new_y;
            win_x = new_x;
            win = newwin(win_h, win_w, win_y, win_x);
            if (!win) {
                too_small = true;
                break;
            }
            keypad(win, TRUE);
        }

        int max_text_w = win_w - 6;
        int viewable_height = win_h - 4;

        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%s", header);

        if (count == 0) {
            mvwprintw(win, 3, 2, "No events scheduled for this day.");
            mvwprintw(win, win_h - 2, 2, "Press any key to return...");
            wrefresh(win);
            if (wgetch(win) != KEY_RESIZE) done = true;
            continue;
        }

        int item_start_y[MAX_DAY_EVENTS];
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
                mvwprintw(win, base_screen_y, 3, "%-*.*s", max_text_w - 3, max_text_w - 3, events[i]->title);
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
                        mvwprintw(win, desc_y, 5, "%-*.*s", max_text_w - 5, max_text_w - 5, desc_buf);
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
            if (ui_confirm_delete(events[selected_idx]->title)) {
                to_delete_id = events[selected_idx]->id;
                done = true;
            } else {
                touchwin(stdscr);
                refresh();
                touchwin(win);
            }
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

    if (win) delwin(win);
    touchwin(stdscr);
    refresh();
    if (too_small) ui_show_too_small();
    return to_delete_id;
}