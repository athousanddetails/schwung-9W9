/*
 * render.c — offline renderer for the ER-99 C engine.
 *
 * Renders either one voice per file (for A/B against the Web Audio reference)
 * or a demo pattern. Runs anywhere; on Move it doubles as a smoke test.
 *
 *   er99_render <module_dir> <out_dir> [voice|all|pattern]
 *
 * GPL-3.0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "er99_engine.h"

static void wr32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void wr16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int write_wav(const char *path, const float *mono, int frames, int sr)
{
    FILE *f = fopen(path, "wb");
    if(!f) { fprintf(stderr, "cannot write %s\n", path); return 0; }
    const uint32_t bytes = (uint32_t)frames * 2;
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16);
    wr16(f, 1); wr16(f, 1); wr32(f, (uint32_t)sr);
    wr32(f, (uint32_t)sr * 2); wr16(f, 2); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, bytes);
    for(int i=0; i<frames; ++i)
    {
        float v = mono[i];
        if(v >  1.0f) v =  1.0f;
        if(v < -1.0f) v = -1.0f;
        const int16_t s = (int16_t)(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    if(argc < 3)
    {
        fprintf(stderr, "usage: %s <module_dir> <out_dir> [voice|all|pattern]\n", argv[0]);
        return 1;
    }
    const char *module_dir = argv[1];
    const char *out_dir    = argv[2];
    const char *what       = argc > 3 ? argv[3] : "all";
    const int   sr         = 44100;

    const int   frames = sr * 2;               /* 2 s per voice */
    float      *buf    = (float*)calloc((size_t)frames, sizeof(float));
    if(!buf) return 1;

    if(!strcmp(what, "pattern"))
    {
        const int pframes = sr * 4;
        float *p = (float*)calloc((size_t)pframes, sizeof(float));
        er99_engine_t e; er99_engine_init(&e, (float)sr, module_dir);

        /* key=value overrides apply here too — a pattern render that ignores
         * them silently is exactly how a bogus A/B test happens. */
        for(int a=4; a<argc; ++a)
        {
            char kv[128];
            snprintf(kv, sizeof(kv), "%s", argv[a]);
            char *eq = strchr(kv, '=');
            if(!eq) continue;
            *eq = '\0';
            if(!er99_engine_set_param(&e, kv, (float)atof(eq + 1)))
                fprintf(stderr, "  (unknown param: %s)\n", kv);
        }

        /* 16 steps over 2 bars at 120 bpm */
        const int step = sr / 4;               /* 16ths at 120 bpm */
        for(int s=0; s<16; ++s)
        {
            const int at = s * step;
            if(s % 4 == 0)            er99_engine_trigger(&e, ER99_BD, 110);
            if(s % 8 == 4)            er99_engine_trigger(&e, ER99_SD, 110);
            if(s % 2 == 0)            er99_engine_trigger(&e, ER99_CHH, 80);
            if(s == 14)               er99_engine_trigger(&e, ER99_OHH, 100);
            if(s == 7)                er99_engine_trigger(&e, ER99_HC, 100);
            if(s == 11)               er99_engine_trigger(&e, ER99_RS, 100);
            er99_engine_render(&e, p + at, step);
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/er99_pattern.wav", out_dir);
        write_wav(path, p, 16 * step, sr);
        printf("wrote %s\n", path);
        er99_engine_free(&e); free(p); free(buf);
        return 0;
    }

    for(int t=0; t<ER99_NUM_TRIGGERS; ++t)
    {
        if(strcmp(what, "all") && strcmp(what, er99_trigger_names[t])) continue;

        er99_engine_t e;
        er99_engine_init(&e, (float)sr, module_dir);

        /* Trailing key=value args override engine parameters, so variants can
         * be auditioned without rebuilding. */
        for(int a=4; a<argc; ++a)
        {
            char kv[128];
            snprintf(kv, sizeof(kv), "%s", argv[a]);
            char *eq = strchr(kv, '=');
            if(!eq) continue;
            *eq = '\0';
            if(!er99_engine_set_param(&e, kv, (float)atof(eq + 1)))
                fprintf(stderr, "  (unknown param: %s)\n", kv);
        }

        /* isolate the voice: render it alone, no accent */
        er99_engine_trigger(&e, (er99_trigger_t)t, 80);
        memset(buf, 0, (size_t)frames * sizeof(float));
        er99_engine_render(&e, buf, frames);

        float peak = 0.0f;
        for(int i=0; i<frames; ++i) { const float a = buf[i] < 0 ? -buf[i] : buf[i]; if(a > peak) peak = a; }

        char path[1024];
        snprintf(path, sizeof(path), "%s/c_%s.wav", out_dir, er99_trigger_names[t]);
        write_wav(path, buf, frames, sr);
        printf("%-4s peak=%.4f  %s\n", er99_trigger_names[t], peak, path);

        er99_engine_free(&e);
    }

    free(buf);
    return 0;
}
