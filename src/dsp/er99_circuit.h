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

    /* --- runtime --- */
    wa_osc_t    osc, osc2, sub_osc;
    wa_param_t  pitch, amp, click_env, noise_env;
    wa_biquad_t click_lp, noise_hpf, noise_lpf, dc_block;
    /* ENV4's shape, from the SD schematic: C73 holds the noise VCA fully open
     * for ~24 ms before the decay starts; the discharge then runs to a
     * NEGATIVE rail (VR7+R254), so it falls faster than a natural exponential;
     * and the single-transistor VCA passes nothing below ~0.4 V of its ~15 V
     * envelope — the tail GATES off at about -31 dB instead of fading out.
     * This plateau + hard gate is the 909 snare's signature snap. */
    int         noise_hold;    /* samples of plateau left    */
    int         noise_gated;   /* env fell through the floor */
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
/* Turn-on voltage of the single-transistor VCAs, as a fraction of the ~15 V
 * envelope, and the gain that renormalises what is left so a full-scale
 * envelope still opens the VCA fully. */
/* The Snappy pot is a level control into a summing amp whose resistors fix how
 * the noise sits against the shells (R294 47K for the noise leg).
 *
 * Calibrated against Gus's own 909 with the pot AT NOON, which is where the
 * reference recording was made: noise-to-shell 0.18 / 0.22 / 0.48 at 5 / 20 /
 * 50 ms. The first pass scaled the noise DOWN, having assumed that same
 * balance came from a maxed pot — at noon it left the noise four to six times
 * too quiet with the shells barking through, which is what Gus heard. Noon now
 * lands on the hardware and the top of the pot goes past it, as a pot with
 * travel left should. */
#define ER99_SNAPPY_MIX 1.55f

#define ER99_VCA_VT    0.027f
#define ER99_VCA_NORM  (1.0f / (1.0f - ER99_VCA_VT))

static inline float er99_shape(const float _x, const float _drive, const float _type)
{
    const int t = (int)(_type + 0.5f);
    const float k = _drive > 0.01f ? _drive : 0.01f;
    switch(t)
    {
    /* Below unity drive the clip and fold stages would only ATTENUATE (x*k
     * never reaches the rails), so switching Dist at low Drive changed the
     * level by up to 14 dB and nothing else. Normalising by k below unity
     * makes Drive 0 genuinely transparent for every type. */
    case 1: {   /* hard clip — aggressive, square-ish */
        float v = _x * k;
        if(v >  1.0f) v =  1.0f;
        if(v < -1.0f) v = -1.0f;
        return k < 1.0f ? v / k : v;
    }
    case 2: {   /* wavefolder — metallic, adds odd harmonics as drive rises */
        float v = _x * k;
        for(int i=0; i<3; ++i)
        {
            if(v >  1.0f) v =  2.0f - v;
            if(v < -1.0f) v = -2.0f - v;
        }
        return k < 1.0f ? v / k : v;
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
    /* The board's noise path is a Sallen-Key HP and LP pair — a bandpass, not
     * a bare highpass. The LP is what keeps the snap from being pure hiss. */
    wa_biquad_set(&v->noise_lpf, WA_LOWPASS, 6500.0f, 0.7071f, 0.0f, _sr);
    wa_biquad_reset(&v->noise_hpf);
    wa_biquad_reset(&v->noise_lpf);
    wa_biquad_reset(&v->click_lp);
    wa_biquad_reset(&v->dc_block);
    v->noise_hold = 0;
    v->noise_gated = 0;
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

    /* Every hit starts at the same point in the cycle. Free-running phase —
     * what this did before — gave every hit a different attack, and under the
     * pitch sweep a different perceived pitch: the "the kick moves around"
     * complaint. 0.25 is the triangle's rising zero crossing, so the hit starts
     * from silence instead of stepping straight to the negative peak.
     *
     * How much of this the 909 itself does is not settled: its voices are
     * CMOS-inverter oscillators whose frequency comes from a starved supply
     * rail, and they stop outright when that CV is low enough — so the pitch
     * pulse at note start does restart them from rest at low Tune settings.
     * With the CV higher they keep running between hits and the VCA does the
     * gating. Treat this reset as ours, for consistency, not as a claim about
     * the circuit. (TR-909 service notes, voicing board; Whittle's SD notes.) */
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

    /* Snare noise path: plateau first (C73), decay armed when it ends. */
    if(v->snappy > 0.0f)
    {
        wa_set_value(&v->noise_env, 1.0f);
        v->noise_hold = (int)(24.0f * ms);
        v->noise_gated = 0;
        (void)(v->noise_decay > 0.0f ? v->noise_decay : 120.0f);
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

    float amp = wa_param_tick(&v->amp);
    /* The shell VCAs are the same crude single-transistor stage as the noise
     * one (Q50/Q51): conduction falls to nothing as the envelope approaches
     * the transistor's turn-on voltage (~0.4 V of ~15 V), so the voice stops
     * well short of the exponential's tail. Modelled as the threshold
     * SUBTRACTED from the envelope, not as a switch at it — gain reaches zero
     * continuously, which is what the junction does. Zeroing gain outright
     * (what this did first) cuts the shell mid-cycle and clicks. */
    if(v->tune2 > 0.0f && v->osc2_mix > 0.0f)
        amp = amp > ER99_VCA_VT ? (amp - ER99_VCA_VT) * ER99_VCA_NORM : 0.0f;

    /* Oscillator: triangle, rounded toward sine by the diode pair. */
    float o = wa_osc_triangle(&v->osc, f);
    if(v->tune2 > 0.0f && v->osc2_mix > 0.0f)
    {
        /* Two-shell voice (snare). On the board EACH VCO has its own diode
         * pair and its own VCA — the two shells are rounded separately and
         * only meet at the summing node. Shaping the SUM instead (what this
         * did) intermodulates them: 205 and 325 Hz through one nonlinearity
         * grow a ~120 Hz difference tone, a phantom pitch no 909 makes. The
         * user Drive stage after this still distorts the mix on purpose;
         * at low Drive it stays clean. */
        o = er99_diode_round(o, 2.0f);
        const float ratio = v->tune > 1.0f ? v->tune2 / v->tune : 1.0f;
        o += er99_diode_round(wa_osc_triangle(&v->osc2, f * ratio), 2.0f)
           * v->osc2_mix;
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

    /* Snare noise (Snappy): ENV4's plateau, accelerated fall, and gate. */
    float snare = 0.0f;
    if(v->snappy > 0.0f && !v->noise_gated)
    {
        if(v->noise_hold > 0)
        {
            /* C73 still holding the VCA open. Arm the decay on the last
             * sample; the extra 0.7 on the time models the discharge running
             * toward the negative rail — faster than a decay to zero. */
            if(--v->noise_hold == 0)
                wa_exp_ramp(&v->noise_env, 0.00001f,
                            (v->noise_decay > 0.0f ? v->noise_decay : 120.0f)
                            * 0.7f * 0.001f * v->sample_rate);
        }
        float env = wa_param_tick(&v->noise_env);
        if(v->noise_hold <= 0)
        {
            /* Same junction, same soft cutoff — and once it is shut, stay shut
             * (the envelope only falls further). */
            env = env > ER99_VCA_VT ? (env - ER99_VCA_VT) * ER99_VCA_NORM : 0.0f;
            if(env <= 0.0f) v->noise_gated = 1;
        }
        snare = wa_biquad_tick(&v->noise_lpf,
                    wa_biquad_tick(&v->noise_hpf, _noise))
              * env * v->snappy * ER99_SNAPPY_MIX;
    }

    return (body + click + snare) * v->out_gain;
}

#endif /* ER99_CIRCUIT_H */
