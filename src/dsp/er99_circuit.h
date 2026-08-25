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

    /* --- snare / tom extras (0 disables) --- */
    float tune2;        /* Hz, second oscillator                  */
    float osc2_mix;     /* 0..1                                   */
    float snappy;       /* 0..1 noise level                       */
    float noise_decay;  /* ms                                     */
    float noise_hp;     /* Hz, highpass on the snare noise        */

    /* --- runtime --- */
    wa_osc_t    osc, osc2;
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
    /* The kick's amplitude envelope holds near full before it falls: measured
     * on BD 909 Clean Long A, the level is still 0.96 at 40 ms and only then
     * decays with a ~120 ms time constant. A plain exponential from t=0 misses
     * the whole front of the note. Milliseconds; 0 on voices that do not do
     * this (the snare's shells start decaying at once). */
    float       amp_hold;
    int         amp_hold_left;
    /* Stock-909 pitch sweep (BD only), measured from four Tune positions of a
     * real machine (letters F/A/B/C on the recordings): the BASE pitch is
     * fixed at ~49 Hz in every one; the knob raises the sweep's height and
     * length TOGETHER — height tracks 4.1 Hz per ms of time constant across
     * the whole ladder (the C9 charge/discharge race), tau spanning ~7-33 ms
     * (x3.7 gives Fraser's quoted 30-120 ms full decay). And the fall is
     * exponential IN HERTZ (f = base + df*e^(-t/tau)), not the geometric ramp
     * wa_param makes — geometric dives under the base and never holds the
     * long tail the F recording shows still 7 Hz sharp at 100 ms. */
    int         bd_sweep;      /* use the additive stock sweep       */
    float       bd_df;         /* current Hz above base              */
    float       bd_mult;       /* per-sample decay of bd_df          */
    float       bd_base;       /* this hit's base Hz (mods applied)  */
    /* The two mod pots (Fraser/Whittle), wired so P.Depth ZERO is the stock
     * kick bit for bit and Pitch is inert until P.Depth opens it:
     *   sweep height x(1 + 1.2*depth)  -> 2.2x stock at max (the 330K)
     *   base 49 x (1 + depth*(pitch-1)) -> full 0.43..4.7x only at full depth
     * pitch_mod is the multiplier itself (1.0 = stock). */
    float       pitch_mod;
    int         bd_phold;      /* samples before the sweep starts to
                                  fall — C9's charge time; Fraser's
                                  "fixed attack time of the sweep"    */
    float       hit_tune;      /* this hit's (drifted) base pitch    */
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
 * travel left should.
 *
 * The value is no longer eyeballed from three time points: 9W9's own snare is
 * rendered over a grid of settings and scored against the recording as a
 * ten-point trajectory (shell and noise band at 5/20/50/100/150 ms, in dB).
 * Best fit is 2.5 dB RMS, and it puts the noise where noon needs it.
 *
 * Re-fitted once the shell decay was corrected (tau 28 ms): the optimum moved
 * to 0.45 of the pot, so noon was about 10% hot. 1.95 puts the best fit back
 * at noon. For reference, Roland's own TR-909 plugin rendered from Gus's saved
 * preset is quieter still on the noise leg (0.12 against his hardware's 0.18
 * at 5 ms) — its Snappy was evidently set below noon, so the hardware stays
 * the reference. */
#define ER99_SNAPPY_MIX 1.95f

/* Turn-on voltage of the single-transistor VCAs. Fitted, not assumed: at 0.027
 * the snare's shells were cut off entirely by 100 ms where the real drum still
 * has them at -30 dB and audible to 150. Fitted again once the shell decay was
 * measured directly (tau 28 ms, not 36): at 0.005 the tail still gated off
 * before 150 ms where the references are at -39 dB and running to -46 at
 * 200 ms. Overridable so the fitting rig can sweep it (-DER99_VCA_VT=...).
 *
 * 0.0002 after fitting the shells against Roland's own TR-909 with Snappy at
 * zero — the cleanest shell reference there is. At 0.001 the tail still cut
 * short of theirs (0.005 against 0.015 at 150 ms, both around -40 dB, so this
 * is tidiness rather than anything audible). */
#ifndef ER99_VCA_VT
#define ER99_VCA_VT    0.0002f
#endif
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
     * makes Drive 0 genuinely transparent for every type.
     *
     * These three are ours, not the 909's, and they were all SYMMETRIC —
     * measured at 1.2% even harmonics against up to 39% odd. Odd-only
     * distortion is the hollow, brittle sound Gus called thin; the even
     * harmonics that make an overdriven amp sound full come from asymmetry.
     * Diode (case 0) is deliberately left alone: it is the 909's own
     * back-to-back diode pair, symmetric by nature, and every voice in the
     * module is fitted through it. */
    case 1: {   /* asymmetric soft clip — amp-like, even harmonics and all */
        /* One rail saturates sooner than the other, as a single-ended stage
         * does. Offset by the bias' own output so silence stays silence and
         * no DC reaches the mix. */
        /* The bias is added AFTER the drive gain, not scaled by it: biasing
         * the input meant that at high drive one rail saturated completely
         * and the stage turned into a rectifier with 30x gain. Here the two
         * rails stay 1 : 1.5 apart at any drive, which is a single-ended
         * stage's asymmetry rather than a broken one. 0.958 sets unity
         * small-signal gain, so Drive 0 is still transparent. */
        const float bias = 0.35f, tb = 0.33638f;   /* tanhf(0.35f) */
        const float v = (tanhf(_x * k + bias) - tb) * 0.958f;
        return k < 1.0f ? v / k : v;
    }
    case 2: {   /* wavefolder — metallic, without hollowing the note out */
        float v = _x * k;
        for(int i=0; i<3; ++i)
        {
            if(v >  1.0f) v =  2.0f - v;
            if(v < -1.0f) v = -2.0f - v;
        }
        /* Folding EATS the fundamental — measured 7455 down to 3387 at high
         * drive, which is why it sounded thin rather than metallic. Keep a
         * floor of the clipped signal underneath so the note survives its own
         * harmonics. */
        float body = _x * k;
        if(body >  1.0f) body =  1.0f;
        if(body < -1.0f) body = -1.0f;
        v = 0.62f * v + 0.38f * body;
        return k < 1.0f ? v / k : v;
    }
    case 3: {   /* bitcrush — lo-fi grit */
        /* Was 2 + 30/k: even wide open that is ~6 levels, and measured under
         * 4% THD, so the setting did almost nothing. Bites now. */
        const float steps = 1.5f + 9.0f / k;
        return floorf(_x * steps + 0.5f) / steps;
    }
    case 0:
    default:
        return er99_diode_round(_x, k);
    }
}

/* The measured resting pitch of the stock kick — every Tune position on the
 * reference machine settles here. */
#define ER99_BD_BASE_HZ 49.0f
/* Fitted against all four lettered Tune positions at once (24 windowed pitch
 * readings, total squared error 26 — about 1 Hz per point): the sweep rises
 * 4.6 Hz per ms of its time constant, and HOLDS for ~16 ms before falling —
 * C9's charge time, Fraser's "fixed attack time of the sweep". Without the
 * hold no exponential matches the recordings; the early readings sit far
 * above any curve that starts falling at t=0. */
#ifndef ER99_BD_DF_PER_MS
#define ER99_BD_DF_PER_MS 4.6f     /* sweep height per ms of tau */
#endif
#ifndef ER99_BD_PHOLD_MS
#define ER99_BD_PHOLD_MS  16.0f    /* sweep hold before the fall */
#endif

static inline void er99_bt_init(er99_bt_t *v, const float _sr)
{
    v->sample_rate = _sr;
    wa_osc_init(&v->osc,  _sr);
    wa_osc_init(&v->osc2, _sr);
    /* DC blocker: removes the offset the asymmetric diode rounding adds. */
    wa_biquad_set(&v->dc_block, WA_HIGHPASS, 20.0f, 0.7071f, 0.0f, _sr);
    v->hit_tune = v->tune;
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
    v->amp_hold_left = 0;
    v->bd_sweep = 0; v->bd_df = 0.0f; v->bd_mult = 0.0f; v->bd_phold = 0;
    v->bd_base = ER99_BD_BASE_HZ;
    if(v->pitch_mod <= 0.0f) v->pitch_mod = 1.0f;
    v->out_gain = 0.0f;
    v->impulse = 0;
    v->mute_countdown = 0.0;
}

static inline void er99_bt_trigger(er99_bt_t *v, const float _accent)
{
    const float sr = v->sample_rate;
    const float ms = 0.001f * sr;

    /* Every hit starts at the same point in the cycle — shock excitation, see
     * the history in the repo: free-running phase made every hit land on a
     * different attack and perceived pitch. 0.25 is the triangle's rising zero
     * crossing. The tube stage's DC blocker state is cleared for the same
     * reason. */
    wa_osc_set_phase(&v->osc,  0.25);
    wa_osc_set_phase(&v->osc2, 0.25);
    wa_biquad_reset(&v->dc_block);

    v->hit_tune = v->tune;

    if(v->bd_sweep)
    {
        /* v->tune carries the Tune pot as the sweep time constant in ms
         * (7..33); height follows at the measured 4.1 Hz/ms. */
        const float tau = v->tune < 6.0f ? 6.0f : (v->tune > 32.0f ? 32.0f : v->tune);
        const float m  = v->sweep_depth < 0.0f ? 0.0f
                       : (v->sweep_depth > 1.0f ? 1.0f : v->sweep_depth);
        const float pm = v->pitch_mod > 0.0f ? v->pitch_mod : 1.0f;
        v->bd_base  = ER99_BD_BASE_HZ * (1.0f + m * (pm - 1.0f));
        v->bd_df    = ER99_BD_DF_PER_MS * tau * (1.0f + 1.2f * m);
        v->bd_mult  = expf(-1.0f / (tau * ms));
        v->bd_phold = (int)(ER99_BD_PHOLD_MS * ms);
        wa_set_value(&v->pitch, v->bd_base);
    }
    else
    {
        /* Pitch: start high, sweep down to (drifted) Tune. */
        wa_set_value(&v->pitch, v->hit_tune * v->sweep_depth);
        wa_exp_ramp(&v->pitch, v->hit_tune, v->sweep_time * ms);
    }

    /* Amplitude: the analog envelope is an RC discharge — exponential, after
     * the hold above (see amp_hold). */
    wa_set_value(&v->amp, 1.0f);
    v->amp_hold_left = (int)(v->amp_hold * ms);
    if(v->amp_hold_left <= 0)
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
    v->out_gain = v->level * _accent;

    const float longest = v->decay > v->noise_decay ? v->decay : v->noise_decay;
    v->mute_countdown = (longest + 20.0f) * ms;
}

static inline float er99_bt_render(er99_bt_t *v, const float _noise)
{
    if(v->mute_countdown <= 0.0) return 0.0f;
    v->mute_countdown -= 1.0;

    float f = wa_param_tick(&v->pitch);
    if(v->bd_sweep)
    {
        f = v->bd_base + v->bd_df;
        if(v->bd_phold > 0) --v->bd_phold;
        else v->bd_df *= v->bd_mult;
    }

    if(v->amp_hold_left > 0 && --v->amp_hold_left == 0)
        wa_exp_ramp(&v->amp, 0.00001f, v->decay * 0.001f * v->sample_rate);
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

    /* Oscillator: triangle, rounded toward sine by the diode pair. The pair
     * is ALWAYS in circuit (Whittle: conduction at ~0.5-0.6 V at the stock
     * drive level; the Drive mod only pushes it harder) — this rounding is
     * the voice's tone, not an effect. At Drive 0 the kick used to ship the
     * raw triangle, which is buzzy in a way no 909 ever is. */
    float o = wa_osc_triangle(&v->osc, f);
    if(!(v->tune2 > 0.0f && v->osc2_mix > 0.0f))
    {
        /* Fitted to the recordings' settled body: H3/H1 4-5% (k 2.0 gave 7%)
         * and H2/H1 2-4% — the pair conducts slightly asymmetrically, and a
         * perfectly symmetric shaper produces none. The square term's DC goes
         * through the blocker below. */
        o = er99_diode_round(o, 1.6f);
        o += 0.05f * o * o;
        o = wa_biquad_tick(&v->dc_block, o);
    }
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
