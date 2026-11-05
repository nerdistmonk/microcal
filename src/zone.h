#ifndef ZONE_H
#define ZONE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Longest zone name (TZID) kept with an event
#define MAX_ZONE_LEN 128

// Zones come from the system zone database. Returns false for a name that is not a TZif file.
bool zone_is_known(const char* zone);

// Converts a wall-clock time in a zone to its instant. An empty zone means the viewer's local time.
bool zone_to_instant(int y, int m, int d, int minute, const char* zone, time_t* instant);

// Converts an instant to wall-clock time in a zone. An empty zone means the viewer's local time.
bool zone_wall_clock(time_t instant, const char* zone, int* y, int* m, int* d, int* minute);

// Names the viewer's own zone (from TZ or /etc/localtime) so it can be written as a TZID. Returns false if unnamed.
bool zone_local_name(char* out, size_t size);

#endif // ZONE_H
