/* The serial the panel advertises.
 *
 * The door's own indoor units identify themselves by MAC with no separators
 * ("fa72a237a18d"). A blank serial is a plausible reason for a door to accept
 * a call and then refuse to play audio to the caller, so the panel derives one
 * rather than shipping a blank.
 *
 * These tests run on whatever machine builds the tree, so they assert the
 * shape and the guarantees — never a specific address. */
#include "test.h"

#include "syncn/util.h"

#include <stdbool.h>
#include <string.h>

static bool is_lower_hex(const char *s, size_t len)
{
    if (strlen(s) != len)
        return false;
    for (size_t i = 0; i < len; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

void suite_identity(void)
{
    SUITE("identity");

    char mac[32];
    memset(mac, 0x7f, sizeof mac);
    const bool have = syncn_local_mac_hex(mac, sizeof mac);

    if (have) {
        /* Exactly the door's format: twelve lowercase hex digits, no colons.
         * A serial with separators would not match what the door has seen
         * from its own units. */
        CHECK(is_lower_hex(mac, 12));
        CHECK(strchr(mac, ':') == NULL);
        CHECK(strcmp(mac, "000000000000") != 0);

        /* Stable across calls: the door will see the same identity on every
         * discovery reply, not a new panel each time. */
        char again[32];
        CHECK(syncn_local_mac_hex(again, sizeof again));
        CHECK(strcmp(mac, again) == 0);
    } else {
        /* A container with no non-loopback interface is a legitimate outcome;
         * the daemon logs a warning and advertises the configured serial. */
        printf("    note: no non-loopback interface here, skipping format checks\n");
        CHECK(true);
    }

    /* Too small to hold twelve digits and a terminator must fail rather than
     * truncate: half a serial is a different identity, not a shorter one. */
    char tiny[12];
    memset(tiny, 'x', sizeof tiny);
    CHECK(!syncn_local_mac_hex(tiny, sizeof tiny));
    CHECK(tiny[0] == 'x');

    /* And the smallest buffer that can hold one is accepted whenever the
     * larger buffer was. */
    char exact[13];
    CHECK(syncn_local_mac_hex(exact, sizeof exact) == have);
    if (have)
        CHECK(strcmp(exact, mac) == 0);
}
