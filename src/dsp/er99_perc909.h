/*
 * er99_perc909.h — circuit-informed TR-909 RIM SHOT and HAND CLAP.
 *
 * Replaces er-99's versions, which are not the target any more:
 *
 *  - er-99's rim is a single 220 Hz bandpass (its "parallel" topology only ever
 *    feeds filter 0 — the other two are wired to the output but nothing drives
 *    them), so it comes out dull and one-dimensional.
 *  - er-99's clap approximates the burst with five scheduled gain taps and no
 *    real delay line.
 *
 * The hardware:
 *
 *  RIM SHOT — a trigger pulse shock-excites a high-Q resonant network. Very
 *  short, and its character is the *pair* of resonances (a low "tock" body and
 *  a high "tick" edge) ringing together, plus a little noise for the transient.
 *
 *  HAND CLAP — one noise source through a bandpass, gated by TWO envelopes
 *  summed: a burst of three fast pulses roughly 10 ms apart (the "hands"), and
 *  a longer decaying tail (the room). That two-part structure is what makes a
 *  909 clap sound like a clap rather than a noise blip.
 *
 * Hats, ride and crash stay sampled — the real 909's cymbals are 6-bit PCM, so
 * sampling them is accurate, not a shortcut.
 *
 * GPL-3.0.
 */

#ifndef ER99_PERC909_H
#define ER99_PERC909_H

#include "webaudio.h"
#include "er99_circuit.h"   /* er99_shape() */

/* ===================== RIM SHOT ===================== */
typedef struct {
    float tune;        /* Hz, low resonance ("tock")  */
    float tune2;       /* Hz, high resonance ("tick") */
    float res;         /* Q of both resonators        */
    float decay;       /* ms                          */
    float noise_mix;   /* 0..1 transient noise        */
    float drive;
    float dist_type;
    float level;

    wa_biquad_t bp1, bp2, hp;
    wa_param_t  amp;
    int         impulse;
    double      mute_countdown;
    float       sample_rate;
    float       accent_gain_;
} er99_rim909_t;

static inline void er99_rim909_init(er99_rim909_t *v, const float _sr)
{
    v->sample_rate = _sr;
    wa_biquad_set(&v->bp1, WA_BANDPASS, v->tune  > 0 ? v->tune  : 220.0f,
                  v->res > 0 ? v->res : 12.0f, 0.0f, _sr);
    wa_biquad_set(&v->bp2, WA_BANDPASS, v->tune2 > 0 ? v->tune2 : 1300.0f,
                  v->res > 0 ? v->res * 0.7f : 8.0f, 0.0f, _sr);
    wa_biquad_set(&v->hp, WA_HIGHPASS, 120.0f, 0.7071f, 0.0f, _sr);
    wa_param_init(&v->amp, 0.0f);
    v->impulse = 0;
    v->mute_countdown = 0.0;
    v->accent_gain_ = 1.0f;
}

static inline void er99_rim909_retune(er99_rim909_t *v)
{
    wa_biquad_set(&v->bp1, WA_BANDPASS, v->tune, v->res, 0.0f, v->sample_rate);
    wa_biquad_set(&v->bp2, WA_BANDPASS, v->tune2, v->res * 0.7f, 0.0f, v->sample_rate);
}

static inline void er99_rim909_trigger(er99_rim909_t *v, const float _accent)
{
    const float ms = 0.001f * v->sample_rate;
    wa_set_value(&v->amp, 1.0f);
    wa_exp_ramp(&v->amp, 0.00001f, v->decay * ms);
    v->impulse = 2;                        /* the trigger pulse */
    v->accent_gain_ = _accent;
    v->mute_countdown = (v->decay + 20.0f) * ms;
}

static inline float er99_rim909_render(er99_rim909_t *v, const float _noise)
{
    if(v->mute_countdown <= 0.0) return 0.0f;
    v->mute_countdown -= 1.0;

    /* Pulse + a touch of noise shock-excites both resonators. */
    float exc = 0.0f;
    if(v->impulse > 0) { exc += 1.0f; v->impulse--; }
    exc += _noise * v->noise_mix;

    const float a = wa_param_tick(&v->amp);
    /* Makeup: a high-Q bandpass fed a two-sample impulse puts out very little,
     * so the resonators need gain to sit level with the other voices. */
    float y = wa_biquad_tick(&v->bp1, exc) * 7.0f
            + wa_biquad_tick(&v->bp2, exc) * 5.0f;
    y = er99_shape(y * a, v->drive, v->dist_type);
    y = wa_biquad_tick(&v->hp, y);
    return y * v->level * v->accent_gain_;
}

/* ===================== HAND CLAP ===================== */
typedef struct {
    float tune;         /* Hz, bandpass centre       */
    float res;          /* Q                         */
    float spread;       /* ms between the 3 pulses   */
    float burst_decay;  /* ms, each pulse            */
    float tail_decay;   /* ms, the room tail         */
    float tail_level;   /* 0..1                      */
    float drive;
    float dist_type;
    float level;

    wa_biquad_t bp, hp;
    wa_param_t  burst, tail;
    int         pulse_index;
    double      next_pulse;
    double      mute_countdown;
    float       sample_rate;
    float       accent_gain_;
} er99_clap909_t;

static inline void er99_clap909_init(er99_clap909_t *v, const float _sr)
{
    v->sample_rate = _sr;
    wa_biquad_set(&v->bp, WA_BANDPASS, v->tune > 0 ? v->tune : 1000.0f,
                  v->res > 0 ? v->res : 2.5f, 0.0f, _sr);
    wa_biquad_set(&v->hp, WA_HIGHPASS, 400.0f, 0.7071f, 0.0f, _sr);
    wa_param_init(&v->burst, 0.0f);
    wa_param_init(&v->tail, 0.0f);
    v->pulse_index = 3;
    v->mute_countdown = 0.0;
    v->accent_gain_ = 1.0f;
}

static inline void er99_clap909_retune(er99_clap909_t *v)
{
    wa_biquad_set(&v->bp, WA_BANDPASS, v->tune, v->res, 0.0f, v->sample_rate);
}

static inline void er99_clap909_trigger(er99_clap909_t *v, const float _accent)
{
    const float ms = 0.001f * v->sample_rate;
    v->pulse_index = 0;
    v->next_pulse = 0.0;
    /* The tail runs from the start, under the burst. */
    wa_set_value(&v->tail, v->tail_level);
    wa_exp_ramp(&v->tail, 0.00001f, v->tail_decay * ms);
    v->accent_gain_ = _accent;
    v->mute_countdown = (v->tail_decay + v->spread * 4.0f + 20.0f) * ms;
}

static inline float er99_clap909_render(er99_clap909_t *v, const float _noise)
{
    if(v->mute_countdown <= 0.0) return 0.0f;
    v->mute_countdown -= 1.0;

    const float ms = 0.001f * v->sample_rate;

    /* Three fast pulses = the hands. */
    if(v->pulse_index < 3)
    {
        if(v->next_pulse <= 0.0)
        {
            wa_set_value(&v->burst, 1.0f);
            wa_exp_ramp(&v->burst, 0.00001f, v->burst_decay * ms);
            v->pulse_index++;
            v->next_pulse = v->spread * ms;
        }
        v->next_pulse -= 1.0;
    }

    const float env = wa_param_tick(&v->burst) + wa_param_tick(&v->tail);
    float y = wa_biquad_tick(&v->bp, _noise) * env;
    y = er99_shape(y, v->drive, v->dist_type);
    y = wa_biquad_tick(&v->hp, y);
    return y * v->level * v->accent_gain_;
}

#endif /* ER99_PERC909_H */
