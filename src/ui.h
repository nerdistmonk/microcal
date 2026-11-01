#ifndef UI_H
#define UI_H

#include <ncurses.h>
#include "calendar.h"

// Built-in Color Schemes
typedef enum {
    THEME_DEFAULT = 1,
    THEME_OCEAN,
    THEME_FOREGROUND_POP
} ColorTheme;

typedef struct {
    WINDOW* grid_win;
    WINDOW* status_win;
    WINDOW* header_win;
    ColorTheme current_theme;
} UIState;

// UI Initialization & Cleanup
void ui_init(void);
void ui_cleanup(void);

// Theme management
void ui_set_theme(ColorTheme theme);

// Rendering functions
void ui_draw_header(const CalendarState* cal);
void ui_draw_grid(const CalendarState* cal);
void ui_draw_status_bar(void);
void ui_refresh_all(const CalendarState* cal);

// Dialog / Prompt helpers
void ui_show_message(const char* title, const char* message);
bool ui_show_prompt(const char* title, const char* prompt, char* buf, int max_len);
int ui_show_day_events(const CalendarState* cal, Event* events[], int count);

#endif // UI_H