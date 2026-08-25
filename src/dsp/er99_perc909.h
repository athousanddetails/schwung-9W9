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
/* 723/210 from the BOM's 22K x 10n section against the measured body. */
/* Centred between the recording's 700 and 890 Hz pair and left broad (Q ~3),
 * so one section covers both the way the real spectrum shows them — nearly
 * equal in level — rather than spiking on one. */
#define ER99_RIM_F3(tune) ((tune) * 3.76f)

typedef struct {
    float tune;        /* Hz, low resonance ("tock")  */
    float tune2;       /* Hz, high resonance ("tick") */
    float res;         /* Q of both resonators        */
    float decay;       /* ms                          */
    float noise_mix;   /* 0..1 transient noise        */
    float drive;
    float dist_type;
    float level;

    wa_biquad_t bp1, bp2, bp3, hp;
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
    /* Both resonances measure Q ~10-11 on a real rim (ring times 15.5 ms at
     * 210 Hz and 7.5 ms at 480 Hz — the upper one dies sooner because it is
     * higher, not because it is damped harder). The 0.7 factor here made it
     * die too slowly relative to the body. */
    wa_biquad_set(&v->bp2, WA_BANDPASS, v->tune2 > 0 ? v->tune2 : 480.0f,
                  v->res > 0 ? v->res * 1.1f : 11.0f, 0.0f, _sr);
    /* Third section — the wood. The rim's capacitors come in matched pairs in
     * the BOM (4.7n, 27n, 10n, 10n), i.e. more Sallen-Key sections than the
     * two obvious resonances, and the RC values land where the recording has
     * energy: 12K x 27n = 491 Hz (the 480), 22K x 10n = 723 Hz (this one),
     * 12K x 10n = 1327 Hz (the faint top). Broad and low, so it covers the
     * 700 and 890 Hz pair the real drum shows rather than a single spike. */
    wa_biquad_set(&v->bp3, WA_BANDPASS, ER99_RIM_F3(v->tune),
                  v->res > 0 ? v->res * 0.30f : 3.0f, 0.0f, _sr);
    wa_biquad_set(&v->hp, WA_HIGHPASS, 120.0f, 0.7071f, 0.0f, _sr);
    wa_param_init(&v->amp, 0.0f);
    v->impulse = 0;
    v->mute_countdown = 0.0;
    v->accent_gain_ = 1.0f;
}

static inline void er99_rim909_retune(er99_rim909_t *v)
{
    wa_biquad_set(&v->bp1, WA_BANDPASS, v->tune, v->res, 0.0f, v->sample_rate);
    wa_biquad_set(&v->bp2, WA_BANDPASS, v->tune2, v->res * 1.1f, 0.0f, v->sample_rate);
    wa_biquad_set(&v->bp3, WA_BANDPASS, ER99_RIM_F3(v->tune), v->res * 0.30f,
                  0.0f, v->sample_rate);
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

    /* The trigger pulse shock-excites the network and then nothing drives it
     * — it rings down on its own. Noise belongs IN the pulse (a strike is
     * broadband for an instant), never after it: feeding noise continuously,
     * as this did, turns the voice into two filters ringing on hiss for the
     * whole envelope, which is audibly not a rim shot and shows up as HF the
     * real drum does not have (1180 Hz measures 3 against 56 at 210 Hz). */
    float exc = 0.0f;
    if(v->impulse > 0)
    {
        exc += 1.0f + _noise * v->noise_mix;
        v->impulse--;
    }

    const float a = wa_param_tick(&v->amp);
    /* Makeup: a high-Q bandpass fed a two-sample impulse puts out very little,
     * so the resonators need gain to sit level with the other voices. */
    /* Measured on a real 909 rim, the upper resonance sits at about half the
     * lower one (56 vs 29 at 2 ms). Equal-ish gains made the edge dominate. */
    /* Rebalanced once the noise stopped driving the pair: the upper resonator
     * had been living off that hiss, and lost most of its level when it went. */
    float y = wa_biquad_tick(&v->bp1, exc) * 60.0f
            + wa_biquad_tick(&v->bp2, exc) * 32.0f
            + wa_biquad_tick(&v->bp3, exc) * 330.0f;
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
