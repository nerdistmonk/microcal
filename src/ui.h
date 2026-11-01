#ifndef UI_H
#define UI_H

#include <ncurses.h>
#include <stdbool.h>
#include "calendar.h"

// Built-in Color Schemes
typedef enum {
    THEME_DEFAULT = 1,
    THEME_OCEAN,
    THEME_FOREGROUND_POP
} ColorTheme;

// UI Initialization & Cleanup
void ui_init(void);
void ui_cleanup(void);

// Theme & View state management
void ui_set_theme(ColorTheme theme);
void ui_toggle_details(void);
bool ui_is_details_enabled(void);

// Rendering functions
void ui_draw_header(const CalendarState* cal);
void ui_draw_status_bar(void);
void ui_refresh_all(const CalendarState* cal);

// Dialog / Prompt helpers
void ui_show_message(const char* title, const char* message);
bool ui_show_prompt(const char* title, const char* prompt, char* buf, int max_len);
int ui_show_day_events(const CalendarState* cal, Event* events[], int count);

// Mouse input translation
int ui_get_day_at_coords(const CalendarState* cal, int y, int x);

#endif // UI_H