/*
 * er99_engine.h — ER-99 drum voices, ported from matthewcieplak/er-99.
 *
 * The original is a Web Audio node graph in TypeScript; this is a faithful
 * reimplementation against the same spec-defined node behaviours (webaudio.h).
 *
 * GPL-3.0 (er-99 is GPL-3.0).
 */

#ifndef ER99_ENGINE_H
#define ER99_ENGINE_H

#include <stdint.h>
#include "webaudio.h"
#include "er99_circuit.h"
#include "er99_tom909.h"
#include "er99_pots.h"
#include "er99_perc909.h"

/* Trigger IDs. 'chh' is not a separate voice — it retriggers the hi-hat with
 * the closed decay time, exactly as er-99's playNote() does. */
typedef enum {
    ER99_BD = 0, ER99_SD, ER99_LT, ER99_MT, ER99_HT,
    ER99_RS, ER99_HC,
    ER99_OHH, ER99_CHH, ER99_RC, ER99_CR,
    ER99_NUM_TRIGGERS
} er99_trigger_t;

#define ER99_NUM_INSTRUMENTS 5   /* bd sd lt mt ht */
/* Open hat, ride, crash, closed hat. The closed hat is its own sampler rather
 * than a second decay on the open one, so it can have its own tuning, level and
 * drive; it plays the same buffer, and the two choke each other (see
 * er99_trigger) because they are one physical hat. */
#define ER99_NUM_SAMPLERS    4   /* ohh ride crash chh */
#define ER99_SAMP_OHH        0
#define ER99_SAMP_RC         1
#define ER99_SAMP_CR         2
#define ER99_SAMP_CHH        3

/* ---- Oscillator+noise voices: BD, SD, LT, MT, HT ---- */


/* ---- Rim shot ---- */


/* ---- Hand clap ---- */


/* ---- Samplers: hi-hat, ride, crash ---- */
typedef struct {
    float  decay;         /* ms (open)     */
    float  decay_closed;  /* ms, hh only   */
    float  volume;
    float  pitch;         /* playbackRate  */
    float  drive;         /* per-sampler distortion (er99_shape) */
    float  dist_type;

    float  crush_st[2];

    const float *buffer;
    uint32_t     length;
    double       pos;
    wa_param_t   out;
    double       mute_countdown;
} er99_sampler_t;

/* ---- Send FX: one reverb, one delay, fed by per-voice sends ----
 *
 * Old-school on purpose. The reverb is a small early-digital topology (four
 * combs, two allpasses) with 12-BIT QUANTISATION IN THE FEEDBACK PATH — that
 * is where the character of the early rack units lives — and damping in the
 * loop. The delay is a 90s rack digital: mono, feedback through a darkening
 * one-pole, 12-bit write, and the time knob SLEWS so it warps instead of
 * clicking. Both take a highpass on the send input so low end stays out.
 * Sends default to zero: with them down the engine is bit-identical to one
 * that has no FX at all. The kick has no sends, deliberately. */
#define ER99_DLY_MAX 88200            /* 2 s at 44.1k: a dotted half at 90 BPM */
#define ER99_DLY_DIVS 13
typedef struct {
    float time_ms, fdbk, tone, hpf_hz, level;
    float divi;                       /* note division index, 0..12 */
    float bpm;                        /* fed by the host each block */
    wa_biquad_t hp;
    float buf[ER99_DLY_MAX];
    int   w;
    float dcur;                       /* slewed delay, samples */
    float lp;                         /* loop damping state    */
} er99_dly_t;

typedef struct {
    float decay, tone, hpf_hz, level;
    wa_biquad_t hp;
    float comb[4][1356];
    int   cpos[4];
    float cdmp[4];                    /* per-comb damping state */
    float apb[2][556];
    int   apos[2];
} er99_verb_t;

/* ---- Master bus ---- */
typedef struct {
    /* No compressor and no limiter. er-99 inherited a Web Audio
     * DynamicsCompressorNode on the master bus; a TR-909 has nothing of the
     * kind — its voices sum through a mixer straight into the output amp. The
     * headroom comes from the gain structure instead (voice Level tops out at
     * the ceiling, master volume 0.35), and the only thing left on the bus is
     * the distortion, which is ours and optional.
     *
     * Master distortion: whole-kit drive.
     * dist_mode: 0=Off, 1=Diode, 2=Hard Clip, 3=Wavefolder, 4=Bitcrush
     * (modes 1..4 map to er99_shape types 0..3). */
    float drive;
    float dist_mode;
    float volume;

    /* Gain a FULL-velocity hit reaches — the 909's accent bus, and the level
     * this kit has always played at from a sequencer (Move sends 100+, which
     * the old accent switch put here). Velocity scales down from it, never
     * above it, so this stays the kit's reference level. */
    float accent;

    /* Velocity sensitivity, 0..1 — how far below Accent a soft hit falls.
     * 0 ignores velocity entirely and every hit plays at Accent, which is
     * exactly how the kit sounded before velocity existed. It only ever
     * attenuates: nothing gets louder than it plays at 0. */
    float vel_depth;

    /* One-knob bus glue — OURS, requested as a feature, not 909 circuitry,
     * and unlike the compressor this module once inherited it is honest
     * about it: at zero the stage is bypassed entirely (bit-identical), and
     * the knob blends threshold, ratio and auto-makeup together. */
    float crush_st[2];  /* master bitcrush decimator     */
    float comp;         /* 0..1 amount; 0 = hard bypass  */
    float comp_env_db;  /* smoothed gain reduction state */
    float comp_det;     /* rectified level follower      */
} er99_master_t;

typedef struct {
    float sample_rate;

    /* Circuit-informed BD/SD/tom models (er99_circuit.h). The er-99 legacy
     * selects which engine the 5 oscillator voices use: 1 = circuit (default,
     * the good one), 0 = stock er-99 for comparison. */
    er99_bt_t         bt[ER99_NUM_INSTRUMENTS];
    er99_tom_t        tom909[3]; /* LT/MT/HT, three oscillators each */
    er99_rim909_t     rim909;   /* circuit models (default)      */
    er99_clap909_t    clap909;
    er99_sampler_t    sampler[ER99_NUM_SAMPLERS];

    er99_verb_t   verb;
    er99_dly_t    dly;
    float         send_rev[ER99_NUM_TRIGGERS];
    float         send_dly[ER99_NUM_TRIGGERS];

    er99_master_t master;
    wa_noise_t    noise;


    /* Pot positions (0..127) — the external parameter surface. Real
     * engineering values are derived from these via g_er99_pots. */
    unsigned char pots[ER99_POT_COUNT];

    /* decoded sample data, owned by the engine */
    float   *sample_data[ER99_NUM_SAMPLERS];
    uint32_t sample_len[ER99_NUM_SAMPLERS];
} er99_engine_t;

/* Initialise with stock er-99 parameters. module_dir may be NULL, in which
 * case the samplers stay silent (engine still runs). */
void er99_engine_init(er99_engine_t *e, float sample_rate, const char *module_dir);
void er99_engine_free(er99_engine_t *e);

/* velocity 0..127; scales the voice, reaching the Accent gain at 127. */
void er99_engine_trigger(er99_engine_t *e, er99_trigger_t which, int velocity);

/* Render mono into out (engine is mono; the plugin duplicates to stereo). */
void er99_engine_render(er99_engine_t *e, float *out, int frames);

/* Raw access in engineering units (ms, Hz, ...). Internal. */
int er99_engine_set_raw(er99_engine_t *e, const char *key, float value);
int er99_engine_get_raw(const er99_engine_t *e, const char *key, float *out);

/* Public parameter surface: pot positions 0..127, like the hardware panel.
 * Enum-style keys (dist_type, master_dist) pass through as
 * their own small integer values rather than being scaled. */
void er99_engine_seed_pots(er99_engine_t *e);
int er99_engine_set_param(er99_engine_t *e, const char *key, float pot);
int er99_engine_get_param(const er99_engine_t *e, const char *key, float *pot);

/* Self-contained state blob — required for slot autosave and User Presets. */
int er99_engine_get_state(const er99_engine_t *e, char *buf, int len);
int er99_engine_set_state(er99_engine_t *e, const char *blob);

extern const char *const er99_trigger_names[ER99_NUM_TRIGGERS];

#endif /* ER99_ENGINE_H */
