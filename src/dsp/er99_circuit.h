/*
 * er99_circuit.h — circuit-informed TR-909 voice models (BD / SD / toms).
 *
 * er-99's oscillator voices are "triangle + VCA envelope", which is why its
 * toms sound thin and its kick lacks attack. The real 909 analog voice is:
 *
 *   - an oscillator producing a TRIANGLE that is rounded toward a SINE by
 *     back-to-back diodes conducting ~0.5-0.6 V either side of ground,
 *   - a PITCH envelope: a pulse at note start runs the oscillator high, then
 *     it sweeps down to the frequency set by Tune,
 *   - an AMPLITUDE envelope (Decay),
 *   - and a separate CLICK path — a short impulse plus lowpass-filtered noise,
 *     summed and given their own fast envelope (the Attack control). This is
 *     the beater contact, and it is what er-99 is missing entirely.
 *
 * The snare adds a second tuned oscillator and a noise path with its own
 * envelope and highpass (Snappy).
 *
 * Sources: Network-909 circuit analysis (cited by er-99 itself), Roland
 * TR-909 service documentation, and the sound-mod analysis at firstpr.com.au.
 *
 * GPL-3.0.
 */

#ifndef ER99_CIRCUIT_H
#define ER99_CIRCUIT_H

#include "webaudio.h"
#include "er99_reso.h"

typedef struct {
    /* --- panel controls --- */
    float tune;         /* Hz, oscillator base frequency          */
    float sweep_depth;  /* frequency multiplier at t=0            */
    float sweep_time;   /* ms, pitch envelope decay               */
    float decay;        /* ms, amplitude envelope                 */
    float attack;       /* 0..1 click level                       */
    float click_tone;   /* Hz, lowpass on the click noise         */
    float drive;        /* shaping amount (triangle->sine at ~2)   */
    float level;        /* 0..1                                   */
    float dist_type;    /* 0 diode, 1 hard clip, 2 folder, 3 crush */

    /* --- kick extras (0 disables; used by BD only) --- */
    float sub;          /* 0..1 sub-oscillator layer (one octave down)   */
    float tube;         /* 0..6 asymmetric tube stage ahead of the shaper */
    float drift;        /* 0..1 per-hit analog drift (pitch/level jitter) */

    /* --- snare / tom extras (0 disables) --- */
    float tune2;        /* Hz, second oscillator                  */
    float osc2_mix;     /* 0..1                                   */
    float snappy;       /* 0..1 noise level                       */
    float noise_decay;  /* ms                                     */
    float noise_hp;     /* Hz, highpass on the snare noise        */

    /* --- topology ---
     * 0 = oscillator + amplitude envelope (the er-99 lineage).
     * 1 = bridged-T: the panel controls drive a shock-excited resonator whose
     *     own damping IS the decay, which is how the hardware works. Set per
     *     voice, so the kick can keep one core while the shells use the other. */
    int         bridged_t;

    /* --- runtime --- */
    er99_reso_t reso, reso2;    /* shell / body networks (bridged_t)   */
    er99_pulse_t exc;           /* trigger pulse feeding them          */
    wa_osc_t    osc, osc2, sub_osc;
    wa_param_t  pitch, amp, click_env, noise_env;
    wa_biquad_t click_lp, noise_hpf, dc_block;
    uint32_t    rng;           /* per-voice drift RNG                */
    float       hit_tune;      /* this hit's (drifted) base pitch    */
    float       hit_gain;      /* this hit's (drifted) level scale   */
    float       out_gain;
    int         impulse;        /* samples of initial impulse left */
    double      mute_countdown;
    float       sample_rate;
} er99_bt_t;

/*
 * Back-to-back diode rounding. A triangle wave's sharp peaks are what make it
 * sound buzzy; the diodes conduct near the peaks and round them off, taking
 * the waveform most of the way to a sine. A tanh soft-clip is the standard
 * model of that pair. drive ~2 reproduces the 909's rounded triangle; higher
 * values push it back toward a harder, more aggressive tone.
 */
static inline float er99_diode_round(const float _x, const float _drive)
{
    const float k = _drive > 0.01f ? _drive : 0.01f;
    return tanhf(k * _x) / tanhf(k);
}

/*
 * Distortion flavours. Type 0 is the authentic diode rounding; the rest are
 * deliberate extensions — the point of doing this in software rather than
 * cloning the circuit exactly.
 */
static inline float er99_shape(const float _x, const float _drive, const float _type)
{
    const int t = (int)(_type + 0.5f);
    const float k = _drive > 0.01f ? _drive : 0.01f;
    switch(t)
    {
    case 1: {   /* hard clip — aggressive, square-ish */
        float v = _x * k;
        if(v >  1.0f) v =  1.0f;
        if(v < -1.0f) v = -1.0f;
        return v;
    }
    case 2: {   /* wavefolder — metallic, adds odd harmonics as drive rises */
        float v = _x * k;
        for(int i=0; i<3; ++i)
        {
            if(v >  1.0f) v =  2.0f - v;
            if(v < -1.0f) v = -2.0f - v;
        }
        return v;
    }
    case 3: {   /* bitcrush/decimate — lo-fi grit */
        const float steps = 2.0f + 30.0f / k;
        return floorf(_x * steps + 0.5f) / steps;
    }
    case 0:
    default:
        return er99_diode_round(_x, k);
    }
}

static inline void er99_bt_init(er99_bt_t *v, const float _sr)
{
    v->sample_rate = _sr;
    wa_osc_init(&v->osc,  _sr);
    wa_osc_init(&v->osc2, _sr);
    wa_osc_init(&v->sub_osc, _sr);
    /* DC blocker: the asymmetric tube stage adds a DC offset; a gentle 20 Hz
     * highpass removes it without touching the kick fundamental. */
    wa_biquad_set(&v->dc_block, WA_HIGHPASS, 20.0f, 0.7071f, 0.0f, _sr);
    v->rng = 0x9E3779B9u;
    v->hit_tune = v->tune;
    v->hit_gain = 1.0f;
    wa_param_init(&v->pitch, v->tune);
    wa_param_init(&v->amp, 0.0f);
    wa_param_init(&v->click_env, 0.0f);
    wa_param_init(&v->noise_env, 0.0f);
    wa_biquad_set(&v->click_lp, WA_LOWPASS,
                  v->click_tone > 0.0f ? v->click_tone : 3000.0f, 0.7071f, 0.0f, _sr);
    wa_biquad_set(&v->noise_hpf, WA_HIGHPASS,
                  v->noise_hp > 0.0f ? v->noise_hp : 800.0f, 0.7071f, 0.0f, _sr);
    er99_reso_init(&v->reso,  _sr);
    er99_reso_init(&v->reso2, _sr);
    v->exc.level = 0.0f; v->exc.decay = 0.0f;
    v->out_gain = 0.0f;
    v->impulse = 0;
    v->mute_countdown = 0.0;
}

static inline void er99_bt_trigger(er99_bt_t *v, const float _accent)
{
    const float sr = v->sample_rate;
    const float ms = 0.001f * sr;

    /* Per-hit drift: tiny random pitch/level variation, like component
     * tolerance and temperature in the analog original. At drift=1 the pitch
     * wanders about +-3% and level about +-10%. */
    v->hit_tune = v->tune;
    v->hit_gain = 1.0f;
    if(v->drift > 0.0f)
    {
        v->rng ^= v->rng << 13; v->rng ^= v->rng >> 17; v->rng ^= v->rng << 5;
        const float r1 = ((float)(v->rng & 0xFFFF) / 32768.0f) - 1.0f;
        v->rng ^= v->rng << 13; v->rng ^= v->rng >> 17; v->rng ^= v->rng << 5;
        const float r2 = ((float)(v->rng & 0xFFFF) / 32768.0f) - 1.0f;
        v->hit_tune *= 1.0f + 0.03f * v->drift * r1;
        v->hit_gain  = 1.0f + 0.10f * v->drift * r2;
    }

    if(v->bridged_t)
    {
        /* Hit the network with the trigger pulse. Nothing is cleared: a hit
         * that lands while the shell is still ringing adds to it, which is
         * what makes rolls and flams behave like the hardware. The pulse is
         * ~0.6 ms — wide enough to put its energy where the network rings
         * rather than landing as a click. */
        er99_pulse_fire(&v->exc, 1.6f * v->hit_gain, 0.6f, sr);
        v->reso.sat  = v->drive * 0.12f;
        v->reso2.sat = v->reso.sat;
        /* Force a coefficient refresh on the next sample so the sweep starts
         * from this hit's pitch, not the previous hit's. */
        v->reso.countdown = v->reso2.countdown = 0;

        wa_set_value(&v->pitch, v->hit_tune * v->sweep_depth);
        wa_exp_ramp(&v->pitch, v->hit_tune, v->sweep_time * ms);
        /* The click path is shared with the oscillator core; the beater/brush
         * transient is separate circuitry in the 909 too. */
        wa_set_value(&v->click_env, 1.0f);
        wa_exp_ramp(&v->click_env, 0.00001f, 3.0f * ms);
        if(v->snappy > 0.0f)
        {
            wa_set_value(&v->noise_env, 1.0f);
            wa_exp_ramp(&v->noise_env, 0.00001f,
                        (v->noise_decay > 0.0f ? v->noise_decay : 120.0f) * ms);
        }
        v->impulse = 0;                 /* the pulse IS the impulse here */
        v->out_gain = v->level * _accent * v->hit_gain;
        /* Damping sets the ring length, so the voice is done when the network
         * has run down — allow for the ring plus the noise tail. */
        {
            const float longest = v->decay > v->noise_decay ? v->decay : v->noise_decay;
            v->mute_countdown = (longest * 4.0f + 40.0f) * ms;
        }
        return;
    }

    /* Shock excitation: every hit starts at the same point in the cycle. The
     * 909's bridged-T oscillator is kicked by the trigger pulse and always
     * rings the same way. Free-running phase — what this did before — gave
     * every hit a different attack, and under the pitch sweep a different
     * perceived pitch, which is exactly the "the kick moves around" complaint.
     * 0.25 is the triangle's rising zero crossing, so the hit starts from
     * silence instead of stepping straight to the negative peak. */
    wa_osc_set_phase(&v->osc,     0.25);
    wa_osc_set_phase(&v->osc2,    0.25);
    wa_osc_set_phase(&v->sub_osc, 0.25);
    /* Same reason: the tube stage's DC blocker holds charge from the previous
     * hit, and its settling transient is the low thump that made otherwise
     * identical hits land differently. */
    wa_biquad_reset(&v->dc_block);

    /* Pitch: start high, sweep down to (drifted) Tune. */
    wa_set_value(&v->pitch, v->hit_tune * v->sweep_depth);
    wa_exp_ramp(&v->pitch, v->hit_tune, v->sweep_time * ms);

    /* Amplitude: the analog envelope is an RC discharge — exponential. */
    wa_set_value(&v->amp, 1.0f);
    wa_exp_ramp(&v->amp, 0.00001f, v->decay * ms);

    /* Click: very fast decay, this is the beater transient. */
    wa_set_value(&v->click_env, 1.0f);
    wa_exp_ramp(&v->click_env, 0.00001f, 3.0f * ms);

    /* Snare noise path. */
    if(v->snappy > 0.0f)
    {
        wa_set_value(&v->noise_env, 1.0f);
        wa_exp_ramp(&v->noise_env, 0.00001f,
                    (v->noise_decay > 0.0f ? v->noise_decay : 120.0f) * ms);
    }

    /* Two-sample impulse = the pulse generator's burst of energy. */
    v->impulse = 2;
    v->out_gain = v->level * _accent * v->hit_gain;

    const float longest = v->decay > v->noise_decay ? v->decay : v->noise_decay;
    v->mute_countdown = (longest + 20.0f) * ms;
}

static inline float er99_bt_render(er99_bt_t *v, const float _noise)
{
    if(v->mute_countdown <= 0.0) return 0.0f;
    v->mute_countdown -= 1.0;

    const float f   = wa_param_tick(&v->pitch);

    if(v->bridged_t)
    {
        /* Damping = decay: no amplitude envelope multiplies this, the network
         * runs down on its own the way the circuit does. */
        er99_reso_track(&v->reso, f, v->decay);
        const float x = er99_pulse_tick(&v->exc);
        float o = er99_reso_tick(&v->reso, x);

        if(v->tune2 > 0.0f && v->osc2_mix > 0.0f)
        {
            /* Second shell (snare) / body network (toms), tracking the same
             * sweep ratio and struck by the same pulse. */
            const float ratio = v->tune > 1.0f ? v->tune2 / v->tune : 1.0f;
            er99_reso_track(&v->reso2, f * ratio, v->decay);
            o += er99_reso_tick(&v->reso2, x) * v->osc2_mix;
            o /= (1.0f + v->osc2_mix);
        }

        float body = er99_shape(o, v->drive, v->dist_type);

        float click = 0.0f;
        if(v->attack > 0.0f)
        {
            click = wa_biquad_tick(&v->click_lp, _noise)
                  * wa_param_tick(&v->click_env) * v->attack;
        }
        else wa_param_tick(&v->click_env);

        float snare = 0.0f;
        if(v->snappy > 0.0f)
            snare = wa_biquad_tick(&v->noise_hpf, _noise)
                  * wa_param_tick(&v->noise_env) * v->snappy;

        /* Stop once the network is quiet, not merely once the timer expired:
         * cutting a still-ringing shell is a click. */
        if(v->mute_countdown < 200.0 && er99_reso_energy(&v->reso) < 0.0002f)
            v->mute_countdown = 0.0;

        return (body + click + snare) * v->out_gain;
    }

    const float amp = wa_param_tick(&v->amp);

    /* Oscillator: triangle, rounded toward sine by the diode pair. */
    float o = wa_osc_triangle(&v->osc, f);
    if(v->tune2 > 0.0f && v->osc2_mix > 0.0f)
    {
        /* Second oscillator tracks the same sweep ratio (snare shell / tom body). */
        const float ratio = v->tune > 1.0f ? v->tune2 / v->tune : 1.0f;
        o += wa_osc_triangle(&v->osc2, f * ratio) * v->osc2_mix;
        o /= (1.0f + v->osc2_mix);
    }
    /* Sub layer: one octave below, tracks the same pitch envelope. Rounded
     * hard toward a sine so it stays clean weight rather than buzz. */
    if(v->sub > 0.0f)
    {
        const float s2 = er99_diode_round(wa_osc_triangle(&v->sub_osc, f * 0.5f), 3.0f);
        o = (o + s2 * v->sub) / (1.0f + v->sub * 0.5f);
    }

    /* Tube stage: asymmetric soft clip ahead of the main shaper. The bias
     * makes positive and negative halves saturate differently (even
     * harmonics), then the DC blocker removes the offset it introduces. */
    if(v->tube > 0.0f)
    {
        const float k = 1.0f + v->tube;
        o = tanhf(k * (o + 0.12f * v->tube)) / tanhf(k);
        o = wa_biquad_tick(&v->dc_block, o);
    }

    float body = er99_shape(o, v->drive, v->dist_type) * amp;

    /* Click: impulse + lowpass-filtered noise, own envelope (Attack). */
    float click = 0.0f;
    if(v->impulse > 0) { click += 1.0f; v->impulse--; }
    click += wa_biquad_tick(&v->click_lp, _noise);
    click *= wa_param_tick(&v->click_env) * v->attack;

    /* Snare noise (Snappy). */
    float snare = 0.0f;
    if(v->snappy > 0.0f)
        snare = wa_biquad_tick(&v->noise_hpf, _noise)
              * wa_param_tick(&v->noise_env) * v->snappy;

    return (body + click + snare) * v->out_gain;
}

#endif /* ER99_CIRCUIT_H */
