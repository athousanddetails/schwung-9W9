/*
 * er99_plugin.c — Schwung plugin_api_v2 wrapper for the ER-99 engine.
 *
 * Runs in-process inside the shim's SPI callback (SCHED_FIFO 90). render_block
 * therefore does no allocation, no file I/O and takes no locks — all of that
 * happens in create_instance.
 *
 * GPL-3.0.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "er99_engine.h"
#include "er99_params.h"
#include "plugin_api_v1.h"   /* defines both v1 and v2 */

static const host_api_v1_t *g_host = NULL;

typedef struct {
    er99_engine_t engine;
    float last_bpm;
    char          module_dir[512];
    /* Pad-follow state. Deliberately NOT updated while the transport runs —
     * otherwise every sequenced note yanks the editor to a different drum and
     * you can never keep a page open while a pattern plays. */
    int           focus_voice;
    unsigned      focus_count;

    /* Silent-select support: while samples_rendered < mute_until, incoming
     * note triggers are swallowed. The editor sets this just before
     * re-injecting a Shift+Pad press into Move, so Move updates its pad
     * selection (white pad) while 9W9 stays quiet. */
    uint64_t      samples_rendered;
    uint64_t      mute_until;
    int           mute_one;       /* swallow only the FIRST note in the window */

    /* Per-lane performance mutes (Mute+Pad in the editor). Bit n = lane n. */
    uint16_t      lane_mutes;
} er99_instance_t;

/* ---- MIDI note map -------------------------------------------------- */
/*
 * Move's 32 pads are notes 68..99, laid out as 4 rows of 8, bottom-left first:
 *
 *   row 3 (top)    92 93 94 95 | 96 97 98 99
 *   row 2          84 85 86 87 | 88 89 90 91
 *   row 1          76 77 78 79 | 80 81 82 83
 *   row 0 (bottom) 68 69 70 71 | 72 73 74 75
 *                  ^---------^
 *                  drum block
 *
 * The kit lives in the LEFT 4x4 block so it reads like a normal drum machine
 * instead of being smeared across the whole grid. Order follows the 909 front
 * panel, bottom-left to top-right.
 */
static int pad_to_trigger(const uint8_t _note)
{
    switch(_note)
    {
    /* row 0 */
    case 68: return ER99_BD;   case 69: return ER99_SD;
    case 70: return ER99_LT;   case 71: return ER99_MT;
    /* row 1 */
    case 76: return ER99_HT;   case 77: return ER99_RS;
    case 78: return ER99_HC;   case 79: return ER99_CHH;
    /* row 2 */
    case 84: return ER99_OHH;  case 85: return ER99_CR;
    case 86: return ER99_RC;
    default: return -1;
    }
}

/*
 * Drum-rack map. On a Move DRUM track the pads do not send the raw pad notes
 * (68..99) — they send the drum rack's own notes starting at 36 (C1), laid out
 * bottom-left first, 4 per row:
 *
 *   48 49 50 51
 *   44 45 46 47
 *   40 41 42 43
 *   36 37 38 39   <- bottom row
 *
 * Same kit order as the raw-pad map, so the physical layout is identical
 * whichever way the notes arrive. This is the default because it is what makes
 * each drum a separately sequencable lane on a Move drum track.
 */
static int drumrack_to_trigger(const uint8_t _note)
{
    switch(_note)
    {
    case 36: return ER99_BD;   case 37: return ER99_SD;
    case 38: return ER99_LT;   case 39: return ER99_MT;
    case 40: return ER99_HT;   case 41: return ER99_RS;
    case 42: return ER99_HC;   case 43: return ER99_CHH;
    case 44: return ER99_OHH;  case 45: return ER99_CR;
    case 46: return ER99_RC;
    default: return -1;
    }
}

/* note_map: 0 = drum rack (default), 1 = General MIDI */
static int g_note_map = 0;

static int note_to_trigger(const uint8_t _note)
{
    const int pad = pad_to_trigger(_note);
    if(pad >= 0) return pad;

    if(g_note_map == 0)
        return drumrack_to_trigger(_note);

    switch(_note)
    {
    /* GM drum map */
    case 36: return ER99_BD;   case 35: return ER99_BD;
    case 38: return ER99_SD;   case 40: return ER99_SD;
    case 41: return ER99_LT;   case 45: return ER99_LT;
    case 47: return ER99_MT;   case 48: return ER99_MT;
    case 50: return ER99_HT;   case 43: return ER99_HT;
    case 37: return ER99_RS;
    case 39: return ER99_HC;
    case 42: return ER99_CHH;
    case 46: return ER99_OHH;
    case 51: return ER99_RC;
    case 49: return ER99_CR;
    default: break;
    }
    return -1;
}

/* ---- plugin_api_v2 -------------------------------------------------- */

static void *create_instance(const char *_module_dir, const char *_json_defaults)
{
    (void)_json_defaults;
    er99_instance_t *inst = (er99_instance_t*)calloc(1, sizeof(er99_instance_t));
    if(!inst) return NULL;

    if(_module_dir)
        snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", _module_dir);

    const float sr = g_host && g_host->sample_rate > 0
                   ? (float)g_host->sample_rate : 44100.0f;

    er99_engine_init(&inst->engine, sr, _module_dir);

    if(g_host && g_host->log)
        g_host->log("er99: engine ready");
    return inst;
}

static void destroy_instance(void *_instance)
{
    er99_instance_t *inst = (er99_instance_t*)_instance;
    if(!inst) return;
    er99_engine_free(&inst->engine);
    free(inst);
}

static void on_midi(void *_instance, const uint8_t *_msg, const int _len, const int _source)
{
    er99_instance_t *inst = (er99_instance_t*)_instance;
    if(!inst || _len < 3) return;

    const uint8_t status = _msg[0] & 0xF0u;
    const uint8_t note   = _msg[1];
    const uint8_t vel    = _msg[2];

    /* Drums are one-shots: note-off is ignored, note-on with velocity 0 too. */
    if(status != 0x90 || vel == 0) return;

    const int t = note_to_trigger(note);
    if(t < 0) return;

    /* Performance mute: lane is muted, hit never sounds. */
    if(inst->lane_mutes & (1u << t)) return;

    /* Silent-select window: swallow (only) the first note that arrives —
     * that's the Shift+Pad press routed back through Move. */
    if(inst->samples_rendered < inst->mute_until)
    {
        if(inst->mute_one) { inst->mute_one = 0; return; }
    }

    er99_engine_trigger(&inst->engine, (er99_trigger_t)t, (int)vel);

    /* Only a hand-played hit moves the editor. While Move's sequencer is
     * running, notes arrive constantly and following them makes the UI
     * unusable — so focus is frozen for the duration of playback. */
    const int clock = (g_host && g_host->get_clock_status)
                    ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_STOPPED;
    if(clock != MOVE_CLOCK_STATUS_RUNNING)
    {
        inst->focus_voice = t;
        inst->focus_count++;
    }
}

static void set_param(void *_instance, const char *_key, const char *_val)
{
    er99_instance_t *inst = (er99_instance_t*)_instance;
    if(!inst || !_key || !_val) return;

    if(!strcmp(_key, "note_map"))
    {
        g_note_map = atoi(_val) != 0;
        return;
    }
    if(!strcmp(_key, "mute_ms"))
    {
        const float sr = (float)inst->engine.sample_rate;
        const int ms = atoi(_val);
        inst->mute_until = inst->samples_rendered
                         + (uint64_t)((ms > 0 ? ms : 0) * 0.001f * sr);
        inst->mute_one = 1;
        return;
    }
    if(!strcmp(_key, "mutes"))
    {
        inst->lane_mutes = (uint16_t)(atoi(_val) & 0x7FF);
        return;
    }
    /* Slot autosave / User Presets restore through this key. */
    if(!strcmp(_key, "state"))
    {
        er99_engine_set_state(&inst->engine, _val);
        /* Plugin-level keys piggyback on the same blob. */
        {
            const char *mq = strstr(_val, "mutes=");
            if(mq) set_param(_instance, "mutes", mq + 6);
        }
        return;
    }
    er99_engine_set_param(&inst->engine, _key, (float)atof(_val));
}

static int get_param(void *_instance, const char *_key, char *_buf, const int _len)
{
    er99_instance_t *inst = (er99_instance_t*)_instance;
    if(!inst || !_key || !_buf || _len <= 0) return -1;

    /* Served dynamically: module.json is capped at 8 KB by the loader and our
     * parameter surface is much larger than that. */
    if(!strcmp(_key, "chain_params"))
    {
        if(_len <= ER99_CHAIN_PARAMS_LEN) return -1;   /* refuse to truncate */
        memcpy(_buf, er99_chain_params_json, ER99_CHAIN_PARAMS_LEN + 1);
        return ER99_CHAIN_PARAMS_LEN;
    }
    /* ui_hierarchy is deliberately served EMPTY, not refused. The Shadow UI's
     * enterComponentEdit uses the hierarchy editor whenever a module offers
     * one, and only falls back to loading the module's own ui_chain.js when it
     * doesn't. 9W9 ships ui_chain.js — the RD-9-style pad-select editor — so
     * the hierarchy must stay absent... but "absent" has a spelling. The host's
     * component load gate (component_load_gate.mjs) reads this key with THREE
     * answers: JSON = declared, "" = served and empty (fall back at once),
     * error/null = the read did not complete (hold and ask again). Returning
     * -1 here was the third answer, and a Swap Module into 9W9 sat on
     * "Loading..." until the user backed out. Reported from hardware. */
    if(!strcmp(_key, "ui_hierarchy"))
    {
        if(_len < 1) return -1;
        _buf[0] = 0;
        return 0;
    }

    /* The same hierarchy IS published — under a key the host doesn't probe —
     * for ui_chain.js to feed the shared param_pages controller (host 0.12.1+).
     * Its injected getParam rewrites "ui_hierarchy" requests to "ui_pages". */
    if(!strcmp(_key, "ui_pages"))
    {
        /* Whichever note map the engine is currently using. The declaration
         * carries a `note` per voice for Schwung's pad_layout / voices
         * contract, and g_note_map is switchable at RUNTIME — so one static
         * answer would be wrong half the time, and a host laying out pads from
         * it would put every voice in the wrong place with nothing to say why.
         * Two strings, one pointer choice, nothing built on the audio thread. */
        const int gm = (g_note_map != 0);
        const char *const j = gm ? er99_ui_hierarchy_gm_json
                                 : er99_ui_hierarchy_json;
        const int n = gm ? ER99_UI_HIERARCHY_GM_LEN : ER99_UI_HIERARCHY_LEN;
        if(_len <= n) return -1;
        memcpy(_buf, j, (size_t)n + 1);
        return n;
    }

    /*
     * Pad-follow. Publishes "<trigger-count>:<level-id>" so the Shadow UI can
     * jump the parameter page to the drum you just hit. The counter is what
     * the UI watches: it only navigates when a NEW hit arrives, so manually
     * browsing to another page is never yanked away underneath you.
     */
    if(!strcmp(_key, "ui_focus_level"))
    {
        /* THE LEVEL IDS THE HIERARCHY ACTUALLY DECLARES, in trigger order
         * (BD SD LT MT HT RS HC OHH CHH RC CR — note OHH before CHH and RC
         * before CR, which is not the order the nav list uses).
         *
         * These were "hat","hat","cym","cym", naming two levels that
         * gen_params.py does not emit: the hierarchy has chh, ohh, ride and
         * crash as four separate pages. A host resolving a focus answer
         * against the declared levels found nothing for any of the four, so
         * the two hats and the two cymbals silently never followed. */
        static const char *const level_of[ER99_NUM_TRIGGERS] = {
            "bd","sd","lt","mt","ht","rim","clap","ohh","chh","ride","crash"
        };
        const int v = inst->focus_voice;
        if(inst->focus_count == 0 || v < 0 || v >= ER99_NUM_TRIGGERS) return -1;
        return snprintf(_buf, (size_t)_len, "%u:%s",
                        inst->focus_count, level_of[v]);
    }

    if(!strcmp(_key, "clock_running"))
    {
        const int clock = (g_host && g_host->get_clock_status)
                        ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_STOPPED;
        return snprintf(_buf, (size_t)_len, "%d",
                        clock == MOVE_CLOCK_STATUS_RUNNING ? 1 : 0);
    }
    if(!strcmp(_key, "mutes"))
        return snprintf(_buf, (size_t)_len, "%u", (unsigned)inst->lane_mutes);
    if(!strcmp(_key, "state"))
    {
        int n = er99_engine_get_state(&inst->engine, _buf, _len);
        if(n < 0) return n;
        if(n < _len - 1)
            n += snprintf(_buf + n, (size_t)(_len - n), "mutes=%u;",
                          (unsigned)inst->lane_mutes);
        return n < _len ? n : _len - 1;
    }

    if(!strcmp(_key, "note_map"))
        return snprintf(_buf, (size_t)_len, "%d", g_note_map);

    float v = 0.0f;
    if(er99_engine_get_param(&inst->engine, _key, &v))
        return snprintf(_buf, (size_t)_len, "%.4f", v);

    return -1;
}

static int get_error(void *_instance, char *_buf, const int _len)
{
    (void)_instance; (void)_buf; (void)_len;
    return 0;
}

static void render_block(void *_instance, int16_t *_out_lr, const int _frames)
{
    er99_instance_t *inst = (er99_instance_t*)_instance;
    if(!inst) { memset(_out_lr, 0, (size_t)_frames * 2 * sizeof(int16_t)); return; }

    inst->samples_rendered += (uint64_t)_frames;

    /* Synced delay: feed the host tempo in. Cheap (one strcmp chain per
     * block); the engine only recomputes when the value actually changes,
     * and the delay's slewed read warps through tempo changes like tape. */
    if(g_host && g_host->get_bpm)
    {
        const float bpm = g_host->get_bpm();
        if(bpm > 20.0f && bpm != inst->last_bpm)
        {
            inst->last_bpm = bpm;
            er99_engine_set_raw(&inst->engine, "dly_bpm", bpm);
        }
    }

    /* Stack scratch: no allocation on the realtime path. Schwung's block is
     * 128 frames; guard anyway. */
    float mono[512];
    int done = 0;
    while(done < _frames)
    {
        int chunk = _frames - done;
        if(chunk > (int)(sizeof(mono)/sizeof(mono[0]))) chunk = (int)(sizeof(mono)/sizeof(mono[0]));

        er99_engine_render(&inst->engine, mono, chunk);

        for(int i=0; i<chunk; ++i)
        {
            float v = mono[i];
            if(v >  1.0f) v =  1.0f;
            if(v < -1.0f) v = -1.0f;
            const int16_t s = (int16_t)(v * 32767.0f);
            _out_lr[(done + i) * 2 + 0] = s;
            _out_lr[(done + i) * 2 + 1] = s;
        }
        done += chunk;
    }
}

static plugin_api_v2_t g_api = {
    .api_version     = 2,
    .create_instance = create_instance,
    .destroy_instance= destroy_instance,
    .on_midi         = on_midi,
    .set_param       = set_param,
    .get_param       = get_param,
    .get_error       = get_error,
    .render_block    = render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *_host)
{
    g_host = _host;
    return &g_api;
}
