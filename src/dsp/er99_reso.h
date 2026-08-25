/*
 * er99_reso.h — the TR-909's bridged-T drum voice, as a difference equation.
 *
 * The 909's snare shells and tom bodies are not oscillators. Each is a
 * bridged-T network in the feedback path of a transistor stage: a tuned,
 * lightly-damped second-order system that sits silent until the trigger
 * circuit hits it with a short pulse, then rings and dies away. "Pitch" is
 * where it resonates; "decay" is how heavily it is damped.
 *
 * A second-order resonant network shock-excited from rest produces
 *
 *      y(t) = A e^(-t/tau) sin(2 pi f0 t)
 *
 * and the digital form of that same system is a two-pole filter with its poles
 * at r*e^(+-j w0):
 *
 *      y[n] = 2 r cos(w0) y[n-1] - r^2 y[n-2] + x[n]
 *      r = e^(-1/(tau fs))     w0 = 2 pi f0 / fs
 *
 * So this is not an imitation of the bridged-T's sound — for the linear part
 * it is the same system, written as a recurrence instead of as an RC network.
 * What it is NOT is a component-level model: the pole frequency and damping
 * here come from the panel controls and from tuning by ear, not from the
 * schematic's resistor and capacitor values.
 *
 * Two behaviours fall out of the topology that an oscillator-plus-envelope
 * voice has to fake:
 *
 *   - Retriggering adds a pulse to a filter that is still ringing, exactly as
 *     the hardware does. Fast rolls and flams interact instead of restarting,
 *     with no reset heuristic anywhere.
 *   - Saturation inside the loop makes damping amplitude-dependent, so a hard
 *     hit decays differently from a soft one. That is the transistor's own
 *     behaviour, and much of the 909's "thwack".
 *
 * GPL-3.0.
 */
#ifndef ER99_RESO_H
#define ER99_RESO_H

#include <math.h>

/* Coefficients are refreshed on this stride while the pitch sweeps. The sweep
 * is slow next to 0.36 ms, and a per-sample cosf() is not worth its cost on a
 * Cortex-A72 that also has ten other voices to render. */
#define ER99_RESO_STRIDE 16

typedef struct {
    float y1, y2;       /* delay line                                   */
    float a1, a2;       /* 2 r cos(w0), -r^2                            */
    float gain;         /* input scale: sin(w0), keeps peak near unity  */
    float sat;          /* in-loop saturation drive (0 = linear)        */
    int   countdown;    /* samples until the next coefficient refresh   */
    float sample_rate;
} er99_reso_t;

static inline void er99_reso_init(er99_reso_t *r, const float _sr)
{
    r->y1 = r->y2 = 0.0f;
    r->a1 = 0.0f; r->a2 = 0.0f; r->gain = 0.0f;
    r->sat = 0.0f;
    r->countdown = 0;
    r->sample_rate = _sr > 0.0f ? _sr : 44100.0f;
}

/*
 * Place the poles. `_decay_ms` is the time to 1/e of the ring, which is what
 * the damping resistor sets in the circuit and what the Decay pot reads as.
 */
static inline void er99_reso_set(er99_reso_t *r, float _freq_hz, float _decay_ms)
{
    const float sr = r->sample_rate;
    /* Keep the pole below Nyquist: a sweep that starts high must not wrap
     * around and come back as a descending alias. */
    const float max_hz = sr * 0.45f;
    if(_freq_hz < 10.0f)   _freq_hz = 10.0f;
    if(_freq_hz > max_hz)  _freq_hz = max_hz;
    if(_decay_ms < 1.0f)   _decay_ms = 1.0f;

    const float w = 6.2831853f * _freq_hz / sr;
    const float tau_samples = _decay_ms * 0.001f * sr;
    float rad = expf(-1.0f / tau_samples);
    /* r == 1 is a pole ON the unit circle: an undamped ring that never stops
     * and, with rounding, wanders. Real networks always lose something. */
    if(rad > 0.99995f) rad = 0.99995f;

    r->a1   = 2.0f * rad * cosf(w);
    r->a2   = -rad * rad;
    /* Impulse response of this pair is r^n sin((n+1)w)/sin(w), so scaling the
     * input by sin(w) normalises the ring to about unit amplitude regardless
     * of where the voice is tuned. */
    r->gain = sinf(w);
}

/* Refresh coefficients on the stride only; call every sample with the current
 * swept frequency and let it decide. */
static inline void er99_reso_track(er99_reso_t *r, const float _freq_hz,
                                   const float _decay_ms)
{
    if(--r->countdown > 0) return;
    r->countdown = ER99_RESO_STRIDE;
    er99_reso_set(r, _freq_hz, _decay_ms);
}

/*
 * One sample. `_x` is the excitation — the trigger pulse — NOT a reset: energy
 * adds to whatever is still ringing, which is the whole point of the topology.
 */
static inline float er99_reso_tick(er99_reso_t *r, const float _x)
{
    float y = r->a1 * r->y1 + r->a2 * r->y2 + _x * r->gain;

    /* The transistor cannot swing past its rails, so a loud ring is clipped
     * and loses energy faster than a quiet one. Applied to the state that
     * feeds back, not to the output, so the damping really is amplitude
     * dependent rather than a shaper hung on the end. */
    if(r->sat > 0.0f)
    {
        const float k = 1.0f + r->sat * 3.0f;
        y = tanhf(y * k) / k;
    }

    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* Energy still in the network, for deciding when a voice may stop rendering. */
static inline float er99_reso_energy(const er99_reso_t *r)
{
    const float a = r->y1 < 0.0f ? -r->y1 : r->y1;
    const float b = r->y2 < 0.0f ? -r->y2 : r->y2;
    return a > b ? a : b;
}

/*
 * The trigger pulse. The 909's is a short shaped kick of current, not a single
 * sample: a one-sample impulse is all treble and lands as a click, while the
 * real pulse has width and therefore favours the lower partials the network
 * actually rings at.
 */
typedef struct {
    float level;        /* current pulse amplitude   */
    float decay;        /* per-sample multiplier     */
} er99_pulse_t;

static inline void er99_pulse_fire(er99_pulse_t *p, const float _amp,
                                   const float _ms, const float _sr)
{
    const float ms = _ms > 0.05f ? _ms : 0.05f;
    p->level = _amp;
    p->decay = expf(-1.0f / (ms * 0.001f * _sr));
}

static inline float er99_pulse_tick(er99_pulse_t *p)
{
    const float v = p->level;
    if(v == 0.0f) return 0.0f;
    p->level *= p->decay;
    if(p->level < 1.0e-5f) p->level = 0.0f;
    return v;
}

#endif /* ER99_RESO_H */
