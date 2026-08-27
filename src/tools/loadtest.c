/* End-to-end test of the REAL dsp.so exactly as the chain host uses it:
 * dlopen -> move_plugin_init_v2 -> create_instance -> params -> midi -> render.
 * Built for aarch64 and run ON THE MOVE. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "plugin_api_v1.h"

static void hlog(const char*m){ printf("   [host] %s\n", m); }

/* Fake transport for the sequencer test: advances one beat per ~11 blocks. */
static double g_beats = -1.0;
static double fake_beat_position(void){ return g_beats; }
static int fake_clock_status(void){ return g_beats >= 0.0 ? 2 : 1; }
static int fails = 0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } else printf("ok  : %s\n", msg);}while(0)

int main(int argc, char**argv){
    const char *so  = argc>1?argv[1]:"./dsp.so";
    const char *dir = argc>2?argv[2]:".";
    void *h = dlopen(so, RTLD_NOW);
    if(!h){ printf("FAIL dlopen: %s\n", dlerror()); return 1; }
    CHECK(1, "dlopen dsp.so");

    move_plugin_init_v2_fn init = (move_plugin_init_v2_fn)dlsym(h, "move_plugin_init_v2");
    CHECK(init != NULL, "move_plugin_init_v2 symbol present");
    if(!init) return 1;

    static host_api_v1_t host; memset(&host,0,sizeof(host));
    host.api_version=1; host.sample_rate=44100; host.frames_per_block=128; host.log=hlog;
    host.get_beat_position = fake_beat_position;
    host.get_clock_status  = fake_clock_status;

    plugin_api_v2_t *api = init(&host);
    CHECK(api && api->api_version==2, "api_version == 2");
    CHECK(api->create_instance && api->render_block && api->on_midi &&
          api->set_param && api->get_param, "all v2 entry points non-NULL");

    void *inst = api->create_instance(dir, NULL);
    CHECK(inst != NULL, "create_instance");
    if(!inst) return 1;

    /* the two dynamic payloads the shadow UI needs */
    static char buf[65536];
    int n = api->get_param(inst, "chain_params", buf, sizeof(buf));
    CHECK(n > 100 && buf[0]=='[', "get_param(chain_params) returns JSON array");
    printf("      chain_params = %d bytes\n", n);
    /* Every voice's Drive and Distortion must be DECLARED, not merely
     * implemented: an undeclared key still answers set_param (the web panel
     * writes it by name) but has no knob on the device, no automation target
     * and no entry for Movy. The sampled voices drifted that way once. */
    {
        static const char *must_declare[] = {
            "ohh_drive", "ohh_dist_type", "rc_drive", "rc_dist_type",
            "cr_drive", "cr_dist_type", "rs_dist_type", "hc_dist_type",
        };
        int declared = 1;
        for (unsigned i = 0; i < sizeof(must_declare)/sizeof(must_declare[0]); ++i)
        {
            char needle[64];
            snprintf(needle, sizeof(needle), "\"%s\"", must_declare[i]);
            if (!strstr(buf, needle))
            { printf("      NOT DECLARED: %s\n", must_declare[i]); declared = 0; }
        }
        CHECK(declared, "every voice declares Drive + Distortion in chain_params");
    }

    int m = api->get_param(inst, "ui_hierarchy", buf, sizeof(buf));
    CHECK(m < 0, "ui_hierarchy absent (ui_chain.js fallback engages)");
    m = api->get_param(inst, "ui_pages", buf, sizeof(buf));
    CHECK(m > 100 && strstr(buf, "\"levels\"") != NULL,
          "ui_pages serves the hierarchy for the param_pages binding");

    /* Parameters are 0..127 pot positions now, like the hardware panel. */
    api->set_param(inst, "bd_c_tune", "77");
    n = api->get_param(inst, "bd_c_tune", buf, sizeof(buf));
    CHECK(n>0 && atof(buf)==77.0, "pot bd_c_tune round-trips (77)");
    api->set_param(inst, "sd_c_snappy", "100");
    api->get_param(inst, "sd_c_snappy", buf, sizeof(buf));
    CHECK(atof(buf)==100.0, "pot sd_c_snappy round-trips (100)");
    api->set_param(inst, "ht_c_decay", "0");
    api->get_param(inst, "ht_c_decay", buf, sizeof(buf));
    CHECK(atof(buf)==0.0, "pot ht_c_decay accepts 0");
    api->set_param(inst, "ht_c_decay", "999");   /* must clamp, not wrap */
    api->get_param(inst, "ht_c_decay", buf, sizeof(buf));
    CHECK(atof(buf)==127.0, "pot ht_c_decay clamps 999 -> 127");
    api->set_param(inst, "ht_c_decay", "-5");
    api->get_param(inst, "ht_c_decay", buf, sizeof(buf));
    CHECK(atof(buf)==0.0, "pot ht_c_decay clamps -5 -> 0");

    /* Every pot must move the sound: pot 0 vs 127 on kick decay must differ. */
    {
        static int16_t o[128*2];
        long e0=0,e1=0;
        api->set_param(inst,"bd_c_decay","0");
        { uint8_t m2[3]={0x90,36,100}; api->on_midi(inst,m2,3,0); }
        for(int b=0;b<120;++b){ api->render_block(inst,o,128);
            for(int k=0;k<256;++k){ long a=o[k]<0?-o[k]:o[k]; e0+=a; } }
        api->set_param(inst,"bd_c_decay","127");
        { uint8_t m2[3]={0x90,36,100}; api->on_midi(inst,m2,3,0); }
        for(int b=0;b<120;++b){ api->render_block(inst,o,128);
            for(int k=0;k<256;++k){ long a=o[k]<0?-o[k]:o[k]; e1+=a; } }
        char lbl[96]; snprintf(lbl,sizeof(lbl),"kick decay pot audibly changes (energy %ld -> %ld)",e0,e1);
        CHECK(e1 > e0*2, lbl);
    }

    /* Per-hit drift is off by default, so the kick repeats. Turning the knob
     * up must still vary the hit, or the control is dead. */
    {
        /* Two identical hits must SOUND the same by default. Compared sample by
         * sample over the attack (a late block is two silences and passes
         * either way), with the beater click muted: its noise is free-running
         * on purpose, varies every hit, and has nothing to do with pitch. */
        #define DRIFT_BLOCKS 24
        static int16_t hit[2][DRIFT_BLOCKS*128*2];
        api->set_param(inst,"bd_c_decay","64");
        api->set_param(inst,"bd_c_attack","0");      /* mute the beater noise */
        long d_off = 0, energy = 0;
        {
            /* Drift is pinned off at trigger — a 909 repeats exactly, and the
             * pin also defeats stale saved state. Only determinism is
             * asserted; there is no knob left to vary the hit. */
            for(int h = 0; h < 2; ++h)
            {
                uint8_t m2[3]={0x90,36,100}; api->on_midi(inst,m2,3,0);
                for(int k=0;k<DRIFT_BLOCKS;++k)
                    api->render_block(inst, hit[h] + k*128*2, 128);
                /* Let the voice ring out so hit 2 starts from the same
                 * (settled) filter state hit 1 did. */
                static int16_t tail[128*2];
                for(int k=0;k<160;++k) api->render_block(inst, tail, 128);
            }
            for(int k=0;k<DRIFT_BLOCKS*256;++k)
            {
                long x = (long)hit[0][k] - (long)hit[1][k];
                d_off += x < 0 ? -x : x;
                long a2 = hit[0][k]; energy += a2 < 0 ? -a2 : a2;
            }
        }
        api->set_param(inst,"bd_c_attack","45");
        char l1[128];
        snprintf(l1,sizeof(l1),"kick repeats identically by default (diff %ld vs energy %ld)",d_off,energy);
        CHECK(d_off * 1000 < energy, l1);
        #undef DRIFT_BLOCKS
    }

    /* The tom must really have the board's three oscillators, not one with a
     * bright edge. The audible claim, measured from a real 909, is temporal:
     * the upper VCOs bark in the attack and are gone by ~110 ms, leaving a
     * near-pure fundamental. So: partial band's share of the spectrum in the
     * first 93 ms versus at 370 ms. Calibrated natively against this exact
     * voice — 3 oscillators read 0.167 early / 0.082 late, and the same voice
     * with the upper envelopes silenced reads 0.095 / 0.082 — so the gate is
     * early > 1.6x late and early > 0.12. Both windows are Hann'd: the
     * fundamental is dominant enough that rectangular-window leakage buries a
     * true partial (tried), and a half window lets the onset splash flood the
     * bins identically for one oscillator or three (also tried). */
    {
        static float w1[4096], w2[4096];
        static int16_t tb[128*2];
        api->set_param(inst,"lt_c_decay","110");
        api->set_param(inst,"lt_c_level","127");
        api->set_param(inst,"lt_c_attack","0");     /* stick out of the way */
        api->set_param(inst,"lt_c_drive","0");
        { uint8_t m2[3]={0x90,38,100}; api->on_midi(inst,m2,3,0); }
        int n1=0, n2=0;
        for(int b2=0;b2<32;++b2){ api->render_block(inst,tb,128);
            for(int k=0;k<128 && n1<4096;++k) w1[n1++] = (float)tb[k*2]/32768.0f; }
        for(int b2=0;b2<94;++b2) api->render_block(inst,tb,128);    /* to ~370 ms */
        for(int b2=0;b2<32;++b2){ api->render_block(inst,tb,128);
            for(int k=0;k<128 && n2<4096;++k) w2[n2++] = (float)tb[k*2]/32768.0f; }
        for(int k=0;k<n1;++k)
            w1[k] *= 0.5f*(1.0f - cosf(2.0f*3.14159265f*(float)k/(float)(n1-1)));
        for(int k=0;k<n2;++k)
            w2[k] *= 0.5f*(1.0f - cosf(2.0f*3.14159265f*(float)k/(float)(n2-1)));

        #define GOERTZ(X,N,F) ({ \
            const float w = 2.0f*3.14159265f*(F)/44100.0f; \
            const float c = 2.0f*cosf(w); float s1=0.0f, s2=0.0f; \
            for(int q=0;q<(N);++q){ const float s0 = (X)[q] + c*s1 - s2; s2=s1; s1=s0; } \
            sqrtf(s1*s1 + s2*s2 - c*s1*s2); })
        float part1=0, part2=0;
        static const float pf[] = { 95, 102, 109, 180, 187, 194 };
        for(unsigned q=0;q<sizeof(pf)/sizeof(pf[0]);++q)
        { part1 += GOERTZ(w1,n1,pf[q]); part2 += GOERTZ(w2,n2,pf[q]); }
        const float early = part1 / (GOERTZ(w1,n1,68.0f) + 1.0f);
        const float late  = part2 / (GOERTZ(w2,n2,68.0f) + 1.0f);
        #undef GOERTZ

        char l[140]; snprintf(l,sizeof(l),
            "tom barks then rings pure (partial share %.3f early vs %.3f late)",
            (double)early, (double)late);
        CHECK(early > late * 1.6f && early > 0.12f, l);
        api->set_param(inst,"lt_c_attack","45");
        api->set_param(inst,"lt_c_drive","40");
        api->set_param(inst,"lt_c_level","64");
        api->set_param(inst,"lt_c_decay","64");
    }

    /* Closed hat is its own voice: its own tuning/level, and the two hats choke
     * each other because they are one pair of cymbals. */
    {
        static int16_t h[128*2];
        api->set_param(inst,"chh_pitch","100");
        api->get_param(inst,"chh_pitch",buf,sizeof(buf));
        CHECK(atof(buf)==100.0, "chh_pitch is its own control");
        api->set_param(inst,"ohh_pitch","20");
        api->get_param(inst,"chh_pitch",buf,sizeof(buf));
        CHECK(atof(buf)==100.0, "open hat tuning does not move the closed hat");

        /* Old patches carry the closed hat's decay as ohh_decay_closed. */
        api->set_param(inst,"ohh_decay_closed","90");
        api->get_param(inst,"chh_decay",buf,sizeof(buf));
        CHECK(atof(buf)==90.0, "ohh_decay_closed still reaches the closed hat");

        /* Open hat ringing, then the pedal shuts: the ring must stop. The closed
         * hat is muted for this, or its own sound would swamp what we measure. */
        api->set_param(inst,"ohh_decay","127");        /* long open hat */
        api->set_param(inst,"chh_volume","0");
        long ring = 0, choked = 0;
        for(int pass=0; pass<2; ++pass)
        {
            { uint8_t m2[3]={0x90,44,100}; api->on_midi(inst,m2,3,0); }   /* open */
            for(int b=0;b<8;++b) api->render_block(inst,h,128);
            /* Drum-rack map is sequential from 36: 43 is the closed hat, 44 the
             * open one. (42 is the clap — in GM it would be the closed hat.) */
            if(pass) { uint8_t m2[3]={0x90,43,100}; api->on_midi(inst,m2,3,0); }
            for(int b=0;b<4;++b) api->render_block(inst,h,128);           /* let the choke settle */
            long acc = 0;
            for(int b=0;b<16;++b){ api->render_block(inst,h,128);
                for(int k=0;k<256;++k){ long a2=h[k]<0?-h[k]:h[k]; acc+=a2; } }
            if(pass) choked = acc; else ring = acc;
            for(int b=0;b<400;++b) api->render_block(inst,h,128);
        }
        api->set_param(inst,"chh_volume","64");
        char l[120]; snprintf(l,sizeof(l),
            "closed hat chokes the open hat (%ld ringing vs %ld choked)", ring, choked);
        CHECK(choked * 4 < ring, l);
    }

    /* The output must be dead centre. The engine renders mono and writes the
     * same sample to both channels, so any imbalance would be a bug here, not
     * a mix decision — assert it sample-exact over a busy passage rather than
     * trusting the code to keep doing it. */
    {
        static int16_t st[128*2];
        long eL = 0, eR = 0; int mismatched = 0;
        for(int v = 0; v < 11; ++v)
        { uint8_t m2[3] = {0x90, (uint8_t)(36+v), 100}; api->on_midi(inst,m2,3,0); }
        for(int b = 0; b < 200; ++b)
        {
            api->render_block(inst, st, 128);
            for(int k = 0; k < 128; ++k)
            {
                if(st[k*2] != st[k*2+1]) mismatched++;
                eL += st[k*2]   < 0 ? -st[k*2]   : st[k*2];
                eR += st[k*2+1] < 0 ? -st[k*2+1] : st[k*2+1];
            }
        }
        char l[128];
        snprintf(l,sizeof(l),"output is centred: %d/25600 samples differ L vs R", mismatched);
        CHECK(mismatched == 0, l);
        snprintf(l,sizeof(l),"channel energy matches (L %ld, R %ld)", eL, eR);
        CHECK(eL == eR && eL > 0, l);
    }

    /* Level must keep working all the way up. The master bus compresses hard
     * above 0 dBFS, so a voice can reach a point where turning Level does
     * nothing audible — report the curve so that ceiling is visible. */
    {
        static int16_t lv[128*2];
        int peaks[5]; const char *pots[5] = { "16", "48", "80", "112", "127" };
        for(int i = 0; i < 5; ++i)
        {
            api->set_param(inst, "bd_c_level", pots[i]);
            /* Play it like a pattern, not a lab tone: four accented hits at
             * sixteenth-note spacing, measuring the last. A limiter that
             * cannot recover in that gap is exactly what makes Level feel
             * dead on the device. */
            for(int h = 0; h < 3; ++h)
            {
                uint8_t m2[3]={0x90,36,110}; api->on_midi(inst,m2,3,0);
                for(int b = 0; b < 43; ++b) api->render_block(inst, lv, 128);
            }
            { uint8_t m2[3]={0x90,36,110}; api->on_midi(inst,m2,3,0); }
            int pk = 0;
            for(int b = 0; b < 43; ++b)
            {
                api->render_block(inst, lv, 128);
                for(int k = 0; k < 256; ++k)
                { int a2 = lv[k] < 0 ? -lv[k] : lv[k]; if(a2 > pk) pk = a2; }
            }
            peaks[i] = pk;
        }
        api->set_param(inst, "bd_c_level", "100");
        char l[160];
        snprintf(l,sizeof(l),"kick Level keeps rising: 16=%d 48=%d 80=%d 112=%d 127=%d",
                 peaks[0],peaks[1],peaks[2],peaks[3],peaks[4]);
        /* Each step must add at least 3%% — less than that is a dead knob. */
        CHECK(peaks[1] > peaks[0]*103/100 && peaks[2] > peaks[1]*103/100 &&
              peaks[3] > peaks[2]*103/100 && peaks[4] > peaks[3]*103/100, l);
    }

    /* The Comp knob: zero is a hard bypass (identical output), and turning it
     * up must actually change the waveform. Kick only, Attack at zero: the
     * snare's noise source free-runs, so no two renders containing it are ever
     * bit-identical and it can prove nothing here. */
    {
        static int16_t c0[40*128*2], c1[40*128*2], cb[128*2];
        api->set_param(inst, "master_comp", "0");
        api->set_param(inst, "bd_c_attack", "0");
        for(int b=0;b<400;++b) api->render_block(inst,cb,128);
        { uint8_t m3[3]={0x90,36,110}; api->on_midi(inst,m3,3,0); }
        for(int b=0;b<40;++b) api->render_block(inst,c0+b*256,128);
        for(int b=0;b<400;++b) api->render_block(inst,cb,128);
        { uint8_t m3[3]={0x90,36,110}; api->on_midi(inst,m3,3,0); }
        for(int b=0;b<40;++b) api->render_block(inst,c1+b*256,128);
        int same = memcmp(c0,c1,sizeof(c0)) == 0;
        CHECK(same, "comp at 0 is a hard bypass (repeat renders identical)");
        api->set_param(inst, "master_comp", "127");
        for(int b=0;b<400;++b) api->render_block(inst,cb,128);
        { uint8_t m3[3]={0x90,36,110}; api->on_midi(inst,m3,3,0); }
        for(int b=0;b<40;++b) api->render_block(inst,c1+b*256,128);
        long d=0, ref=0;
        for(int k=0;k<40*256;++k){ long x=(long)c1[k]-(long)c0[k]; d+=x<0?-x:x;
                                   long a2=c0[k]; ref+=a2<0?-a2:a2; }
        char l[120]; snprintf(l,sizeof(l),"comp engages (diff %ld vs ref %ld)",d,ref);
        CHECK(d > ref/10, l);
        api->set_param(inst, "master_comp", "0");
        api->set_param(inst, "bd_c_attack", "13");
    }

    /* master distortion applies and is audible */
    api->set_param(inst, "master_dist", "2");
    api->get_param(inst, "master_dist", buf, sizeof(buf));
    CHECK(atof(buf)==2.0, "master_dist enum round-trips");
    api->set_param(inst, "master_drive", "90");
    api->get_param(inst, "master_drive", buf, sizeof(buf));
    CHECK(atof(buf)==90.0, "master_drive pot round-trips");
    {
        /* The reworked stages are level-normalised, so "louder" is no longer
         * the evidence — compare the waveforms of the same (deterministic)
         * kick with the stage off and on. */
        static int16_t offw[60*128*2], onw[60*128*2];
        long eoff=0,eon=0;
        api->set_param(inst,"master_dist","0");
        for(int b=0;b<400;++b) api->render_block(inst,offw,128);   /* ring out */
        { uint8_t m3[3]={0x90,36,110}; api->on_midi(inst,m3,3,0); }
        for(int b=0;b<60;++b) api->render_block(inst,offw+b*256,128);
        api->set_param(inst,"master_dist","2");
        for(int b=0;b<400;++b) api->render_block(inst,onw,128);    /* ring out */
        { uint8_t m3[3]={0x90,36,110}; api->on_midi(inst,m3,3,0); }
        for(int b=0;b<60;++b) api->render_block(inst,onw+b*256,128);
        for(int k2=0;k2<60*256;++k2){
            long d=(long)onw[k2]-(long)offw[k2]; eon+=d<0?-d:d;
            long a=offw[k2]; eoff+=a<0?-a:a;
        }
        char lbl2[96]; snprintf(lbl2,sizeof(lbl2),"master dist audibly changes output (diff %ld vs ref %ld)",eon,eoff);
        CHECK(eon > eoff/8, lbl2);
        api->set_param(inst,"master_dist","0");
    }

    /* state */
    n = api->get_param(inst, "state", buf, sizeof(buf));
    CHECK(n>200, "get_param(state) returns a blob");
    char saved[8192]; snprintf(saved,sizeof(saved),"%s",buf);
    api->set_param(inst, "bd_c_tune", "40");
    api->set_param(inst, "state", saved);
    api->get_param(inst, "bd_c_tune", buf, sizeof(buf));
    CHECK(atof(buf)==77.0, "state restore brings bd_c_tune back to 77");

    /* Pre-rebuild blobs must be REJECTED whole: their pot positions map onto
     * changed ranges and pinned fields, and restoring one resurrects the old
     * sound from any slot autosave, set state or patch it hides in. */
    api->set_param(inst, "bd_c_tune", "50");
    api->set_param(inst, "state", "er99v2;bd_c_tune=90;sd_c_osc2_mix=0.7;");
    api->get_param(inst, "bd_c_tune", buf, sizeof(buf));
    CHECK(atof(buf)==50.0, "v2 state blob is rejected whole (defaults survive)");

    /* audio: every drum-rack note must make sound */
    static int16_t out[128*2];
    const int notes[] = {36,37,38,39,40,41,42,43,44,45,46};
    for(size_t i=0;i<sizeof(notes)/sizeof(notes[0]);++i){
        uint8_t msg[3]={0x90,(uint8_t)notes[i],100};
        api->on_midi(inst,msg,3,0);
        long peak=0;
        for(int b=0;b<40;++b){ api->render_block(inst,out,128);
            for(int k=0;k<256;++k){ long a=out[k]<0?-out[k]:out[k]; if(a>peak)peak=a; } }
        char lbl[64]; snprintf(lbl,sizeof(lbl),"note %d produces audio (peak %ld)",notes[i],peak);
        CHECK(peak>200, lbl);
    }

    /* ---- Step sequencer: program a kick on steps 1+9, run the fake
     * transport, and demand audio with NO manual triggers at all. ---- */
    {
        api->set_param(inst, "seq_voice", "0");
        uint8_t step1[3]={0x90,16,100}, step9[3]={0x90,24,100};
        api->on_midi(inst, step1, 3, 0);
        api->on_midi(inst, step9, 3, 0);
        n = api->get_param(inst, "seq_bd", buf, sizeof(buf));
        CHECK(n>0 && atoi(buf)==(1|(1<<8)), "step toggles set seq_bd mask 257");

        uint8_t ext[3]={0x90,17,100};
        api->on_midi(inst, ext, 3, 2);
        api->get_param(inst, "seq_bd", buf, sizeof(buf));
        CHECK(atoi(buf)==(1|(1<<8)), "external note 16-31 ignored by sequencer");

        static int16_t so[128*2];
        long energy=0;
        g_beats = 0.0;
        for(int b=0;b<690;++b){
            api->render_block(inst, so, 128);
            for(int k3=0;k3<256;++k3){ long a=so[k3]<0?-so[k3]:so[k3]; energy+=a; }
            g_beats += 128.0/44100.0*2.0;
        }
        g_beats = -1.0;
        char lbl3[96]; snprintf(lbl3,sizeof(lbl3),"sequencer alone produces audio (energy %ld)",energy);
        CHECK(energy > 500000, lbl3);

        n = api->get_param(inst, "state", buf, sizeof(buf));
        CHECK(n>0 && strstr(buf,"seq_bd=257")!=NULL, "state blob contains seq_bd=257");
        api->set_param(inst, "seq_bd", "0");
        api->set_param(inst, "state", buf);
        api->get_param(inst, "seq_bd", buf, sizeof(buf));
        CHECK(atoi(buf)==257, "state restore brings the pattern back");
        api->set_param(inst, "seq_bd", "0");
    }

    /* Silent-select gate: with mute_ms set, a note must NOT sound; after the
     * window has elapsed it must sound again. */
    {
        static int16_t mo[128*2];
        long gated=0, after=0;
        api->set_param(inst, "mute_ms", "120");
        { uint8_t mm[3]={0x90,36,110}; api->on_midi(inst,mm,3,0); }
        for(int b=0;b<30;++b){ api->render_block(inst,mo,128);
            for(int k4=0;k4<256;++k4){ long a=mo[k4]<0?-mo[k4]:mo[k4]; gated+=a; } }
        for(int b=0;b<20;++b) api->render_block(inst,mo,128);   /* pass window */
        { uint8_t mm[3]={0x90,36,110}; api->on_midi(inst,mm,3,0); }
        for(int b=0;b<30;++b){ api->render_block(inst,mo,128);
            for(int k4=0;k4<256;++k4){ long a=mo[k4]<0?-mo[k4]:mo[k4]; after+=a; } }
        char lbl4[96]; snprintf(lbl4,sizeof(lbl4),"mute_ms gates note (gated %ld vs after %ld)",gated,after);
        CHECK(gated < after/10, lbl4);
    }

    /* Lane mutes: muted kick must be silent, unmuted must sound. */
    {
        static int16_t lm[128*2];
        long muted=0, unmuted=0;
        /* Let whatever is still ringing die first. The kick's tail is ~860 ms
         * now that its decay matches a real 909, so without this the "muted"
         * window is measuring the PREVIOUS hit, not silence. */
        for(int b=0;b<400;++b) api->render_block(inst,lm,128);
        api->set_param(inst, "mutes", "1");           /* bit 0 = BD */
        { uint8_t mm[3]={0x90,36,110}; api->on_midi(inst,mm,3,0); }
        for(int b=0;b<30;++b){ api->render_block(inst,lm,128);
            for(int k5=0;k5<256;++k5){ long a=lm[k5]<0?-lm[k5]:lm[k5]; muted+=a; } }
        api->set_param(inst, "mutes", "0");
        { uint8_t mm[3]={0x90,36,110}; api->on_midi(inst,mm,3,0); }
        for(int b=0;b<30;++b){ api->render_block(inst,lm,128);
            for(int k5=0;k5<256;++k5){ long a=lm[k5]<0?-lm[k5]:lm[k5]; unmuted+=a; } }
        char lbl5[96]; snprintf(lbl5,sizeof(lbl5),"lane mute silences BD (%ld vs %ld)",muted,unmuted);
        CHECK(muted < unmuted/10, lbl5);
        /* mutes survive the state blob */
        api->set_param(inst, "mutes", "5");
        n = api->get_param(inst, "state", buf, sizeof(buf));
        CHECK(n>0 && strstr(buf,"mutes=5")!=NULL, "state blob contains mutes=5");
        api->set_param(inst, "mutes", "0");
        api->set_param(inst, "state", buf);
        api->get_param(inst, "mutes", buf, sizeof(buf));
        CHECK(atoi(buf)==5, "state restore brings mutes back");
        api->set_param(inst, "mutes", "0");
    }

    /* per-voice dist on samplers */
    api->set_param(inst, "ohh_dist_type", "2");
    api->get_param(inst, "ohh_dist_type", buf, sizeof(buf));
    CHECK(atof(buf)==2.0, "ohh_dist_type round-trips");
    api->set_param(inst, "rc_drive", "110");
    api->get_param(inst, "rc_drive", buf, sizeof(buf));
    CHECK(atof(buf)==110.0, "rc_drive pot round-trips");
    api->set_param(inst, "ohh_dist_type", "0");
    api->set_param(inst, "rc_drive", "0");

    /* Velocity. Below the accent point it must scale the voice; at or above it
     * the accent gain is untouched at any depth, so no existing pattern gets
     * quieter. Depth 0 restores the old switch: every sub-accent velocity the
     * same level. */
    {
        long p30=0, p90=0, p110=0, f30=0, f90=0, f110=0;
        for(int pass=0; pass<2; ++pass){
            api->set_param(inst, "vel_depth", pass ? "0" : "127");
            const int vv[3] = { 30, 90, 110 };
            long out3[3] = {0,0,0};
            for(int i=0;i<3;++i){
                for(int b=0;b<200;++b) api->render_block(inst,out,128);   /* drain */
                uint8_t hit[3] = { 0x90, 43, (uint8_t)vv[i] };            /* closed hat */
                api->on_midi(inst, hit, 3, 0);
                long pk=0;
                for(int b=0;b<80;++b){
                    api->render_block(inst,out,128);
                    for(int k=0;k<256;++k){ long a=out[k]<0?-out[k]:out[k]; if(a>pk)pk=a; }
                }
                out3[i]=pk;
            }
            if(pass){ f30=out3[0]; f90=out3[1]; f110=out3[2]; }
            else    { p30=out3[0]; p90=out3[1]; p110=out3[2]; }
        }
        api->set_param(inst, "vel_depth", "127");
        char l[128];
        snprintf(l,sizeof l,"velocity scales below accent (v30 %ld < v90 %ld)",p30,p90);
        CHECK(p30 > 0 && p30 * 10 < p90 * 7, l);
        snprintf(l,sizeof l,"accent lifts the hit at full depth (v110 %ld > v90 %ld)",p110,p90);
        CHECK(p110 > p90 * 13 / 10, l);
        /* Velocity 0 must mean velocity 0: every velocity the same level,
         * accented ones included. Move's Full Velocity sends 127, and with the
         * first cut that still jumped 6 dB over a hand-played hit. */
        snprintf(l,sizeof l,"Velocity 0 = no response at all (v30 %ld, v90 %ld, v110 %ld)",f30,f90,f110);
        CHECK(f30 == f90 && f90 == f110 && f30 > 0, l);
    }

    /* FX sends: rim into the delay must produce an echo at 260 ms that is
     * absent with the send at zero; snare into the reverb must leave tail
     * energy where the dry voice is already dead. */
    {
        long e_on=0, e_off=0;
        for(int pass=0; pass<2; ++pass){
            api->set_param(inst, "rs_dly", pass ? "127" : "0");
            api->set_param(inst, "dly_time", "5");   /* 1/8 = 250 ms at 120 BPM */
            for(int b=0;b<400;++b) api->render_block(inst,out,128);  /* drain */
            uint8_t hit[3] = { 0x90, 41, 110 };
            api->on_midi(inst, hit, 3, 0);
            /* echo window: 240-330 ms; rim itself is dead by 220 ms */
            long e=0;
            for(int b=0;b<114;++b){
                api->render_block(inst,out,128);
                if(b>=83){ for(int k=0;k<256;++k){ long a=out[k]<0?-out[k]:out[k]; e+=a; } }
            }
            if(pass) e_on=e; else e_off=e;
        }
        char l[96];
        snprintf(l,sizeof l,"delay send: echo window %ld with send, %ld without",e_on,e_off);
        CHECK(e_on > 2000 && e_off < e_on/20, l);
        api->set_param(inst, "rs_dly", "0");
        long v_on=0, v_off=0;
        api->set_param(inst, "rev_decay", "127");   /* long tail so the window is unambiguous */
        for(int pass=0; pass<2; ++pass){
            api->set_param(inst, "sd_c_rev", pass ? "127" : "0");
            for(int b=0;b<1400;++b) api->render_block(inst,out,128);  /* drain both FX */
            uint8_t hit[3] = { 0x90, 37, 110 };
            api->on_midi(inst, hit, 3, 0);
            /* tail window: 0.6-1.0 s; the dry snare is long dead there */
            long e=0;
            for(int b=0;b<344;++b){
                api->render_block(inst,out,128);
                if(b>=207){ for(int k=0;k<256;++k){ long a=out[k]<0?-out[k]:out[k]; e+=a; } }
            }
            if(pass) v_on=e; else v_off=e;
        }
        char l2[96];
        snprintf(l2,sizeof l2,"reverb send: tail %ld with send, %ld without",v_on,v_off);
        CHECK(v_on > 2000 && v_off < v_on/20, l2);
        api->set_param(inst, "sd_c_rev", "0");
        api->set_param(inst, "rev_decay", "73");    /* back to the default pot */
    }

    /* Silence when idle. Must outlast the longest voice: ride/crash decay is
     * 2000 ms, so render 6 s before asserting, and report when it actually
     * went quiet so a stuck voice is distinguishable from a short window. */
    {
        long peak=0; int quiet_at=-1;
        const int blocks = (int)(6.0*44100/128);
        for(int b=0;b<blocks;++b){
            api->render_block(inst,out,128);
            long bp=0;
            for(int k=0;k<256;++k){ long a=out[k]<0?-out[k]:out[k]; if(a>bp)bp=a; }
            if(bp>peak) peak=bp;
            if(bp<50 && quiet_at<0) quiet_at=b;
            if(bp>=50) quiet_at=-1;
        }
        long tail=0;
        for(int b=0;b<40;++b){ api->render_block(inst,out,128);
            for(int k=0;k<256;++k){ long a=out[k]<0?-out[k]:out[k]; if(a>tail)tail=a; } }
        printf("      went quiet after %.2f s; final tail peak %ld\n",
               quiet_at<0?-1.0:quiet_at*128.0/44100.0, tail);
        CHECK(tail<50, "returns to silence when idle (6 s after last hit)");
    }

    api->destroy_instance(inst);
    CHECK(1, "destroy_instance");
    dlclose(h);
    printf("\n%s (%d failures)\n", fails?"*** FAILURES ***":"ALL CHECKS PASSED", fails);
    return fails?1:0;
}
