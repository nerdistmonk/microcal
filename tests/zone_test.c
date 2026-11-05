#include <stdio.h>
#include <stdlib.h>

#include "zone.h"

static int failures = 0;

static void check_instant(const char* label, int y, int m, int d, int minute, const char* zone, long long expected) {
    time_t instant = 0;
    if (!zone_to_instant(y, m, d, minute, zone, &instant)) {
        printf("FAIL %s: zone_to_instant returned false\n", label);
        failures++;
        return;
    }
    if ((long long)instant != expected) {
        printf("FAIL %s: got %lld, expected %lld\n", label, (long long)instant, expected);
        failures++;
    } else {
        printf("ok   %s\n", label);
    }
}

static void check_states(void) {
    size_t count = 0;
    ZoneOffset* states = zone_offset_states("America/New_York", 2026, 2026, &count);
    if (states == NULL) {
        printf("FAIL states: zone_offset_states returned NULL\n");
        failures++;
        return;
    }
    if (count != 3 || states[0].offset != -5 * 3600 || states[1].offset != -4 * 3600 || states[2].offset != -5 * 3600) {
        printf("FAIL states: got %zu states\n", count);
        failures++;
    } else {
        printf("ok   states NY 2026\n");
    }
    free(states);
}

int main(void) {
    // Gap: 02:30 does not exist on 2026-03-08 in New York, so it resolves with the pre-change offset (EST) to 03:30 EDT
    check_instant("NY gap 2026-03-08 02:30", 2026, 3, 8, 2 * 60 + 30, "America/New_York", 1772955000LL);
    // Overlap: 01:30 occurs twice on 2026-11-01 in New York, so the earlier occurrence (EDT) is used
    check_instant("NY overlap 2026-11-01 01:30", 2026, 11, 1, 1 * 60 + 30, "America/New_York", 1793511000LL);
    // Gap in London: 01:30 does not exist on 2026-03-29, so it resolves with GMT to 02:30 BST
    check_instant("London gap 2026-03-29 01:30", 2026, 3, 29, 1 * 60 + 30, "Europe/London", 1774747800LL);
    // Ordinary time in a half-hour zone
    check_instant("Kolkata 2026-06-01 09:00", 2026, 6, 1, 9 * 60, "Asia/Kolkata", 1780284600LL);
    check_states();

    if (failures == 0) {
        printf("All zone tests passed\n");
        return EXIT_SUCCESS;
    }
    printf("%d zone test(s) failed\n", failures);
    return EXIT_FAILURE;
}
