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

bool zone_to_instant(int y, int m, int d, int minute, const char* zone, time_t* instant) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = minute / 60;
    tm.tm_min = minute % 60;
    tm.tm_isdst = -1;

    time_t result;
    if (zone[0] == '\0') {
        result = mktime(&tm);
    } else {
        char* saved;
        if (!enter_zone(zone, &saved)) return false;
        result = mktime(&tm);
        leave_zone(saved);
    }

    if (result == (time_t)-1) return false;
    *instant = result;
    return true;
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
