/*
 * er99_engine.c — ER-99 drum voices.
 *
 * Signal paths follow matthewcieplak/er-99 exactly (src/instrument.ts,
 * generator.ts, sampler.ts, index.ts). Where the original has a quirk, the
 * quirk is reproduced and commented rather than silently "fixed" — fidelity
 * first; deviations are opt-in flags.
 *
 * GPL-3.0.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "er99_engine.h"
#include "rim_noise.h"

const char *const er99_trigger_names[ER99_NUM_TRIGGERS] = {
    "bd", "sd", "lt", "mt", "ht", "rs", "hc", "ohh", "chh", "rc", "cr"
};

static inline float ms_to_samples(const float _ms, const float _sr)
{
    return _ms * 0.001f * _sr;
}

/* ===================================================================== */
/* WAV loading (mono 16/24-bit PCM — that's what er-99 ships)            */
/* ===================================================================== */

static float *load_wav_mono(const char *_path, uint32_t *_out_len)
{
    *_out_len = 0;
    FILE *f = fopen(_path, "rb");
    if(!f) return NULL;

    unsigned char hdr[12];
    if(fread(hdr, 1, 12, f) != 12 ||
       memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
    { fclose(f); return NULL; }

    uint16_t channels = 1, bits = 16;
    float *out = NULL;

    for(;;)
    {
        unsigned char ch[8];
        if(fread(ch, 1, 8, f) != 8) break;
        const uint32_t size = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                              ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

        if(!memcmp(ch, "fmt ", 4))
        {
            unsigned char fmt[16];
            const uint32_t want = size < 16 ? size : 16;
            if(fread(fmt, 1, want, f) != want) break;
            channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            bits     = (uint16_t)(fmt[14] | (fmt[15] << 8));
            if(size > want) fseek(f, (long)(size - want), SEEK_CUR);
        }
        else if(!memcmp(ch, "data", 4))
        {
            const uint32_t bytes_per = (uint32_t)(bits / 8) * channels;
            if(bytes_per == 0) break;
            const uint32_t frames = size / bytes_per;
            unsigned char *raw = (unsigned char*)malloc(size);
            if(!raw) break;
            if(fread(raw, 1, size, f) != size) { free(raw); break; }

            out = (float*)calloc(frames ? frames : 1, sizeof(float));
            if(!out) { free(raw); break; }

            for(uint32_t i=0; i<frames; ++i)
            {
                const unsigned char *p = raw + (size_t)i * bytes_per; /* ch 0 */
                int32_t v = 0;
                if(bits == 16)
                    v = (int16_t)(p[0] | (p[1] << 8));
                else if(bits == 24)
                    v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                                  (uint32_t)p[2] << 24) >> 8;
                else if(bits == 32)
                    v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                  (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
                const float scale = bits == 16 ? 32768.0f : 8388608.0f;
                out[i] = (float)v / (bits == 32 ? 2147483648.0f : scale);
            }
            free(raw);
            *_out_len = frames;
            break;
        }
        else
        {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);
        }
    }

    fclose(f);
    return out;
}

/* ===================================================================== */
/* Init                                                                   */
/* ===================================================================== */

static void instrument_defaults(er99_instrument_t *v, const int _index)
{
    memset(v, 0, sizeof(*v));
    v->filter_type = -1;
    v->drive       = 1.0f;
    v->osc2_level  = 0.0f;   /* 0 = stock er-99 behaviour */
    v->filter_q    = 1.0f;
    v->noise_lp    = 0.0f;

    switch(_index)
    {
    case 0: /* Bass Drum */
        v->frequency = 80.0f;  v->decay = 300.0f; v->tone = 0.5f;
        v->tone_decay = 20.0f; v->volume = 0.5f;  v->env_amount = 2.5f;
        v->env_duration = 50.0f; v->saturation = 0.5f;
        v->filter_type = WA_LOWPASS; v->filter_freq = 3000.0f;
        break;
    case 1: /* Snare Drum */
        v->frequency = 220.0f; v->decay = 100.0f; v->tone = 0.25f;
        v->tone_decay = 250.0f; v->volume = 0.5f; v->env_amount = 4.0f;
        v->env_duration = 10.0f;
        v->filter_type = WA_NOTCH; v->filter_freq = 1000.0f;
        break;
    case 2: /* Low Tom  — the only voice with offset > 0, so the only osc2 */
        v->frequency = 100.0f; v->offset = 100.0f; v->decay = 200.0f;
        v->tone = 0.05f; v->tone_decay = 100.0f; v->volume = 0.5f;
        v->env_amount = 2.0f; v->env_duration = 100.0f;
        break;
    case 3: /* Med Tom — offset is negative, so er-99 creates no osc2 */
        v->frequency = 200.0f; v->offset = -50.0f; v->decay = 200.0f;
        v->tone = 0.05f; v->tone_decay = 100.0f; v->volume = 0.5f;
        v->env_amount = 2.0f; v->env_duration = 100.0f;
        break;
    case 4: /* Hi Tom */
        v->frequency = 300.0f; v->offset = -80.0f; v->decay = 200.0f;
        v->tone = 0.05f; v->tone_decay = 100.0f; v->volume = 0.5f;
        v->env_amount = 2.0f; v->env_duration = 100.0f;
        break;
    default: break;
    }
}

void er99_engine_init(er99_engine_t *e, const float _sr, const char *_module_dir)
{
    memset(e, 0, sizeof(*e));
    e->sample_rate = _sr;
    wa_noise_init(&e->noise, 0xC0FFEEu);

    for(int i=0; i<ER99_NUM_INSTRUMENTS; ++i)
    {
        er99_instrument_t *v = &e->inst[i];
        instrument_defaults(v, i);
        wa_osc_init(&v->osc,  _sr);
        wa_osc_init(&v->osc2, _sr);
        wa_param_init(&v->pitch, v->frequency);
        wa_param_init(&v->noise_gain, 0.0f);
        wa_param_init(&v->amp, 0.0f);
        v->has_filter = v->filter_type >= 0;
        if(v->has_filter)
            wa_biquad_set(&v->filter, (wa_filter_type_t)v->filter_type,
                          v->filter_freq, v->filter_q, 0.0f, _sr);
        wa_biquad_set(&v->noise_filter, WA_LOWPASS, 8000.0f, 0.7071f, 0.0f, _sr);
        /* instrument.ts uses makeDistortionCurve(2) */
        wa_shaper_init(&v->shaper, 2.0f, _sr);
    }

    /* ---- Circuit-informed BD / SD / toms ----
     * Frequencies follow the 909's ranges; sweep depth and click balance are
     * set so the kick has its characteristic beater attack and the toms have
     * real body rather than a bare triangle. */
    e->circuit_model = 1;
    for(int i=0; i<ER99_NUM_INSTRUMENTS; ++i)
    {
        er99_bt_t *b = &e->bt[i];
        memset(b, 0, sizeof(*b));
        b->drive = 2.0f;  b->level = 0.8f;  b->click_tone = 3000.0f;
        switch(i)
        {
        case 0: /* Bass Drum */
            b->tune = 55.0f;  b->sweep_depth = 3.2f; b->sweep_time = 22.0f;
            b->decay = 190.0f; b->attack = 0.35f; b->click_tone = 2500.0f;
            b->drive = 2.2f;  b->level = 1.0f;
            b->sub = 0.35f; b->tube = 1.2f;
            /* Drift OFF by default. Per-hit pitch/level jitter is a real
             * analog behaviour and the knob is still there for anyone who
             * wants it, but a kick that lands on a different pitch every bar
             * is the single most complained-about thing about this engine —
             * a drum machine should repeat exactly unless asked not to. */
            b->drift = 0.0f;
            break;
        /* Snare fitted to real 909 recordings (SD Clean E 01/03/10 — the same
         * drum at three Snappy settings): shells at 205 and 325 Hz (ratio
         * 1.585, NOT an octave), the second at ~28% of the first, shell t60
         * about 0.19 s, pitch glide only ~2.5% and fast, and the NOISE rings
         * longer than the shells (t60 0.34 s with Snappy up) — the shells are
         * the front of the sound, the noise is the tail. */
        case 1: /* Snare Drum: two tuned shells + snappy noise */
            b->tune = 205.0f; b->tune2 = 325.0f; b->osc2_mix = 0.40f;
            b->sweep_depth = 1.045f; b->sweep_time = 30.0f;
            b->decay = 320.0f; b->attack = 0.15f;
            b->snappy = 0.5f; b->noise_decay = 480.0f; b->noise_hp = 1000.0f;
            b->drive = 1.8f;  b->level = 0.7f;
            break;
        /* Tom tuning, glide and decay are fitted to measurements of a real
         * 909 (Tom Lo/Mid/Hi Clean): f0 68.5 / 102 / 131.5 Hz, pitch gliding
         * down ~10% over 200 ms (71 -> 64.5 on the low tom), t60 about
         * 1.0 / 0.64 / 0.66 s. The old 2.2x sweep was a synth swoop the
         * hardware does not make. */
        case 2: /* Low Tom */
            b->tune = 68.0f;
            b->sweep_depth = 1.12f; b->sweep_time = 200.0f;
            b->decay = 1700.0f; b->attack = 0.18f;
            b->level = 0.8f;
            break;
        case 3: /* Med Tom */
            b->tune = 102.0f;
            b->sweep_depth = 1.12f; b->sweep_time = 180.0f;
            b->decay = 1050.0f; b->attack = 0.18f;
            b->level = 0.8f;
            break;
        case 4: /* Hi Tom */
            b->tune = 132.0f;
            b->sweep_depth = 1.12f; b->sweep_time = 160.0f;
            b->decay = 1100.0f; b->attack = 0.18f;
            b->level = 0.8f;
            break;
        default: break;
        }
        er99_bt_init(b, _sr);
    }
    for(int i=0; i<3; ++i) er99_tom909_init(&e->tom909[i], _sr);

    /* ---- Rim shot (generator.ts / instruments.ts) ---- */
    er99_rim_t *r = &e->rim;
    r->decay = 30.0f; r->volume = 3.0f; r->saturation = 3.0f;
    r->hipass_freq = 100.0f;
    r->filter_freqs[0] = 220.0f; r->filter_freqs[1] = 500.0f; r->filter_freqs[2] = 950.0f;
    r->filter_qs[0] = r->filter_qs[1] = r->filter_qs[2] = 10.5f;
    r->all_bands = 0;   /* stock er-99 behaviour — see render loop */
    for(int i=0; i<3; ++i)
        wa_biquad_set(&r->bp[i], WA_BANDPASS, r->filter_freqs[i], r->filter_qs[i], 0.0f, _sr);
    wa_biquad_set(&r->hipass, WA_HIGHPASS, r->hipass_freq, 1.0f, 0.0f, _sr);
    wa_shaper_init(&r->shaper, 20.0f, _sr);   /* makeDistortionCurve(20) */
    wa_param_init(&r->noise_gain, 0.0f);
    r->buf_pos = ER99_RIM_NOISE_LEN;          /* idle */

    /* ---- Hand clap ---- */
    er99_clap_t *c = &e->clap;
    c->decay = 80.0f; c->delay_const = 10.0f; c->volume = 1.5f;
    c->tune = 1000.0f;      /* unused by stock graph, kept for parity */
    c->tone_decay = 250.0f;
    c->hipass_freq = 80.0f;
    c->filter_freqs[0] = 900.0f;  c->filter_qs[0] = 1.2f;
    c->filter_freqs[1] = 1200.0f; c->filter_qs[1] = 0.7f;
    wa_biquad_set(&c->tone_bp, WA_BANDPASS, 2200.0f, 2.0f, 0.0f, _sr); /* tone: 2200 */
    wa_biquad_set(&c->hp, WA_HIGHPASS, c->filter_freqs[0], c->filter_qs[0], 0.0f, _sr);
    wa_biquad_set(&c->bp, WA_BANDPASS, c->filter_freqs[1], c->filter_qs[1], 0.0f, _sr);
    wa_biquad_set(&c->hipass, WA_HIGHPASS, c->hipass_freq, 1.0f, 0.0f, _sr);
    wa_osc_init(&c->modulator, _sr);
    wa_param_init(&c->noise_gain, 0.0f);
    wa_param_init(&c->delay_out, 0.0f);
    c->tap_index = 5;   /* idle */

    /* ---- Circuit-informed rim + clap (default engine) ---- */
    {
        er99_rim909_t *r9 = &e->rim909;
        memset(r9, 0, sizeof(*r9));
        /* Measured on a real 909 rim: the two resonances are 210 and 480 Hz —
         * a ratio of 2.3, not the near-decade this had. The 1350 Hz "tick" was
         * invented; the real edge lives at 480 with its own harmonics above.
         * Second resonance sits at about half the first (56 vs 29 at 2 ms),
         * and the whole thing is down 30 dB by 30 ms — the shortest voice in
         * the machine. */
        r9->tune = 210.0f; r9->tune2 = 480.0f; r9->res = 10.0f;
        /* decay is the envelope's time to -100 dB, so -30 dB (where the real
         * rim lands at 30 ms) arrives at roughly an eighth of it. Drive down
         * from 2.4: the shaper was distorting the pair hard enough to invert
         * their balance — 480 Hz came out 2.5x the 210 where the hardware has
         * it at half. Distortion stays available on the knob. */
        r9->decay = 300.0f; r9->noise_mix = 0.35f;
        r9->drive = 1.4f;  r9->dist_type = 0.0f; r9->level = 0.58f;
        er99_rim909_init(r9, _sr);

        er99_clap909_t *c9 = &e->clap909;
        memset(c9, 0, sizeof(*c9));
        c9->tune = 1050.0f; c9->res = 2.4f;
        c9->spread = 9.0f;  c9->burst_decay = 7.0f;
        c9->tail_decay = 210.0f; c9->tail_level = 0.42f;
        c9->drive = 1.6f;  c9->dist_type = 0.0f; c9->level = 1.3f;
        er99_clap909_init(c9, _sr);
    }

    /* ---- Samplers ---- */
    static const float vols[ER99_NUM_SAMPLERS]   = { 0.5f, 0.2f, 0.3f, 0.5f };
    static const float sdec[ER99_NUM_SAMPLERS]  = { 450.0f, 1800.0f, 1800.0f, 110.0f };
    /* The closed hat plays the open hat's buffer — same cymbals, shorter gate. */
    static const char *files[ER99_NUM_SAMPLERS]  = { "hh.wav", "ride.wav", "crash.wav", NULL };
    for(int i=0; i<ER99_NUM_SAMPLERS; ++i)
    {
        er99_sampler_t *s = &e->sampler[i];
        s->decay = 450.0f;      /* open hat; ride/crash overridden below */
        s->decay_closed = 110.0f;
        s->volume = vols[i];
        s->decay  = sdec[i];
        s->drive  = 0.2f;      /* min = transparent; turn up to distort */
        s->dist_type = 0.0f;
        s->pitch  = 1.0f;
        wa_param_init(&s->out, 0.0f);
        s->pos = -1.0;   /* idle */

        if(!files[i])
        {
            /* Shares the open hat's data: not loaded again, and not freed
             * through this index either (see er99_engine_free). */
            s->buffer = e->sample_data[ER99_SAMP_OHH];
            s->length = e->sample_len[ER99_SAMP_OHH];
        }
        else if(_module_dir)
        {
            char path[1024];
            snprintf(path, sizeof(path), "%s/samples/%s", _module_dir, files[i]);
            e->sample_data[i] = load_wav_mono(path, &e->sample_len[i]);
            s->buffer = e->sample_data[i];
            s->length = e->sample_len[i];
        }
    }

    /* ---- Master ---- */
    e->master.threshold_db = 0.0f;
    e->master.knee_db      = 10.0f;
    e->master.ratio        = 12.0f;
    e->master.attack_s     = 0.0f;
    e->master.release_s    = 0.25f;
    e->master.env_db       = 0.0f;
    e->master.makeup       = 1.0f;
    e->master.drive        = 2.0f;   /* amount when a mode is chosen */
    e->master.dist_mode    = 0.0f;   /* Off by default */
    e->master.volume       = 0.5f;   /* globalParams.volume  */
    e->master.accent       = 2.0f;   /* globalParams.globalAccent */

    /* Derive initial pot positions from the defaults above. */
    er99_engine_seed_pots(e);
}

void er99_engine_free(er99_engine_t *e)
{
    for(int i=0; i<ER99_NUM_SAMPLERS; ++i)
    {
        free(e->sample_data[i]);        /* NULL for the closed hat: borrowed */
        e->sample_data[i] = NULL;
        e->sampler[i].buffer = NULL;
    }
}

/* ===================================================================== */
/* Triggering                                                             */
/* ===================================================================== */

static void trigger_instrument(er99_engine_t *e, er99_instrument_t *v, const float _accent)
{
    const float sr = e->sample_rate;

    /* osc.frequency: setValueAtTime(f * env_amount) then expRamp(f, env_duration) */
    wa_set_value(&v->pitch, v->frequency * v->env_amount);
    wa_exp_ramp(&v->pitch, v->frequency, ms_to_samples(v->env_duration, sr));

    /* noiseInput.gain: setValueAtTime(tone) then expRamp(0.00001, tone_decay) */
    wa_set_value(&v->noise_gain, v->tone);
    wa_exp_ramp(&v->noise_gain, 0.00001f, ms_to_samples(v->tone_decay, sr));

    /* output.gain: volume * accent */
    v->out_gain = v->volume * _accent;

    /* input.gain: linearRamp(volume, +5ms) then expRamp(0.00001, +decay).
     * Note the exp ramp's end time is decay from the TRIGGER, not from the
     * end of the attack — so stage 2 runs for (decay - 5ms). */
    wa_linear_ramp(&v->amp, v->volume, ms_to_samples(5.0f, sr));
    v->amp_stage = 0;
    v->amp_decay_samples = ms_to_samples(v->decay - 5.0f > 1.0f ? v->decay - 5.0f : 1.0f, sr);

    /* index.ts mutes output after max(decay, tone_decay) */
    const float mute_ms = v->decay > v->tone_decay ? v->decay : v->tone_decay;
    v->mute_countdown = ms_to_samples(mute_ms, sr);
}

int er99_engine_set_raw(er99_engine_t *e, const char *key, const float value);

void er99_engine_trigger(er99_engine_t *e, const er99_trigger_t _which, const int _velocity)
{

    const float accent = _velocity >= ER99_ACCENT_VELOCITY ? e->master.accent : 1.0f;
    const float sr = e->sample_rate;

    switch(_which)
    {
    case ER99_BD: case ER99_SD: case ER99_LT: case ER99_MT: case ER99_HT:
        /* Everything on the snare that is not a panel pot is fixed circuitry:
         * the shell interval (one pitch CV, D-network ratio 1.585), the pitch
         * pulse (ENV1 — "the same for all SD notes"), the shell decay
         * (C70/C79 discharge) and the noise bandpass. Pinned here, at the
         * moment they matter, so no stale saved state can detune the pair,
         * re-arm the old 1.6x pitch swoop, or shorten the shells — the blob
         * still restores those fields, and this overrules it. */
        if(_which == ER99_SD)
        {
            er99_bt_t *sd = &e->bt[ER99_SD];
            sd->tune2       = sd->tune * 1.585f;
            sd->osc2_mix    = 0.40f;   /* ~28% of shell 1 at the output */
            sd->sweep_depth = 1.045f;  /* 206->198 Hz measured, not 8% */
            sd->sweep_time  = 30.0f;
            sd->decay       = 320.0f;
            sd->attack      = 0.0f;    /* the 909 SD has no click path */
            if(sd->noise_hp < 990.0f || sd->noise_hp > 1010.0f)
                er99_engine_set_raw(e, "sd_c_noise_hp", 1000.0f);
        }
        if(!e->circuit_model)
            trigger_instrument(e, &e->inst[_which], accent);
        /* The toms are their own circuit: three oscillators with separate
         * envelopes plus a noise attack (er99_tom909.h). They read the same
         * panel values as the two-oscillator voice, so nothing else changes. */
        else if(_which >= ER99_LT && _which <= ER99_HT)
            er99_tom909_trigger(&e->tom909[_which - ER99_LT], &e->bt[_which], accent);
        else
            er99_bt_trigger(&e->bt[_which], accent);
        break;

    case ER99_RS: {
        if(e->circuit_model)
        {
            /* Fixed circuitry, not pots: the upper resonance is the same
             * network as the lower (measured 480/210 = 2.286, so it tracks
             * Tune), both ring at Q ~10, and the trigger pulse carries a set
             * amount of noise. Pinned here so no saved state can drift them. */
            er99_rim909_t *r9 = &e->rim909;
            const float t2 = r9->tune * 2.286f, q = 10.0f;
            if(r9->tune2 != t2 || r9->res != q)
            { r9->tune2 = t2; r9->res = q; er99_rim909_retune(r9); }
            r9->noise_mix = 0.35f;   /* inside the 2-sample pulse only */
            r9->decay     = 300.0f;  /* -30 dB at ~30 ms, as measured */
            er99_rim909_trigger(r9, accent);
            break;
        }
        er99_rim_t *r = &e->rim;
        wa_set_value(&r->noise_gain, 1.0f);
        wa_exp_ramp(&r->noise_gain, 0.00001f, ms_to_samples(r->decay, sr));
        r->out_gain = r->volume * accent;
        r->buf_pos = 0;                        /* replay the fixed noise table */
        r->mute_countdown = ms_to_samples(r->decay, sr);
        break;
    }

    case ER99_HC: {
        if(e->circuit_model) { er99_clap909_trigger(&e->clap909, accent); break; }
        er99_clap_t *c = &e->clap;
        /* tone path: setValueAtTime(0.5), expRamp(0.001, tone_decay) */
        wa_set_value(&c->noise_gain, 0.5f);
        wa_exp_ramp(&c->noise_gain, 0.001f, ms_to_samples(c->tone_decay, sr));
        /* first of five taps; the rest are scheduled in the render loop */
        c->tap_index = 0;
        c->next_tap_samples = 0.0;
        c->out_gain = c->volume * accent;
        /* playNote(): decay*3 + delayConst*8 */
        c->mute_countdown = ms_to_samples(c->decay * 3.0f + c->delay_const * 8.0f, sr);
        break;
    }

    case ER99_OHH: case ER99_CHH: case ER99_RC: case ER99_CR: {
        const int idx = _which == ER99_OHH ? ER99_SAMP_OHH
                      : _which == ER99_CHH ? ER99_SAMP_CHH
                      : _which == ER99_RC  ? ER99_SAMP_RC : ER99_SAMP_CR;
        er99_sampler_t *s = &e->sampler[idx];

        /* One pair of cymbals: closing the pedal stops the open hat ringing,
         * and opening it cuts the closed sound short. Anything else lets two
         * hats sound at once, which the hardware cannot do. */
        if(idx == ER99_SAMP_OHH || idx == ER99_SAMP_CHH)
        {
            er99_sampler_t *other = &e->sampler[idx == ER99_SAMP_OHH
                                                ? ER99_SAMP_CHH : ER99_SAMP_OHH];
            /* A short fade, not a hard stop: cutting a ringing buffer mid-cycle
             * is a click. 3 ms is under a hat's own attack. */
            wa_exp_ramp(&other->out, 0.00001f, ms_to_samples(3.0f, sr));
            other->mute_countdown = ms_to_samples(6.0f, sr);
        }

        wa_set_value(&s->out, s->volume * accent);
        wa_exp_ramp(&s->out, 0.00001f, ms_to_samples(s->decay, sr));
        s->pos = 0.0;                          /* restart the buffer */
        s->mute_countdown = ms_to_samples(s->decay, sr);
        break;
    }

    default: break;
    }
}

/* ===================================================================== */
/* Per-voice rendering                                                    */
/* ===================================================================== */

static float render_instrument(er99_engine_t *e, er99_instrument_t *v, const float _noise)
{
    if(v->mute_countdown <= 0.0) return 0.0f;
    v->mute_countdown -= 1.0;

    const float freq = wa_param_tick(&v->pitch);

    /* Amp envelope: 5 ms linear attack, then exponential decay. */
    if(v->amp_stage == 0 && v->amp.type == WA_SEG_CONST)
    {
        v->amp_stage = 1;
        wa_exp_ramp(&v->amp, 0.00001f, v->amp_decay_samples);
    }
    const float amp   = wa_param_tick(&v->amp);
    const float ngain = wa_param_tick(&v->noise_gain);

    /* Oscillator path: osc -> [saturation -> waveshaper] -> input.gain */
    float osc = wa_osc_triangle(&v->osc, freq);
    if(v->saturation > 0.0f)
        osc = wa_shaper_tick(&v->shaper, osc * v->saturation * v->drive);
    else if(v->drive != 1.0f)
        osc *= v->drive;

    /* Noise path: whiteNoise (+ osc2 when offset > 0) -> noiseInput.gain */
    float noise_sum = _noise;
    if(v->noise_lp > 0.0f)
        noise_sum = wa_biquad_tick(&v->noise_filter, noise_sum);
    if(v->osc2_level <= 0.0f && v->offset > 0.0f)
        noise_sum += wa_osc_triangle(&v->osc2, freq);   /* stock: osc2 on the noise env */

    /* Improved second oscillator: |offset| detune, on the AMP envelope, so it
     * actually contributes body instead of vanishing with the noise burst. */
    float body = 0.0f;
    if(v->osc2_level > 0.0f)
    {
        const float det = v->offset < 0.0f ? -v->offset : v->offset;
        const float f2  = freq * ((v->frequency + det) / (v->frequency > 1.0f ? v->frequency : 1.0f));
        body = wa_osc_triangle(&v->osc2, f2) * v->osc2_level;
    }

    /* Both land on output.gain */
    float y = (osc + body) * amp + noise_sum * ngain;
    y *= v->out_gain;

    if(v->has_filter)
        y = wa_biquad_tick(&v->filter, y);

    return y;
}

static float render_rim(er99_rim_t *r)
{
    if(r->mute_countdown <= 0.0) return 0.0f;
    r->mute_countdown -= 1.0;

    /* The 200-sample table is played once per hit (256-frame buffer, rest 0). */
    float exc = 0.0f;
    if(r->buf_pos < ER99_RIM_NOISE_LEN)
        exc = er99_rim_noise[r->buf_pos];
    if(r->buf_pos < 256) r->buf_pos++;

    exc *= wa_param_tick(&r->noise_gain);

    /*
     * er-99 quirk: with filterTopology 'parallel', setupGenerator only wires
     * noiseInput into filterNodes[0]; nodes 1 and 2 are connected to the
     * output but nothing feeds them. So stock rim shot is the 220 Hz bandpass
     * alone. Reproduced by default; all_bands=1 drives all three, which is
     * closer to the real hardware's three-band ring.
     */
    float y = wa_biquad_tick(&r->bp[0], exc);
    if(r->all_bands)
        y += wa_biquad_tick(&r->bp[1], exc) + wa_biquad_tick(&r->bp[2], exc);

    y = wa_shaper_tick(&r->shaper, y * r->saturation);
    y = wa_biquad_tick(&r->hipass, y);
    return y * r->out_gain;
}

static float render_clap(er99_engine_t *e, er99_clap_t *c, const float _noise)
{
    if(c->mute_countdown <= 0.0) return 0.0f;
    c->mute_countdown -= 1.0;

    const float sr = e->sample_rate;

    /* Five amplitude taps, delay_const apart. There is no DelayNode in the
     * original: the "spread" is purely these scheduled gain bursts. */
    if(c->tap_index < 5)
    {
        if(c->next_tap_samples <= 0.0)
        {
            static const float tap_level[5] = { 0.1f, 0.8f, 0.5f, 0.3f, 0.2f };
            const float dc        = c->delay_const * 0.001f;          /* seconds */
            const float decay_val = c->decay / 250.0f * dc;
            const float tc = (c->tap_index == 0) ? (dc / 3.0f + decay_val)
                           : (c->tap_index <  4) ? (dc / 2.0f + decay_val)
                                                 : (dc / 2.0f + c->decay / 2500.0f);
            wa_set_value(&c->delay_out, tap_level[c->tap_index]);
            wa_set_target(&c->delay_out, 0.000001f, tc * sr);
            c->tap_index++;
            c->next_tap_samples = ms_to_samples(c->delay_const, sr);
        }
        c->next_tap_samples -= 1.0;
    }

    /* delayInput.gain = 1.0 + 0.4 * sawtooth(40 Hz) */
    const float mod = 1.0f + 0.4f * wa_osc_sawtooth(&c->modulator, 40.0f);

    const float burst = _noise * mod * wa_param_tick(&c->delay_out);
    float y = wa_biquad_tick(&c->bp, wa_biquad_tick(&c->hp, burst));

    /* tone path: whiteNoise -> bandpass(2200, Q2) -> noiseInput.gain, summed
     * straight into the saturation node (bypasses the HP/BP pair). */
    y += wa_biquad_tick(&c->tone_bp, _noise) * wa_param_tick(&c->noise_gain);

    y = wa_biquad_tick(&c->hipass, y);
    return y * c->out_gain;
}

static float render_sampler(er99_sampler_t *s)
{
    if(s->mute_countdown <= 0.0 || s->pos < 0.0 || !s->buffer) return 0.0f;
    s->mute_countdown -= 1.0;

    float v = 0.0f;
    if(s->pos < (double)s->length)
    {
        /* linear interpolation, matching playbackRate resampling */
        const uint32_t i = (uint32_t)s->pos;
        const float fr = (float)(s->pos - (double)i);
        const float a = s->buffer[i];
        const float b = (i + 1 < s->length) ? s->buffer[i + 1] : 0.0f;
        v = a + (b - a) * fr;
        s->pos += (double)(s->pitch > 0.0f ? s->pitch : 1.0f);
    }
    v *= wa_param_tick(&s->out);
    if(s->drive > 0.25f)                     /* below this the shape is inaudible */
        v = er99_shape(v * (1.0f + s->drive * 0.5f), s->drive, s->dist_type);
    return v;
}

/* DynamicsCompressorNode approximation (threshold 0 dB, knee 10, ratio 12). */
static float master_compress(er99_master_t *m, const float _in, const float _sr)
{
    const float mag = fabsf(_in);
    const float in_db = mag > 1e-9f ? 20.0f * log10f(mag) : -120.0f;

    float over = in_db - m->threshold_db;
    float gain_db = 0.0f;
    if(over > 0.0f)
    {
        if(over < m->knee_db)   /* soft knee */
            gain_db = -((1.0f - 1.0f / m->ratio) * over * over) / (2.0f * m->knee_db);
        else
            gain_db = -(over - over / m->ratio) + 0.0f;
    }

    /* attack is 0 -> instant onset; release 0.25 s one-pole recovery */
    const float rel = expf(-1.0f / (m->release_s * _sr));
    if(gain_db < m->env_db) m->env_db = gain_db;            /* attack 0 */
    else                    m->env_db = gain_db + (m->env_db - gain_db) * rel;

    return _in * powf(10.0f, m->env_db / 20.0f);
}

void er99_engine_render(er99_engine_t *e, float *out, const int frames)
{
    for(int n=0; n<frames; ++n)
    {
        const float noise = wa_noise_tick(&e->noise);

        float mix = 0.0f;
        for(int i=0; i<ER99_NUM_INSTRUMENTS; ++i)
            if(!e->circuit_model)
                mix += render_instrument(e, &e->inst[i], noise);
            else if(i >= ER99_LT && i <= ER99_HT)
                mix += er99_tom909_render(&e->tom909[i - ER99_LT], &e->bt[i], noise);
            else
                mix += er99_bt_render(&e->bt[i], noise);
        mix += e->circuit_model ? er99_rim909_render(&e->rim909, noise)
                                : render_rim(&e->rim);
        mix += e->circuit_model ? er99_clap909_render(&e->clap909, noise)
                                : render_clap(e, &e->clap, noise);
        for(int i=0; i<ER99_NUM_SAMPLERS; ++i)
            mix += render_sampler(&e->sampler[i]);

        if(e->master.dist_mode >= 0.5f)
            mix = er99_shape(mix * e->master.drive, e->master.drive,
                             e->master.dist_mode - 1.0f) * 0.7f;
        mix = master_compress(&e->master, mix, e->sample_rate);
        out[n] = mix * e->master.makeup * e->master.volume;
    }
}

/* ===================================================================== */
/* Parameters                                                             */
/* ===================================================================== */

typedef struct { const char *name; size_t off; } field_t;

/* Circuit-voice fields, shared by set_param/get_param and state save/load. */
typedef struct { const char *n; size_t off; } bt_field_t;
#define BTF(n) { #n, offsetof(er99_bt_t, n) }
static const bt_field_t g_bt_fields[] = {
    BTF(tune), BTF(sweep_depth), BTF(sweep_time), BTF(decay), BTF(attack),
    BTF(click_tone), BTF(drive), BTF(level), BTF(dist_type),
    BTF(tune2), BTF(osc2_mix), BTF(snappy), BTF(noise_decay), BTF(noise_hp),
    BTF(sub), BTF(tube), BTF(drift),
};
#define ER99_BT_FIELD_COUNT (sizeof(g_bt_fields)/sizeof(g_bt_fields[0]))

/* Everything else that must round-trip in a saved state. */
static const char *const g_other_state_keys[] = {
    "rs_decay", "rs_volume", "rs_saturation", "rs_all_bands",
    "hc_decay", "hc_spread", "hc_volume", "hc_tone_decay",
    "ohh_decay", "ohh_volume", "ohh_pitch",
    "chh_decay", "chh_volume", "chh_pitch", "chh_drive",
    "rc_decay", "rc_volume", "rc_pitch",
    "cr_decay", "cr_volume", "cr_pitch",
    "volume", "accent", "circuit_model", "master_dist",
    "rs_dist_type", "hc_dist_type", "ohh_dist_type", "chh_dist_type",
    "rc_dist_type", "cr_dist_type",
};
#define ER99_OTHER_KEY_COUNT (sizeof(g_other_state_keys)/sizeof(g_other_state_keys[0]))

#define IFIELD(n) { #n, offsetof(er99_instrument_t, n) }
static const field_t g_inst_fields[] = {
    IFIELD(frequency), IFIELD(decay), IFIELD(tone), IFIELD(tone_decay),
    IFIELD(volume), IFIELD(env_amount), IFIELD(env_duration),
    IFIELD(saturation), IFIELD(filter_freq), IFIELD(offset), IFIELD(drive),
    IFIELD(osc2_level), IFIELD(filter_q), IFIELD(noise_lp),
};

static er99_instrument_t *find_instrument(er99_engine_t *e, const char *_key,
                                          const char **_rest)
{
    for(int i=0; i<ER99_NUM_INSTRUMENTS; ++i)
    {
        const char *n = er99_trigger_names[i];
        const size_t len = strlen(n);
        if(!strncmp(_key, n, len) && _key[len] == '_')
        {
            /* "<v>_c_<field>" belongs to the circuit voice, not the stock one.
             * Without this guard find_instrument claims the key, fails to find
             * a stock field named "c_tune", and returns 0 — silently swallowing
             * every circuit control. */
            if(!strncmp(_key + len + 1, "c_", 2))
                return NULL;
            *_rest = _key + len + 1;
            return &e->inst[i];
        }
    }
    return NULL;
}

int er99_engine_set_raw(er99_engine_t *e, const char *key, const float value)
{
    const char *rest = NULL;
    er99_instrument_t *v = find_instrument(e, key, &rest);
    if(v)
    {
        for(size_t i=0; i<sizeof(g_inst_fields)/sizeof(g_inst_fields[0]); ++i)
        {
            if(!strcmp(rest, g_inst_fields[i].name))
            {
                *(float*)((char*)v + g_inst_fields[i].off) = value;
                if(v->has_filter)
                    wa_biquad_set(&v->filter, (wa_filter_type_t)v->filter_type,
                                  v->filter_freq, v->filter_q, 0.0f, e->sample_rate);
                if(v->noise_lp > 0.0f)
                    wa_biquad_set(&v->noise_filter, WA_LOWPASS, v->noise_lp,
                                  0.7071f, 0.0f, e->sample_rate);
                return 1;
            }
        }
        return 0;
    }

    if(!strcmp(key, "circuit_model")) { e->circuit_model = value > 0.5f; return 1; }
    {
        for(int i=0; i<ER99_NUM_INSTRUMENTS; ++i)
        {
            char pre[16];
            snprintf(pre, sizeof(pre), "%s_c_", er99_trigger_names[i]);
            const size_t plen = strlen(pre);
            if(strncmp(key, pre, plen)) continue;
            for(size_t k=0; k<ER99_BT_FIELD_COUNT; ++k)
            {
                if(strcmp(key + plen, g_bt_fields[k].n)) continue;
                *(float*)((char*)&e->bt[i] + g_bt_fields[k].off) = value;
                wa_biquad_set(&e->bt[i].click_lp, WA_LOWPASS,
                              e->bt[i].click_tone > 0.0f ? e->bt[i].click_tone : 3000.0f,
                              0.7071f, 0.0f, e->sample_rate);
                wa_biquad_set(&e->bt[i].noise_hpf, WA_HIGHPASS,
                              e->bt[i].noise_hp > 0.0f ? e->bt[i].noise_hp : 800.0f,
                              0.7071f, 0.0f, e->sample_rate);
                return 1;
            }
        }
    }
    if(e->circuit_model)
    {
        er99_rim909_t *r9 = &e->rim909; er99_clap909_t *c9 = &e->clap909;
        if(!strcmp(key,"rs_decay"))     { r9->decay = value; return 1; }
        if(!strcmp(key,"rs_volume"))    { r9->level = value; return 1; }
        if(!strcmp(key,"rs_saturation")){ r9->drive = value; return 1; }
        if(!strcmp(key,"rs_tune"))      { r9->tune = value;  er99_rim909_retune(r9); return 1; }
        if(!strcmp(key,"rs_tune2"))     { r9->tune2 = value; er99_rim909_retune(r9); return 1; }
        if(!strcmp(key,"rs_res"))       { r9->res = value;   er99_rim909_retune(r9); return 1; }
        if(!strcmp(key,"rs_noise"))     { r9->noise_mix = value; return 1; }
        if(!strcmp(key,"hc_decay"))     { c9->tail_decay = value; return 1; }
        if(!strcmp(key,"hc_spread"))    { c9->spread = value; return 1; }
        if(!strcmp(key,"hc_volume"))    { c9->level = value; return 1; }
        if(!strcmp(key,"hc_tone_decay")){ c9->burst_decay = value; return 1; }
        if(!strcmp(key,"hc_tune"))      { c9->tune = value; er99_clap909_retune(c9); return 1; }
        if(!strcmp(key,"hc_tail"))      { c9->tail_level = value; return 1; }
        if(!strcmp(key,"hc_drive"))     { c9->drive = value; return 1; }
    }
    if(!strcmp(key, "rs_decay"))      { e->rim.decay = value;  return 1; }
    if(!strcmp(key, "rs_volume"))     { e->rim.volume = value; return 1; }
    if(!strcmp(key, "rs_saturation")) { e->rim.saturation = value; return 1; }
    if(!strcmp(key, "rs_all_bands"))  { e->rim.all_bands = value > 0.5f; return 1; }
    if(!strcmp(key, "hc_decay"))      { e->clap.decay = value;  return 1; }
    if(!strcmp(key, "hc_spread"))     { e->clap.delay_const = value; return 1; }
    if(!strcmp(key, "hc_volume"))     { e->clap.volume = value; return 1; }
    if(!strcmp(key, "hc_tone_decay")) { e->clap.tone_decay = value; return 1; }

    /* Patches saved before the closed hat became its own voice carry its decay
     * as ohh_decay_closed. Keep accepting it. */
    if(!strcmp(key, "ohh_decay_closed")) { e->sampler[ER99_SAMP_CHH].decay = value; return 1; }

    static const char *sn[ER99_NUM_SAMPLERS] = { "ohh", "rc", "cr", "chh" };
    for(int i=0; i<ER99_NUM_SAMPLERS; ++i)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s_decay", sn[i]);
        if(!strcmp(key, buf)) { e->sampler[i].decay = value; return 1; }
        snprintf(buf, sizeof(buf), "%s_volume", sn[i]);
        if(!strcmp(key, buf)) { e->sampler[i].volume = value; return 1; }
        snprintf(buf, sizeof(buf), "%s_pitch", sn[i]);
        if(!strcmp(key, buf)) { e->sampler[i].pitch = value; return 1; }
        snprintf(buf, sizeof(buf), "%s_decay_closed", sn[i]);
        if(!strcmp(key, buf)) { e->sampler[i].decay_closed = value; return 1; }
        snprintf(buf, sizeof(buf), "%s_drive", sn[i]);
        if(!strcmp(key, buf)) { e->sampler[i].drive = value; return 1; }
        snprintf(buf, sizeof(buf), "%s_dist_type", sn[i]);
        if(!strcmp(key, buf)) { e->sampler[i].dist_type = value < 0 ? 0 : (value > 3 ? 3 : value); return 1; }
    }

    if(!strcmp(key, "rs_dist_type")) { e->rim909.dist_type = value < 0 ? 0 : (value > 3 ? 3 : value); return 1; }
    if(!strcmp(key, "hc_dist_type")) { e->clap909.dist_type = value < 0 ? 0 : (value > 3 ? 3 : value); return 1; }

    if(!strcmp(key, "master_drive")) { e->master.drive = value; return 1; }
    if(!strcmp(key, "master_dist"))  { e->master.dist_mode = value < 0 ? 0 : (value > 4 ? 4 : value); return 1; }
    if(!strcmp(key, "volume")) { e->master.volume = value; return 1; }
    if(!strcmp(key, "accent")) { e->master.accent = value; return 1; }
    return 0;
}

int er99_engine_get_raw(const er99_engine_t *e, const char *key, float *out)
{
    const char *rest = NULL;
    er99_instrument_t *v = find_instrument((er99_engine_t*)e, key, &rest);
    if(v)
    {
        for(size_t i=0; i<sizeof(g_inst_fields)/sizeof(g_inst_fields[0]); ++i)
            if(!strcmp(rest, g_inst_fields[i].name))
            { *out = *(const float*)((const char*)v + g_inst_fields[i].off); return 1; }
        return 0;
    }
    for(int i=0; i<ER99_NUM_INSTRUMENTS; ++i)
    {
        char pre[16];
        snprintf(pre, sizeof(pre), "%s_c_", er99_trigger_names[i]);
        const size_t plen = strlen(pre);
        if(strncmp(key, pre, plen)) continue;
        for(size_t k=0; k<ER99_BT_FIELD_COUNT; ++k)
            if(!strcmp(key + plen, g_bt_fields[k].n))
            { *out = *(const float*)((const char*)&e->bt[i] + g_bt_fields[k].off); return 1; }
    }
    if(!strcmp(key, "circuit_model")) { *out = (float)e->circuit_model; return 1; }
    if(!strcmp(key, "rs_saturation")) { *out = e->rim.saturation; return 1; }
    if(!strcmp(key, "rs_all_bands"))  { *out = (float)e->rim.all_bands; return 1; }
    if(!strcmp(key, "hc_spread"))     { *out = e->clap.delay_const; return 1; }
    if(!strcmp(key, "hc_volume"))     { *out = e->clap.volume; return 1; }
    if(!strcmp(key, "hc_tone_decay")) { *out = e->clap.tone_decay; return 1; }
    {
        if(!strcmp(key, "ohh_decay_closed")) { *out = e->sampler[ER99_SAMP_CHH].decay; return 1; }
        static const char *sn[ER99_NUM_SAMPLERS] = { "ohh", "rc", "cr", "chh" };
        for(int i=0; i<ER99_NUM_SAMPLERS; ++i)
        {
            char b[32];
            snprintf(b, sizeof(b), "%s_decay", sn[i]);
            if(!strcmp(key, b)) { *out = e->sampler[i].decay; return 1; }
            snprintf(b, sizeof(b), "%s_volume", sn[i]);
            if(!strcmp(key, b)) { *out = e->sampler[i].volume; return 1; }
            snprintf(b, sizeof(b), "%s_pitch", sn[i]);
            if(!strcmp(key, b)) { *out = e->sampler[i].pitch; return 1; }
            snprintf(b, sizeof(b), "%s_decay_closed", sn[i]);
            if(!strcmp(key, b)) { *out = e->sampler[i].decay_closed; return 1; }
            snprintf(b, sizeof(b), "%s_drive", sn[i]);
            if(!strcmp(key, b)) { *out = e->sampler[i].drive; return 1; }
            snprintf(b, sizeof(b), "%s_dist_type", sn[i]);
            if(!strcmp(key, b)) { *out = e->sampler[i].dist_type; return 1; }
        }
    }
    if(!strcmp(key, "rs_dist_type")) { *out = e->rim909.dist_type; return 1; }
    if(!strcmp(key, "hc_dist_type")) { *out = e->clap909.dist_type; return 1; }
    if(e->circuit_model)
    {
        const er99_rim909_t *r9 = &e->rim909; const er99_clap909_t *c9 = &e->clap909;
        if(!strcmp(key,"rs_decay"))     { *out = r9->decay; return 1; }
        if(!strcmp(key,"rs_volume"))    { *out = r9->level; return 1; }
        if(!strcmp(key,"rs_saturation")){ *out = r9->drive; return 1; }
        if(!strcmp(key,"rs_tune"))      { *out = r9->tune;  return 1; }
        if(!strcmp(key,"rs_tune2"))     { *out = r9->tune2; return 1; }
        if(!strcmp(key,"rs_res"))       { *out = r9->res;   return 1; }
        if(!strcmp(key,"rs_noise"))     { *out = r9->noise_mix; return 1; }
        if(!strcmp(key,"hc_decay"))     { *out = c9->tail_decay; return 1; }
        if(!strcmp(key,"hc_spread"))    { *out = c9->spread; return 1; }
        if(!strcmp(key,"hc_volume"))    { *out = c9->level; return 1; }
        if(!strcmp(key,"hc_tone_decay")){ *out = c9->burst_decay; return 1; }
        if(!strcmp(key,"hc_tune"))      { *out = c9->tune; return 1; }
        if(!strcmp(key,"hc_tail"))      { *out = c9->tail_level; return 1; }
        if(!strcmp(key,"hc_drive"))     { *out = c9->drive; return 1; }
    }
    if(!strcmp(key, "rs_decay"))  { *out = e->rim.decay;  return 1; }
    if(!strcmp(key, "rs_volume")) { *out = e->rim.volume; return 1; }
    if(!strcmp(key, "hc_decay"))  { *out = e->clap.decay; return 1; }
    if(!strcmp(key, "master_drive")) { *out = e->master.drive; return 1; }
    if(!strcmp(key, "master_dist"))  { *out = e->master.dist_mode; return 1; }
    if(!strcmp(key, "volume"))    { *out = e->master.volume; return 1; }
    if(!strcmp(key, "accent"))    { *out = e->master.accent; return 1; }
    return 0;
}


/* Keys that are switches/enums, not pots: passed through unscaled. */
static int is_enum_key(const char *key)
{
    if(!strcmp(key, "circuit_model") || !strcmp(key, "rs_all_bands") ||
       !strcmp(key, "master_dist")) return 1;
    const size_t n = strlen(key);
    return n > 10 && !strcmp(key + n - 10, "_dist_type");
}

/* ===================================================================== */
/* State — self-contained, so slot autosave and User Presets round-trip   */
/* ===================================================================== */

int er99_engine_get_state(const er99_engine_t *e, char *buf, const int len)
{
    if(!buf || len <= 0) return -1;
    int n = 0;
    /* v3: the voice-rebuild cut. v2 blobs carry pot positions whose meanings
     * and ranges changed under them (tom decay ranges, snare Tune centre, the
     * old octave tune2, the 1.6x sweep, osc2_mix 0.7) — restoring one
     * resurrects the pre-rebuild sound no matter how the defaults are set,
     * which is exactly what kept happening. Old blobs are discarded whole. */
    n += snprintf(buf + n, (size_t)(len - n), "er99v3;");

    /* Pot positions: what the panel shows, and exactly what restores it. */
    for(int i=0; i<ER99_POT_COUNT && n < len - 1; ++i)
        n += snprintf(buf + n, (size_t)(len - n), "%s=%d;",
                      g_er99_pots[i].key, (int)e->pots[i]);

    /* Switches keep their own values. */
    for(size_t k=0; k<ER99_OTHER_KEY_COUNT && n < len - 1; ++k)
    {
        if(!is_enum_key(g_other_state_keys[k])) continue;
        float v = 0.0f;
        if(er99_engine_get_raw(e, g_other_state_keys[k], &v))
            n += snprintf(buf + n, (size_t)(len - n), "%s=%.0f;", g_other_state_keys[k], v);
    }
    for(int i=0; i<ER99_NUM_INSTRUMENTS && n < len - 1; ++i)
        n += snprintf(buf + n, (size_t)(len - n), "%s_c_dist_type=%.0f;",
                      er99_trigger_names[i], e->bt[i].dist_type);

    return n < len ? n : len - 1;
}

int er99_engine_set_state(er99_engine_t *e, const char *blob)
{
    if(!blob) return 0;

    /*
     * v1 blobs stored RAW engineering values (volume=0.5, decay=190). Feeding
     * those to the pot-based setter reads 0.5 as pot 0 — master volume zero,
     * i.e. total silence. Anything that isn't v2 is discarded so the engine
     * keeps its (audible) defaults instead of being silently muted.
     */
    if(strncmp(blob, "er99v3;", 7) != 0)
        return 0;      /* v1/v2/unknown: keep the (correct) defaults */

    const char *p = blob;
    while(*p)
    {
        const char *semi = strchr(p, ';');
        const size_t seglen = semi ? (size_t)(semi - p) : strlen(p);
        if(seglen > 0 && seglen < 96)
        {
            char seg[96];
            memcpy(seg, p, seglen);
            seg[seglen] = '\0';
            char *eq = strchr(seg, '=');
            if(eq)
            {
                *eq = '\0';
                er99_engine_set_param(e, seg, (float)atof(eq + 1));
            }
        }
        if(!semi) break;
        p = semi + 1;
    }
    return 1;
}


/* ===================================================================== */
/* Pot layer — the external 0..127 parameter surface                      */
/* ===================================================================== */

/* Renames the pot layer honours, so a patch saved under an old key lands on the
 * control that replaced it — pot position included, not just the engine field.
 * ohh_decay_closed became chh_decay when the closed hat got its own voice. */
static const char *pot_alias(const char *key)
{
    if(!strcmp(key, "ohh_decay_closed")) return "chh_decay";
    return key;
}

static int pot_index(const char *key)
{
    key = pot_alias(key);
    for(int i=0; i<ER99_POT_COUNT; ++i)
        if(!strcmp(key, g_er99_pots[i].key)) return i;
    return -1;
}


void er99_engine_seed_pots(er99_engine_t *e)
{
    for(int i=0; i<ER99_POT_COUNT; ++i)
    {
        float v = 0.0f;
        if(er99_engine_get_raw(e, g_er99_pots[i].key, &v))
            e->pots[i] = (unsigned char)er99_value_to_pot(&g_er99_pots[i], v);
    }
}

int er99_engine_set_param(er99_engine_t *e, const char *key, const float pot)
{
    if(is_enum_key(key))
        return er99_engine_set_raw(e, key, pot);

    const int idx = pot_index(key);
    if(idx < 0)
        return er99_engine_set_raw(e, key, pot);   /* unknown: treat as raw */

    int p = (int)(pot + 0.5f);
    if(p < 0)   p = 0;
    if(p > 127) p = 127;
    e->pots[idx] = (unsigned char)p;
    return er99_engine_set_raw(e, key, er99_pot_to_value(&g_er99_pots[idx], p));
}

int er99_engine_get_param(const er99_engine_t *e, const char *key, float *pot)
{
    if(is_enum_key(key))
        return er99_engine_get_raw(e, key, pot);

    const int idx = pot_index(key);
    if(idx < 0)
        return er99_engine_get_raw(e, key, pot);

    *pot = (float)e->pots[idx];
    return 1;
}
