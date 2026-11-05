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
// Like ui_show_prompt, but Enter on an empty field counts as submitted (returns true with buf == "")
bool ui_show_prompt_optional(const char* title, const char* prompt, char* buf, int max_len);
// Left/Right (or Up/Down) cycle through options, Enter returns the index, Esc or ERR returns -1
int ui_show_choice(const char* title, const char* const options[], int count, int selected);
// Up/Down move, Space toggles, Enter returns true, Esc returns false
bool ui_show_checkboxes(const char* title, const char* const labels[], bool checked[], int count);
int ui_show_day_events(const CalendarState* cal, const EventOccurrence events[], int count);

// Mouse input translation
int ui_get_day_at_coords(const CalendarState* cal, int y, int x);

#endif // UI_H