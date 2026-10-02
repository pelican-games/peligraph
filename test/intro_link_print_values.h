/* IL-1: the fixture's three tracks at fixed times, one "value ..." line each.
 * The development client and the release probe both print through this, and
 * devcli_intro_link_serve compares the two outputs as text. Include after
 * pelican_intro_link.h (either form) and <stdio.h>. */
#ifndef PELICAN_TEST_INTRO_LINK_PRINT_VALUES_H
#define PELICAN_TEST_INTRO_LINK_PRINT_VALUES_H

static void intro_link_print_values(void) {
    static const float times[] = {-1.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f,
                                  2.0f,  2.5f, 2.9f,  3.0f, 4.0f};
    size_t i;
    for (i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        const float t = times[i];
        printf("value fade_linear %.3f %.9g\n", t, PIL_TRACK(fade_linear, t));
        printf("value flash_step %.3f %.9g\n", t, PIL_TRACK(flash_step, t));
        printf("value wobble_cubic %.3f %.9g\n", t, PIL_TRACK(wobble_cubic, t));
    }
}

#endif
