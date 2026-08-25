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
        long d_off = 0, d_on = 0, energy = 0;
        for(int pass = 0; pass < 2; ++pass)
        {
            /* pass 0: default (drift off).  pass 1: drift at full. */
            if(pass) api->set_param(inst,"bd_c_drift","127");
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
            long d = 0;
            for(int k=0;k<DRIFT_BLOCKS*256;++k)
            {
                long x = (long)hit[0][k] - (long)hit[1][k];
                d += x < 0 ? -x : x;
                if(!pass) { long a2 = hit[0][k]; energy += a2 < 0 ? -a2 : a2; }
            }
            if(!pass) d_off = d; else d_on = d;
        }
        api->set_param(inst,"bd_c_drift","0");
        api->set_param(inst,"bd_c_attack","45");
        char l1[128], l2[128];
        snprintf(l1,sizeof(l1),"kick repeats identically by default (diff %ld vs energy %ld)",d_off,energy);
        snprintf(l2,sizeof(l2),"drift knob still varies the hit when turned up (diff %ld)",d_on);
        CHECK(d_off * 1000 < energy, l1);
        CHECK(d_on  * 100  > energy, l2);
        #undef DRIFT_BLOCKS
    }

    /* Bridged-T topology (circuit_model 2): the snare and toms become
     * shock-excited resonators. Three things must hold, or it is not a
     * resonator model — it is an oscillator wearing a hat. */
    {
        static int16_t o3[128*2];
        api->set_param(inst,"circuit_model","2");

        /* 1. It rings: struck once, the network keeps sounding well after the
         *    excitation pulse (0.6 ms) has gone. */
        long late = 0;
        { uint8_t m2[3]={0x90,38,100}; api->on_midi(inst,m2,3,0); }   /* low tom */
        for(int b=0;b<8;++b) api->render_block(inst,o3,128);          /* skip 23 ms */
        for(int b=0;b<16;++b){ api->render_block(inst,o3,128);
            for(int k=0;k<256;++k){ long a2=o3[k]<0?-o3[k]:o3[k]; late+=a2; } }
        CHECK(late > 10000, "bridged-T rings on after the trigger pulse");

        /* 2. Pitch follows the Tune pot: a resonator tuned lower crosses zero
         *    fewer times in the same window. Counted on the ring, not the
         *    transient. */
        int zc[2] = {0,0};
        for(int pass=0; pass<2; ++pass)
        {
            api->set_param(inst,"lt_c_tune", pass ? "110" : "20");
            { uint8_t m2[3]={0x90,38,100}; api->on_midi(inst,m2,3,0); }
            for(int b=0;b<4;++b) api->render_block(inst,o3,128);
            int prev = 0;
            for(int b=0;b<16;++b){ api->render_block(inst,o3,128);
                for(int k=0;k<256;k+=2){
                    const int cur = o3[k] > 0 ? 1 : (o3[k] < 0 ? -1 : 0);
                    if(cur && prev && cur != prev) zc[pass]++;
                    if(cur) prev = cur; } }
        }
        { char l[110]; snprintf(l,sizeof(l),
            "bridged-T pitch tracks Tune (%d crossings low vs %d high)", zc[0], zc[1]);
          CHECK(zc[1] > zc[0] + 4, l); }

        /* 3. A retrigger ADDS to a ringing network instead of clearing it.
         *    The discriminator is not "does the second hit make sound" — a
         *    reset does that too — it is that striking a network which is
         *    still ringing cannot sound the same as striking a silent one.
         *    A voice that resets on trigger produces identical samples. */
        #define RT_BLOCKS 6
        static int16_t fresh[RT_BLOCKS*128*2], onto_ring[RT_BLOCKS*128*2];
        for(int b=0;b<300;++b) api->render_block(inst,o3,128);      /* silence */
        { uint8_t m2[3]={0x90,38,100}; api->on_midi(inst,m2,3,0); }
        for(int b=0;b<RT_BLOCKS;++b) api->render_block(inst, fresh + b*256, 128);
        for(int b=0;b<300;++b) api->render_block(inst,o3,128);      /* ring out */

        { uint8_t m2[3]={0x90,38,100}; api->on_midi(inst,m2,3,0); }
        for(int b=0;b<4;++b) api->render_block(inst,o3,128);        /* still ringing */
        { uint8_t m2[3]={0x90,38,100}; api->on_midi(inst,m2,3,0); }
        for(int b=0;b<RT_BLOCKS;++b) api->render_block(inst, onto_ring + b*256, 128);

        long diff = 0, ref = 0;
        for(int k=0;k<RT_BLOCKS*256;++k)
        {
            long d2 = (long)onto_ring[k] - (long)fresh[k];
            diff += d2 < 0 ? -d2 : d2;
            long a2 = fresh[k];
            ref += a2 < 0 ? -a2 : a2;
        }
        { char l[120]; snprintf(l,sizeof(l),
            "retrigger adds to the ring rather than resetting (diff %ld vs %ld)", diff, ref);
          CHECK(diff > ref/10, l); }
        #undef RT_BLOCKS

        /* 4. Every converted voice still sounds, at a comparable level to the
         *    oscillator core — a resonator tuned or damped wrong goes silent
         *    rather than wrong, which no other check here would catch. */
        for(int note = 37; note <= 40; ++note)
        {
            for(int b=0;b<200;++b) api->render_block(inst,o3,128);
            int peak = 0;
            { uint8_t m2[3]={0x90,(uint8_t)note,100}; api->on_midi(inst,m2,3,0); }
            for(int b=0;b<60;++b){ api->render_block(inst,o3,128);
                for(int k=0;k<256;++k){ int a2=o3[k]<0?-o3[k]:o3[k]; if(a2>peak) peak=a2; } }
            char l[110]; snprintf(l,sizeof(l),
                "bridged-T note %d sounds (peak %d)", note, peak);
            CHECK(peak > 2000, l);
        }

        api->set_param(inst,"lt_c_tune","64");
        api->set_param(inst,"circuit_model","1");
    }

    /* master distortion applies and is audible */
    api->set_param(inst, "master_dist", "2");
    api->get_param(inst, "master_dist", buf, sizeof(buf));
    CHECK(atof(buf)==2.0, "master_dist enum round-trips");
    api->set_param(inst, "master_drive", "90");
    api->get_param(inst, "master_drive", buf, sizeof(buf));
    CHECK(atof(buf)==90.0, "master_drive pot round-trips");
    {
        static int16_t o2[128*2];
        long eoff=0,eon=0;
        api->set_param(inst,"master_dist","0");
        { uint8_t m3[3]={0x90,36,110}; api->on_midi(inst,m3,3,0); }
        for(int b=0;b<60;++b){ api->render_block(inst,o2,128);
            for(int k2=0;k2<256;++k2){ long a=o2[k2]<0?-o2[k2]:o2[k2]; eoff+=a; } }
        api->set_param(inst,"master_dist","2");
        { uint8_t m3[3]={0x90,36,110}; api->on_midi(inst,m3,3,0); }
        for(int b=0;b<60;++b){ api->render_block(inst,o2,128);
            for(int k2=0;k2<256;++k2){ long a=o2[k2]<0?-o2[k2]:o2[k2]; eon+=a; } }
        char lbl2[96]; snprintf(lbl2,sizeof(lbl2),"master dist audibly changes output (%ld -> %ld)",eoff,eon);
        CHECK(eon > eoff*12/10 || eoff > eon*12/10, lbl2);
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
