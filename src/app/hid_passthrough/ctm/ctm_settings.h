#ifndef CTM_SETTINGS_H
#define CTM_SETTINGS_H

/* Per-controller tunables (audio route/volume, latency, haptics, DS5 patch
 * nibbles), shared by the UI and the controller layer. Extracted from
 * tv_bridge_worker.h so neither depends on the worker (D2 stage 2 removes it).
 * Type names kept for now; a rename to ctm_settings_t lands when
 * tv_bridge_worker is deleted. */

typedef enum {
    TV_BRIDGE_KIND_HID = 0,
    TV_BRIDGE_KIND_DS4 = 4,
    TV_BRIDGE_KIND_DS5 = 5
} tv_bridge_kind_t;

typedef enum {
    /* Auto: no patching of route/volume/enable bits — let the host (game)
     * drive everything. Only the latency byte (BT 0x91 timing block) is still
     * patched. This is the default. */
    TV_BRIDGE_AUDIO_AUTO = 0,
    TV_BRIDGE_AUDIO_OFF = 1,
    TV_BRIDGE_AUDIO_SPEAKER = 2,
    TV_BRIDGE_AUDIO_HEADSET = 3,
    TV_BRIDGE_AUDIO_BOTH = 4
} tv_bridge_audio_mode_t;

typedef struct {
    tv_bridge_kind_t kind;
    tv_bridge_audio_mode_t audio_mode;
    unsigned int latency_ms;
    unsigned int haptics_gain_centi;
    unsigned int headset_volume_percent;
    unsigned int speaker_volume_percent;
    unsigned int ds5_patch_high_nibble;
    unsigned int ds5_patch_low_nibble;
    unsigned int ds5_patch2_high_nibble;
    unsigned int ds5_patch2_low_nibble;
    unsigned int ds5_trigger_reduce;  /* DS5 adaptive-trigger motor power reduction,
                                       * 0 = off (reports untouched) .. 9 = weakest */
    bool composite_passthrough;   /* Flydigi: forward full USB composite to host */
    bool block_bt_audio_sink;     /* prevent BlueZ from registering DS5 as A2DP sink */
    bool auto_plugin;             /* auto-bridge via HID passthrough on next stream start */
} tv_bridge_worker_settings_t;

#endif /* CTM_SETTINGS_H */
