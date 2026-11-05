#ifndef IO_H
#define IO_H

#include <stddef.h>
#include "calendar.h"

// Imports events from a .ics / .ical file into the calendar state
// Returns true on success, false on failure.
bool calendar_import_ics(CalendarState* cal, const char* filepath);

// Lists the events in a file whose rules use parts that import drops or changes. Fills report with one line per
// event (up to a few, then a count of the rest). Returns the number of affected events, or -1 if the file cannot be read.
int calendar_check_ics_rules(const char* filepath, char* report, size_t report_size);

// Exports all events from the calendar state to a .ics / .ical file.
// Automatically appends .ics if missing.
// Returns true on success, false on failure.
bool calendar_export_ics(const CalendarState* cal, const char* filepath);

// Config helpers to remember and autoload the last active file
void calendar_save_last_filepath(const char* filepath);
bool calendar_get_last_filepath(char* out_path, size_t max_len);

#endif // IO_H