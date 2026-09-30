/*
 * Per-controller settings codec (src/app/hid_passthrough/hid_pt_pad_settings.c):
 * the ini words and ranges the pref store loads and writes, and the copy to and
 * from the bridge's settings record.
 */
#include <string.h>

#include "unity.h"
#include "hid_pt_pad_settings.h"

void setUp(void) {}

void tearDown(void) {}

static tv_bridge_worker_settings_t defaults(void)
{
    tv_bridge_worker_settings_t s;
    memset(&s, 0, sizeof(s));
    s.kind = TV_BRIDGE_KIND_DS5;
    s.audio_mode = TV_BRIDGE_AUDIO_AUTO;
    s.latency_ms = 60;
    s.haptics_gain_centi = 100;
    s.headset_volume_percent = 95;
    s.speaker_volume_percent = 95;
    return s;
}

static void test_parse_accepts_the_ranges(void)
{
    uint16_t v = 0xFFFF;
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_LATENCY, "0", &v));
    TEST_ASSERT_EQUAL_UINT16(0, v);
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_LATENCY, "200", &v));
    TEST_ASSERT_EQUAL_UINT16(200, v);
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_SPEAKER, "100", &v));
    TEST_ASSERT_EQUAL_UINT16(100, v);
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_HAPTICS, "150", &v));
    TEST_ASSERT_EQUAL_UINT16(150, v);
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_TRIGGERS, "9", &v));
    TEST_ASSERT_EQUAL_UINT16(9, v);
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_AUDIO, "headset", &v));
    TEST_ASSERT_EQUAL_UINT16(TV_BRIDGE_AUDIO_HEADSET, v);
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_COMPOSITE, "on", &v));
    TEST_ASSERT_EQUAL_UINT16(1, v);
    TEST_ASSERT_TRUE(pad_setting_parse(PAD_SETTING_COMPOSITE, "off", &v));
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

static void test_parse_refuses_out_of_range_and_junk(void)
{
    static const struct {
        pad_setting_t s;
        const char *text;
    } bad[] = {
        {PAD_SETTING_LATENCY, "201"},  {PAD_SETTING_LATENCY, "-1"},   {PAD_SETTING_LATENCY, ""},
        {PAD_SETTING_LATENCY, " 60"},  {PAD_SETTING_LATENCY, "0x3c"}, {PAD_SETTING_LATENCY, "65596"},
        {PAD_SETTING_SPEAKER, "101"},  {PAD_SETTING_HEADSET, "999"},  {PAD_SETTING_HAPTICS, "201"},
        {PAD_SETTING_TRIGGERS, "10"},  {PAD_SETTING_AUDIO, "loud"},   {PAD_SETTING_AUDIO, "2"},
        {PAD_SETTING_COMPOSITE, "true"}, {PAD_SETTING_COMPOSITE, "1"}, {PAD_SETTING_COUNT, "1"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        uint16_t v = 4242;
        TEST_ASSERT_FALSE_MESSAGE(pad_setting_parse(bad[i].s, bad[i].text, &v), bad[i].text);
        TEST_ASSERT_EQUAL_UINT16(4242, v);
    }
    uint16_t v = 0;
    TEST_ASSERT_FALSE(pad_setting_parse(PAD_SETTING_LATENCY, NULL, &v));
}

/* What the store writes, it reads back: format -> parse is the identity. */
static void test_format_round_trips(void)
{
    for (int s = 0; s < PAD_SETTING_COUNT; ++s) {
        for (uint16_t v = 0; v <= 200; ++v) {
            char buf[16];
            pad_setting_format((pad_setting_t) s, v, buf, sizeof(buf));
            uint16_t back = 0xFFFF;
            if (!buf[0]) {
                continue; /* out of range for this setting: nothing is written */
            }
            TEST_ASSERT_TRUE_MESSAGE(pad_setting_parse((pad_setting_t) s, buf, &back), buf);
            if (s == PAD_SETTING_COMPOSITE) {
                TEST_ASSERT_EQUAL_UINT16(v != 0, back);
            } else {
                TEST_ASSERT_EQUAL_UINT16(v, back);
            }
        }
    }
    char buf[16];
    pad_setting_format(PAD_SETTING_TRIGGERS, 10, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
    pad_setting_format(PAD_SETTING_AUDIO, 5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_STRING(".latency", pad_setting_suffix(PAD_SETTING_LATENCY));
    TEST_ASSERT_NULL(pad_setting_suffix(PAD_SETTING_COUNT));
}

/* Only what the page stored comes back; everything else keeps its default. */
static void test_capture_then_apply_touches_only_the_mask(void)
{
    tv_bridge_worker_settings_t user = defaults();
    user.latency_ms = 35;
    user.audio_mode = TV_BRIDGE_AUDIO_SPEAKER;
    user.speaker_volume_percent = 40;
    user.headset_volume_percent = 70;
    user.haptics_gain_centi = 180;
    user.ds5_trigger_reduce = 4;

    pad_settings_t p;
    memset(&p, 0, sizeof(p));
    tv_bridge_worker_settings_t fresh = defaults();
    TEST_ASSERT_FALSE(pad_settings_apply(&p, &fresh));

    pad_settings_capture(&p, &user, PAD_SETTINGS_AUDIO);
    TEST_ASSERT_TRUE(pad_settings_apply(&p, &fresh));
    TEST_ASSERT_EQUAL_UINT(35, fresh.latency_ms);
    TEST_ASSERT_EQUAL_INT(TV_BRIDGE_AUDIO_SPEAKER, fresh.audio_mode);
    TEST_ASSERT_EQUAL_UINT(40, fresh.speaker_volume_percent);
    TEST_ASSERT_EQUAL_UINT(70, fresh.headset_volume_percent);
    TEST_ASSERT_EQUAL_UINT(100, fresh.haptics_gain_centi); /* not in the mask */
    TEST_ASSERT_EQUAL_UINT(0, fresh.ds5_trigger_reduce);

    pad_settings_capture(&p, &user, PAD_SETTINGS_DS5);
    TEST_ASSERT_TRUE(pad_settings_apply(&p, &fresh));
    TEST_ASSERT_EQUAL_UINT(180, fresh.haptics_gain_centi);
    TEST_ASSERT_EQUAL_UINT(4, fresh.ds5_trigger_reduce);
    TEST_ASSERT_FALSE(fresh.composite_passthrough);

    user.composite_passthrough = true;
    pad_settings_capture(&p, &user, PAD_SETTING_BIT(PAD_SETTING_COMPOSITE));
    TEST_ASSERT_TRUE(pad_settings_apply(&p, &fresh));
    TEST_ASSERT_TRUE(fresh.composite_passthrough);
    /* The lightbar and the mode are not this codec's. */
    TEST_ASSERT_FALSE(fresh.lightbar_user);
    TEST_ASSERT_FALSE(fresh.auto_plugin);
}

/* A record holding something the ini cannot say is stored clamped, never as a
 * value the next load would throw away. */
static void test_capture_clamps(void)
{
    tv_bridge_worker_settings_t user = defaults();
    user.latency_ms = 5000;
    user.speaker_volume_percent = 300;
    user.audio_mode = (tv_bridge_audio_mode_t) 17;
    pad_settings_t p;
    memset(&p, 0, sizeof(p));
    pad_settings_capture(&p, &user, PAD_SETTINGS_AUDIO);
    TEST_ASSERT_EQUAL_UINT16(PAD_SETTING_LATENCY_MAX, p.value[PAD_SETTING_LATENCY]);
    TEST_ASSERT_EQUAL_UINT16(PAD_SETTING_VOLUME_MAX, p.value[PAD_SETTING_SPEAKER]);
    TEST_ASSERT_EQUAL_UINT16(TV_BRIDGE_AUDIO_AUTO, p.value[PAD_SETTING_AUDIO]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_accepts_the_ranges);
    RUN_TEST(test_parse_refuses_out_of_range_and_junk);
    RUN_TEST(test_format_round_trips);
    RUN_TEST(test_capture_then_apply_touches_only_the_mask);
    RUN_TEST(test_capture_clamps);
    return UNITY_END();
}
