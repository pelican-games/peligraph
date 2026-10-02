/* IL-1: a development-form client for devcli_intro_link_serve.
 *
 *   intro_link_test_client <graph document to rewrite> <replacement contents>
 *
 * stdin is the server's stdout: its first line is "port <N>". Prints the
 * snapshot's track values (compared with the release probe as text), then
 * checks the shader, the time the server's stdin set (seek 2.5, pause), and
 * that rewriting a curve reaches the client. Exit 0 when every check holds. */
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#define PELICAN_INTRO_LINK_IMPLEMENTATION
#include "pelican_intro_link.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intro_link_print_values.h"

#ifdef _WIN32
#define intro_link_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define intro_link_sleep_ms(ms) usleep((ms) * 1000)
#endif

static int fail(const char *message) {
    fprintf(stderr, "intro_link_test_client: %s\n", message);
    fflush(stdout);
    pil_disconnect();
    return 1;
}

static int copy_file(const char *from, const char *to) {
    char buffer[65536];
    size_t length;
    FILE *in = fopen(from, "rb");
    FILE *out;
    if (!in) return 0;
    length = fread(buffer, 1, sizeof(buffer), in);
    fclose(in);
    out = fopen(to, "wb");
    if (!out) return 0;
    if (fwrite(buffer, 1, length, out) != length) {
        fclose(out);
        return 0;
    }
    return fclose(out) == 0;
}

int main(int argc, char **argv) {
    char line[256];
    int port = 0, i;
    uint32_t words = 0;
    const uint32_t *spirv;
    float seek = -1.0f, before, after = 0.0f;
    int paused = 0, seen_seek = 0, seen_pause = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: intro_link_test_client <document> <replacement>\n");
        return 2;
    }
    if (!fgets(line, sizeof(line), stdin) || sscanf(line, "port %d", &port) != 1 || port <= 0) {
        fprintf(stderr, "intro_link_test_client: no `port <N>` line on stdin\n");
        return 1;
    }
    if (!pil_connect("127.0.0.1", port, 10000)) return fail("pil_connect failed");

    intro_link_print_values();

    spirv = PIL_SHADER(main_frag, &words);
    if (!spirv || words < 5 || spirv[0] != 0x07230203u) return fail("main_frag is not SPIR-V");
    if (PIL_SHADER_ERROR(main_frag)) return fail("main_frag has a compile error");
    if (PIL_SHADER_CHANGED(main_frag)) return fail("the snapshot's shader counts as a change");
    printf("check shader main_frag magic=0x%08x words>=5\n", (unsigned)spirv[0]);

    /* The stdin commands may land before or after the snapshot. */
    for (i = 0; i < 1000 && !(seen_seek && seen_pause); ++i) {
        float t = 0.0f;
        int result;
        pil_poll();
        result = pil_time(&t, &paused);
        if (result == 2) {
            seek = t;
            seen_seek = 1;
        }
        if (result > 0 && paused) seen_pause = 1;
        if (!(seen_seek && seen_pause)) intro_link_sleep_ms(10);
    }
    if (!seen_seek || !seen_pause) return fail("no paused TIME with a seek arrived");
    if (fabsf(seek - 2.5f) > 0.05f) return fail("the seek time is not 2.5");
    printf("check time paused=%d seek=%.2f\n", paused, seek);

    before = PIL_TRACK(fade_linear, 0.5f);
    if (!copy_file(argv[2], argv[1])) return fail("could not rewrite the graph document");
    for (i = 0; i < 1000; ++i) {
        pil_poll();
        after = PIL_TRACK(fade_linear, 0.5f);
        if (after != before) break;
        intro_link_sleep_ms(10);
    }
    if (after == before) return fail("the rewritten curve did not arrive within 10 s");
    if (fabsf(after - 1.5f) > 1e-5f) return fail("the rewritten curve has an unexpected value");
    printf("check reload fade_linear(0.5) %.3f -> %.3f\n", before, after);

    pil_disconnect();
    return 0;
}
