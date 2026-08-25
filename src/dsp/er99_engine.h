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
typedef struct {
    /* parameters (er-99 instruments.ts) */
    float frequency;      /* Hz                                    */
    float offset;         /* osc2 detune; osc2 exists only if > 0   */
    float decay;          /* ms, amp envelope                      */
    float tone;           /* noise amount 0..1                     */
    float tone_decay;     /* ms, noise envelope                    */
    float volume;         /* 0..1                                  */
    float env_amount;     /* pitch env multiplier                  */
    float env_duration;   /* ms                                    */
    float saturation;     /* 0 = no waveshaper                     */
    int   filter_type;    /* wa_filter_type_t, -1 = none           */
    float filter_freq;

    /* --- extensions beyond stock er-99 (default to stock values) --- */
    float drive;          /* extra pre-shaper gain, 1.0 = stock    */
    /*
     * osc2_level > 0 fixes er-99's thin toms. Stock only builds osc2 when
     * offset > 0 (so Med/Hi Tom never get one), and wires it into noiseInput,
     * where it plays at `tone` level through the short noise envelope — on Low
     * Tom that is 5% for 100 ms, i.e. inaudible. With osc2_level set, osc2 uses
     * |offset|, tracks its own pitch envelope, and runs through the AMP
     * envelope at this level. 0 = stock behaviour.
     */
    float osc2_level;
    float filter_q;       /* resonance; 1.0 (no emphasis) = stock  */
    float noise_lp;       /* Hz, lowpass on the noise path; 0 = off */
    wa_biquad_t noise_filter;

    /* runtime */
    wa_osc_t    osc, osc2;
    wa_param_t  pitch;        /* osc frequency envelope            */
    wa_param_t  noise_gain;   /* noiseInput.gain                   */
    wa_param_t  amp;          /* input.gain                        */
    float       out_gain;     /* output.gain (set on trigger)      */
    wa_biquad_t filter;
    wa_shaper_t shaper;
    int         has_filter;
    int         amp_stage;    /* 0 = linear attack, 1 = exp decay  */
    double      amp_decay_samples;
    double      mute_countdown;
} er99_instrument_t;

/* ---- Rim shot ---- */
typedef struct {
    float decay;          /* ms                    */
    float volume;
    float saturation;
    float hipass_freq;
    float filter_freqs[3];
    float filter_qs[3];
    /* Stock er-99 only feeds bandpass[0] (see er99_engine.c). Set to 1 to
     * drive all three in parallel — closer to the real hardware. */
    int   all_bands;

    wa_param_t  noise_gain;
    wa_biquad_t bp[3];
    wa_biquad_t hipass;
    wa_shaper_t shaper;
    float       out_gain;
    int         buf_pos;      /* position in the 200-sample rim table */
    double      mute_countdown;
} er99_rim_t;

/* ---- Hand clap ---- */
typedef struct {
    float decay;          /* ms                              */
    float delay_const;    /* ms between taps ("spread")      */
    float volume;
    float tune;           /* tone bandpass centre, Hz        */
    float tone_decay;     /* ms                              */
    float hipass_freq;
    float filter_freqs[2];
    float filter_qs[2];

    wa_param_t  noise_gain;    /* tone path       */
    wa_param_t  delay_out;     /* the 5 taps      */
    wa_biquad_t tone_bp;
    wa_biquad_t hp, bp;        /* serial 900 / 1200 */
    wa_biquad_t hipass;
    wa_osc_t    modulator;     /* 40 Hz sawtooth  */
    float       out_gain;
    int         tap_index;
    double      next_tap_samples;
    double      mute_countdown;
} er99_clap_t;

/* ---- Samplers: hi-hat, ride, crash ---- */
typedef struct {
    float  decay;         /* ms (open)     */
    float  decay_closed;  /* ms, hh only   */
    float  volume;
    float  pitch;         /* playbackRate  */
    float  drive;         /* per-sampler distortion (er99_shape) */
    float  dist_type;

    const float *buffer;
    uint32_t     length;
    double       pos;
    wa_param_t   out;
    double       mute_countdown;
} er99_sampler_t;

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
    float accent;       /* globalParams.globalAccent = 2.0 */

    /* One-knob bus glue — OURS, requested as a feature, not 909 circuitry,
     * and unlike the compressor this module once inherited it is honest
     * about it: at zero the stage is bypassed entirely (bit-identical), and
     * the knob blends threshold, ratio and auto-makeup together. */
    float comp;         /* 0..1 amount; 0 = hard bypass  */
    float comp_env_db;  /* smoothed gain reduction state */
    float comp_det;     /* rectified level follower      */
} er99_master_t;

typedef struct {
    float sample_rate;

    er99_instrument_t inst[ER99_NUM_INSTRUMENTS];
    /* Circuit-informed BD/SD/tom models (er99_circuit.h). `circuit_model`
     * selects which engine the 5 oscillator voices use: 1 = circuit (default,
     * the good one), 0 = stock er-99 for comparison. */
    er99_bt_t         bt[ER99_NUM_INSTRUMENTS];
    int               circuit_model;
    er99_rim_t        rim;      /* stock er-99 (circuit_model=0) */
    er99_clap_t       clap;
    er99_tom_t        tom909[3]; /* LT/MT/HT, three oscillators each */
    er99_rim909_t     rim909;   /* circuit models (default)      */
    er99_clap909_t    clap909;
    er99_sampler_t    sampler[ER99_NUM_SAMPLERS];

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

/* velocity 0..127; >= ER99_ACCENT_VELOCITY applies globalAccent. */
#define ER99_ACCENT_VELOCITY 100
void er99_engine_trigger(er99_engine_t *e, er99_trigger_t which, int velocity);

/* Render mono into out (engine is mono; the plugin duplicates to stereo). */
void er99_engine_render(er99_engine_t *e, float *out, int frames);

/* Raw access in engineering units (ms, Hz, ...). Internal. */
int er99_engine_set_raw(er99_engine_t *e, const char *key, float value);
int er99_engine_get_raw(const er99_engine_t *e, const char *key, float *out);

/* Public parameter surface: pot positions 0..127, like the hardware panel.
 * Enum-style keys (dist_type, circuit_model, rs_all_bands) pass through as
 * their own small integer values rather than being scaled. */
void er99_engine_seed_pots(er99_engine_t *e);
int er99_engine_set_param(er99_engine_t *e, const char *key, float pot);
int er99_engine_get_param(const er99_engine_t *e, const char *key, float *pot);

/* Self-contained state blob — required for slot autosave and User Presets. */
int er99_engine_get_state(const er99_engine_t *e, char *buf, int len);
int er99_engine_set_state(er99_engine_t *e, const char *blob);

extern const char *const er99_trigger_names[ER99_NUM_TRIGGERS];

#endif /* ER99_ENGINE_H */
