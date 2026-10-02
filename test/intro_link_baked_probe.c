/* IL-1: the release form of pelican_intro_link.h over the header that
 * `pelican_cli intro-link bake` wrote for test/fixtures/intro_link at build
 * time. No socket code is compiled in; devcli_intro_link_serve checks that
 * this executable does not import ws2_32 and that its values equal the ones
 * the development client received. */
#include "pelican_intro_link_fixture_baked.h"

#define PELICAN_INTRO_LINK_BAKED
#include "pelican_intro_link.h"

#include <stdio.h>

#include "intro_link_print_values.h"

int main(void) {
    uint32_t words = 0;
    const uint32_t *spirv;
    float t = 0.0f;
    int paused = 0;
    if (!pil_connect("127.0.0.1", 47611, 0)) return 1;
    pil_poll();
    intro_link_print_values();
    spirv = PIL_SHADER(main_frag, &words);
    printf("shader main_frag words=%u magic=0x%08x changed=%d error=%s\n", (unsigned)words,
           (unsigned)(words ? spirv[0] : 0u), PIL_SHADER_CHANGED(main_frag),
           PIL_SHADER_ERROR(main_frag) ? "yes" : "none");
    printf("time %d\n", pil_time(&t, &paused));
    pil_disconnect();
    return 0;
}
