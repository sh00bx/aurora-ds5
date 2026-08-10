/* Golden test for src/app/hid_passthrough/ctm/ctm_pad_table.h.
 *
 * The table replaced six hand-written predicates and a vendor allow-list. This
 * pins the replacement: the reference implementations below are the predicates
 * as they stood in ui_devices.c immediately before the table landed (commit
 * 6c88b3c7), copied verbatim, and the test asserts that the table answers
 * identically for every id those predicates mention plus a spread of negatives.
 *
 * Host build, no dependencies -- the header is deliberately self-contained:
 *
 *     gcc -Wall -Wextra -Werror -o /tmp/ctm_pad_table_test \
 *         tools/ctm_pad_table_test.c && /tmp/ctm_pad_table_test
 *
 * It is NOT wired into CMake: the app's own test tree is disabled for
 * TARGET_WEBOS builds (see BUILD_TESTS in the top-level CMakeLists.txt), which
 * is the only configuration this subsystem compiles in, so a CMake target here
 * would never run. Run it by hand when you touch the table.
 *
 * Exit status: 0 only when every check ran and every check passed. Any
 * mismatch, and an empty corpus or an empty table, is a non-zero exit.
 */

#include "../src/app/hid_passthrough/ctm/ctm_pad_table.h"

#include <stdio.h>
#include <string.h>

/* ---- reference: the predicates as they were before the table -------------- */

static bool ref_contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) {
        return false;
    }
    size_t nl = strlen(needle);
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

static bool ref_is_ds5(const char *vid, const char *pid)
{
    return strcmp(vid, "054c") == 0 && strcmp(pid, "0ce6") == 0;
}

static bool ref_is_ds4(const char *vid, const char *pid)
{
    return strcmp(vid, "054c") == 0 &&
           (strcmp(pid, "09cc") == 0 || strcmp(pid, "05c4") == 0);
}

static bool ref_is_xbox_pid(const char *pid)
{
    return pid &&
           (strcmp(pid, "02d1") == 0 || strcmp(pid, "02dd") == 0 ||
            strcmp(pid, "02e0") == 0 || strcmp(pid, "02e3") == 0 ||
            strcmp(pid, "02ea") == 0 ||
            strcmp(pid, "02fd") == 0 || strcmp(pid, "0b00") == 0 ||
            strcmp(pid, "0b05") == 0 || strcmp(pid, "0b0a") == 0 ||
            strcmp(pid, "0b12") == 0 || strcmp(pid, "0b13") == 0 ||
            strcmp(pid, "0b20") == 0);
}

static bool ref_is_xbox_device(const char *vid, const char *pid, const char *name)
{
    return strcmp(vid, "045e") == 0 && (ref_is_xbox_pid(pid) || ref_contains_ci(name, "xbox"));
}

static bool ref_is_xpad_compatible_pid(const char *vid, const char *pid)
{
    if (!vid || !pid) return false;
    return strcmp(vid, "045e") == 0 &&
           (strcmp(pid, "028e") == 0 || strcmp(pid, "028f") == 0 ||
            strcmp(pid, "0291") == 0 || strcmp(pid, "0719") == 0 ||
            ref_is_xbox_pid(pid));
}

static bool ref_is_steam_puck(const char *vid, const char *pid)
{
    return strcmp(vid, "28de") == 0 && strcmp(pid, "1304") == 0;
}

/* The one VID:PID term of the old Flydigi predicates; the other terms were
 * sysfs/name heuristics and are still where they were. */
static bool ref_is_flydigi_vidpid(const char *vid, const char *pid)
{
    return strcmp(vid, "04b4") == 0 && strcmp(pid, "2412") == 0;
}

/* gamepad_iface_candidate()'s allow-list, verbatim. */
static bool ref_vendor_allowed(uint16_t vendor_id)
{
    switch (vendor_id) {
        case 0x045e: case 0x054c: case 0x057e: case 0x04b4: case 0x3537:
        case 0x2dc8: case 0x0e6f: case 0x1532: case 0x0738: case 0x046d:
        case 0x20d6: case 0x24c6: case 0x2563: case 0x28de: case 0x0079:
        case 0x0810: case 0x1038: case 0x146b: case 0x1949: case 0x1bad:
        case 0x2378:
            return true;
        default:
            return false;
    }
}

/* logical_name_for_device()'s VID:PID-decided names, in its own order. Returns
 * NULL where the old code fell through to a name heuristic. */
static const char *ref_display_name(const char *vid, const char *pid, bool has_hidraw)
{
    if (ref_is_steam_puck(vid, pid)) return "Valve Software Steam Controller Puck";
    if (ref_is_flydigi_vidpid(vid, pid)) return NULL; /* flydigi heuristics decide */
    if (ref_is_ds5(vid, pid)) return "Sony DS5 Controller";
    if (ref_is_ds4(vid, pid)) return "Sony DS4 Controller";
    if (ref_is_xbox_device(vid, pid, "")) return "Microsoft Xbox Controller";
    if (!has_hidraw && ref_is_xpad_compatible_pid(vid, pid)) return "XInput-compatible Gamepad";
    return NULL;
}

/* ---- harness -------------------------------------------------------------- */

static int g_checks;
static int g_failures;

static void check(bool ok, const char *what, const char *vid, const char *pid)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL %s for %s:%s\n", what, vid, pid);
    }
}

static const char *const g_corpus[][2] = {
    /* Sony */
    {"054c", "0ce6"}, {"054c", "09cc"}, {"054c", "05c4"},
    {"054c", "0000"}, {"054c", "0df2"}, {"054c", "ffff"},
    /* Microsoft: every listed product, plus unlisted ones */
    {"045e", "02d1"}, {"045e", "02dd"}, {"045e", "02e0"}, {"045e", "02e3"},
    {"045e", "02ea"}, {"045e", "02fd"}, {"045e", "0b00"}, {"045e", "0b05"},
    {"045e", "0b0a"}, {"045e", "0b12"}, {"045e", "0b13"}, {"045e", "0b20"},
    {"045e", "028e"}, {"045e", "028f"}, {"045e", "0291"}, {"045e", "0719"},
    {"045e", "0000"}, {"045e", "028d"}, {"045e", "0b21"}, {"045e", "ffff"},
    /* Valve, Cypress/Flydigi */
    {"28de", "1304"}, {"28de", "1102"}, {"04b4", "2412"}, {"04b4", "0001"},
    /* Allow-list vendors with an unmodelled product */
    {"057e", "2009"}, {"3537", "1001"}, {"2dc8", "3106"}, {"0e6f", "0139"},
    {"1532", "0a00"}, {"0738", "4716"}, {"046d", "c21d"}, {"20d6", "a711"},
    {"24c6", "541a"}, {"2563", "0575"}, {"0079", "0006"}, {"0810", "0001"},
    {"1038", "1420"}, {"146b", "0601"}, {"1949", "0402"}, {"1bad", "f016"},
    {"2378", "1008"},
    /* Vendors that are not gamepad vendors, and junk */
    {"005d", "0001"}, {"1234", "5678"}, {"0000", "0000"}, {"", ""},
};

#define CORPUS_COUNT ((int) (sizeof(g_corpus) / sizeof(g_corpus[0])))

static void check_table_shape(void)
{
    for (int i = 0; i < CTM_PAD_TABLE_COUNT; ++i) {
        const ctm_pad_desc_t *row = &ctm_pad_table[i];
        g_checks++;
        if (row->vid == 0) {
            g_failures++;
            fprintf(stderr, "FAIL row %d has vendor 0, which no lookup can reach\n", i);
        }
        g_checks++;
        if (row->pid == CTM_PAD_ANY_PID) {
            if (row->kind != CTM_PAD_KIND_UNKNOWN || row->display_name || row->flags) {
                g_failures++;
                fprintf(stderr, "FAIL wildcard row %d carries a kind, name or flags\n", i);
            }
        } else if (!row->display_name) {
            g_failures++;
            fprintf(stderr, "FAIL row %d (%04x:%04x) has no display name\n",
                    i, row->vid, row->pid);
        }
        for (int j = i + 1; j < CTM_PAD_TABLE_COUNT; ++j) {
            g_checks++;
            if (ctm_pad_table[j].vid == row->vid && ctm_pad_table[j].pid == row->pid) {
                g_failures++;
                fprintf(stderr, "FAIL rows %d and %d both claim %04x:%04x\n",
                        i, j, row->vid, row->pid);
            }
        }
        /* An exact row must precede its vendor's wildcard row, or the wildcard
         * would shadow it. */
        if (row->pid == CTM_PAD_ANY_PID) {
            for (int j = i + 1; j < CTM_PAD_TABLE_COUNT; ++j) {
                g_checks++;
                if (ctm_pad_table[j].vid == row->vid) {
                    g_failures++;
                    fprintf(stderr, "FAIL row %d (%04x:%04x) sits behind its vendor wildcard\n",
                            j, ctm_pad_table[j].vid, ctm_pad_table[j].pid);
                }
            }
        }
    }
}

static bool table_is_xbox_device(const char *vid, const char *pid, const char *name)
{
    if (ctm_pad_kind_str(vid, pid) == CTM_PAD_KIND_XBOX) return true;
    return ctm_pad_hex16(vid) == CTM_PAD_VENDOR_MICROSOFT && ref_contains_ci(name, "xbox");
}

static const char *table_display_name(const char *vid, const char *pid, bool has_hidraw)
{
    const ctm_pad_desc_t *pad = ctm_pad_desc_find_str(vid, pid);
    ctm_pad_kind_t kind = pad ? pad->kind : CTM_PAD_KIND_UNKNOWN;
    if (kind == CTM_PAD_KIND_PUCK) {
        return ctm_pad_display_name(pad, "Steam Controller Puck");
    }
    if (kind == CTM_PAD_KIND_FLYDIGI) {
        return NULL;
    }
    if (kind == CTM_PAD_KIND_DS5 || kind == CTM_PAD_KIND_DS4) {
        return ctm_pad_display_name(pad, "Sony Controller");
    }
    if (table_is_xbox_device(vid, pid, "")) {
        return ctm_pad_display_name((pad && pad->kind == CTM_PAD_KIND_XBOX) ? pad : NULL,
                                    "Microsoft Xbox Controller");
    }
    if (!has_hidraw && ctm_pad_has_flag_str(vid, pid, CTM_PAD_XPAD_COMPATIBLE)) {
        return ctm_pad_display_name(pad, "XInput-compatible Gamepad");
    }
    return NULL;
}

static bool names_equal(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

int main(void)
{
    if (CTM_PAD_TABLE_COUNT <= 0 || CORPUS_COUNT <= 0) {
        fprintf(stderr, "FAIL nothing to check: %d rows, %d corpus entries\n",
                CTM_PAD_TABLE_COUNT, CORPUS_COUNT);
        return 2;
    }

    check_table_shape();

    for (int i = 0; i < CORPUS_COUNT; ++i) {
        const char *vid = g_corpus[i][0];
        const char *pid = g_corpus[i][1];

        check(ref_is_ds5(vid, pid) == (ctm_pad_kind_str(vid, pid) == CTM_PAD_KIND_DS5),
              "is_ds5_device", vid, pid);
        check(ref_is_ds4(vid, pid) == (ctm_pad_kind_str(vid, pid) == CTM_PAD_KIND_DS4),
              "is_ds4_device", vid, pid);
        check(ref_is_steam_puck(vid, pid) == (ctm_pad_kind_str(vid, pid) == CTM_PAD_KIND_PUCK),
              "is_steam_puck_device", vid, pid);
        check(ref_is_flydigi_vidpid(vid, pid) ==
                  ctm_pad_has_flag_str(vid, pid, CTM_PAD_COMPOSITE),
              "flydigi composite id", vid, pid);
        check(ref_is_xbox_pid(pid) == ctm_pad_pid_is_xbox(pid), "is_xbox_pid", vid, pid);
        check(ref_is_xpad_compatible_pid(vid, pid) ==
                  ctm_pad_has_flag_str(vid, pid, CTM_PAD_XPAD_COMPATIBLE),
              "is_xpad_compatible_pid", vid, pid);
        check(ref_is_xbox_device(vid, pid, "") == table_is_xbox_device(vid, pid, ""),
              "is_xbox_device (no name)", vid, pid);
        check(ref_is_xbox_device(vid, pid, "Xbox Wireless Controller") ==
                  table_is_xbox_device(vid, pid, "Xbox Wireless Controller"),
              "is_xbox_device (xbox in name)", vid, pid);
        check(ref_is_xbox_device(vid, pid, "Generic Pad") ==
                  table_is_xbox_device(vid, pid, "Generic Pad"),
              "is_xbox_device (other name)", vid, pid);
        check(ref_vendor_allowed(ctm_pad_hex16(vid)) ==
                  ctm_pad_vendor_makes_gamepads(ctm_pad_hex16(vid)),
              "gamepad vendor allow-list", vid, pid);
        check(names_equal(ref_display_name(vid, pid, false),
                          table_display_name(vid, pid, false)),
              "display name (no hidraw)", vid, pid);
        check(names_equal(ref_display_name(vid, pid, true),
                          table_display_name(vid, pid, true)),
              "display name (hidraw)", vid, pid);

        /* Settings the table now decides that the old code hard-coded in
         * default_settings_for_item(). */
        check(ref_is_ds5(vid, pid) ==
                  ctm_pad_has_flag_str(vid, pid, CTM_PAD_BLOCK_BT_AUDIO_SINK),
              "block_bt_audio_sink is DS5-only", vid, pid);
        check(ref_is_ds4(vid, pid) == ctm_pad_has_flag_str(vid, pid, CTM_PAD_NO_HAPTICS),
              "haptics off is DS4-only", vid, pid);
    }

    if (g_failures) {
        fprintf(stderr, "%d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("%d checks passed\n", g_checks);
    return 0;
}
