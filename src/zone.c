// setenv, unsetenv, tzset, localtime_r and strdup are POSIX, not C99
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "zone.h"

#define ZONEINFO_DIR "/usr/share/zoneinfo/"
#define LOCALTIME_LINK "/etc/localtime"

bool zone_is_known(const char* zone) {
    if (zone[0] == '\0' || zone[0] == '/' || strstr(zone, "..") != NULL) return false;
    char path[sizeof(ZONEINFO_DIR) + MAX_ZONE_LEN];
    if (snprintf(path, sizeof(path), "%s%s", ZONEINFO_DIR, zone) >= (int)sizeof(path)) return false;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    char magic[4];
    bool ok = fread(magic, 1, sizeof(magic), file) == sizeof(magic) && memcmp(magic, "TZif", 4) == 0;
    fclose(file);
    return ok;
}

// Writes the TZ value for a zone: "UTC0" for UTC, otherwise ":<zone>" for a zone file
static void zone_tz_value(const char* zone, char* out, size_t size) {
    if (strcmp(zone, "UTC") == 0) {
        snprintf(out, size, "UTC0");
    } else {
        snprintf(out, size, ":%s", zone);
    }
}

// Points TZ at a zone. *saved receives the previous TZ (NULL when unset) and must be passed to leave_zone.
static bool enter_zone(const char* zone, char** saved) {
    *saved = NULL;
    const char* old = getenv("TZ");
    if (old != NULL) {
        *saved = strdup(old);
        if (*saved == NULL) return false;
    }
    char tz[MAX_ZONE_LEN + 2];
    zone_tz_value(zone, tz, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();
    return true;
}

static void leave_zone(char* saved) {
    if (saved != NULL) {
        setenv("TZ", saved, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    free(saved);
}

static long long days_from_civil(long long y, int m, int d);
static bool offset_state_at(time_t instant, ZoneOffset* state);

// A wall-clock time that exists once is taken at its only offset. A time in a gap (skipped by a forward change) uses the
// offset from before the change. A time that occurs twice (repeated by a backward change) uses the earlier instant.
static bool resolve_local_time(long long local, time_t* instant) {
    // Offsets just before and just after the local time, which are the only two that can apply to it
    ZoneOffset before;
    ZoneOffset after;
    if (!offset_state_at((time_t)(local - 86400), &before)) return false;
    if (!offset_state_at((time_t)(local + 86400), &after)) return false;

    bool found = false;
    time_t best = 0;
    const long offsets[2] = {before.offset, after.offset};
    for (int i = 0; i < 2; i++) {
        time_t candidate = (time_t)(local - offsets[i]);
        ZoneOffset check;
        if (!offset_state_at(candidate, &check)) return false;
        if (check.offset == offsets[i] && (!found || candidate < best)) {
            best = candidate;
            found = true;
        }
    }

    *instant = found ? best : (time_t)(local - before.offset);
    return true;
}

bool zone_to_instant(int y, int m, int d, int minute, const char* zone, time_t* instant) {
    long long local = days_from_civil(y, m, d) * 86400LL + minute * 60LL;

    if (zone[0] == '\0') return resolve_local_time(local, instant);

    char* saved;
    if (!enter_zone(zone, &saved)) return false;
    bool ok = resolve_local_time(local, instant);
    leave_zone(saved);
    return ok;
}

bool zone_wall_clock(time_t instant, const char* zone, int* y, int* m, int* d, int* minute) {
    struct tm wall;
    bool ok;
    if (zone[0] == '\0') {
        ok = localtime_r(&instant, &wall) != NULL;
    } else {
        char* saved;
        if (!enter_zone(zone, &saved)) return false;
        ok = localtime_r(&instant, &wall) != NULL;
        leave_zone(saved);
    }
    if (!ok) return false;
    *y = wall.tm_year + 1900;
    *m = wall.tm_mon + 1;
    *d = wall.tm_mday;
    *minute = wall.tm_hour * 60 + wall.tm_min;
    return true;
}

bool zone_local_name(char* out, size_t size) {
    const char* tz = getenv("TZ");
    if (tz != NULL && tz[0] == ':') tz++;
    if (tz != NULL && tz[0] != '\0' && strlen(tz) < size && zone_is_known(tz)) {
        strcpy(out, tz);
        return true;
    }

    char target[MAX_ZONE_LEN + 64];
    ssize_t length = readlink(LOCALTIME_LINK, target, sizeof(target) - 1);
    if (length <= 0) return false;
    target[length] = '\0';

    const char* marker = strstr(target, "zoneinfo/");
    if (marker == NULL) return false;
    const char* name = marker + strlen("zoneinfo/");
    if (strlen(name) >= size || !zone_is_known(name)) return false;
    strcpy(out, name);
    return true;
}

// Days from 1970-01-01 to the given civil date (proleptic Gregorian calendar)
static long long days_from_civil(long long y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Offset changes are found by sampling at this interval. The system zone data has no two changes closer than about a
// week, so hourly sampling finds every change; a change pair closer than this would be missed.
#define OFFSET_SAMPLE_SECONDS 3600

// Reads the offset state at an instant. The zone must already be entered.
static bool offset_state_at(time_t instant, ZoneOffset* state) {
    struct tm tm;
    if (localtime_r(&instant, &tm) == NULL) return false;
    long long local = days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 86400LL
                    + tm.tm_hour * 3600LL + tm.tm_min * 60LL + tm.tm_sec;
    state->instant = instant;
    state->offset = (long)(local - (long long)instant);
    state->is_dst = tm.tm_isdst > 0;
    return true;
}

static bool same_offset_state(const ZoneOffset* a, const ZoneOffset* b) {
    return a->offset == b->offset && a->is_dst == b->is_dst;
}

// Finds the first instant in (from.instant, to.instant] where the state differs from from's, by bisection
static bool find_offset_change(const ZoneOffset* from, time_t to_instant, ZoneOffset* change) {
    time_t lo = from->instant;
    time_t hi = to_instant;
    while (hi - lo > 1) {
        time_t mid = lo + (hi - lo) / 2;
        ZoneOffset probe;
        if (!offset_state_at(mid, &probe)) return false;
        if (same_offset_state(&probe, from)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return offset_state_at(hi, change);
}

ZoneOffset* zone_offset_states(const char* zone, int from_year, int to_year, size_t* count) {
    *count = 0;
    char* saved;
    if (!enter_zone(zone, &saved)) return NULL;

    time_t start = (time_t)(days_from_civil(from_year, 1, 1) * 86400LL);
    time_t end = (time_t)(days_from_civil((long long)to_year + 1, 1, 1) * 86400LL) - 1;

    size_t capacity = 64;
    size_t n = 0;
    ZoneOffset* states = malloc(capacity * sizeof(*states));
    ZoneOffset current;
    bool ok = states != NULL && offset_state_at(start, &current);
    if (ok) {
        states[n++] = current;
    }

    time_t t = start;
    while (ok && t < end) {
        time_t next = end - t > OFFSET_SAMPLE_SECONDS ? t + OFFSET_SAMPLE_SECONDS : end;
        ZoneOffset probe;
        ok = offset_state_at(next, &probe);
        if (ok && !same_offset_state(&probe, &current)) {
            ZoneOffset change;
            ok = find_offset_change(&current, next, &change);
            if (ok && n == capacity) {
                ZoneOffset* grown = realloc(states, capacity * 2 * sizeof(*states));
                if (grown == NULL) {
                    ok = false;
                } else {
                    states = grown;
                    capacity *= 2;
                }
            }
            if (ok) {
                states[n++] = change;
                current = change;
            }
        }
        t = next;
    }

    leave_zone(saved);
    if (!ok) {
        free(states);
        return NULL;
    }
    *count = n;
    return states;
}
