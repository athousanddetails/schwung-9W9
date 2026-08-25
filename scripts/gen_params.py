#!/usr/bin/env python3
"""Generate er99_params.h + module.json. Single source of truth for the
parameter surface. module.json stays tiny (8 KB loader cap); chain_params and
ui_hierarchy are served from the DSP via get_param()."""
import json, pathlib, sys

# Tight, musical, per-voice ranges. A knob should sweep its USEFUL range end to
# end — not 2 seconds of decay nobody wants on a kick.
V = {
  "bd": dict(name="Bass Drum", tune=(20,120),  decay=(15,400),  sweep=(1,120), body=(0,200)),
  "sd": dict(name="Snare",     tune=(120,400), decay=(15,300),  sweep=(1,60),  body=(150,800)),
  "lt": dict(name="Low Tom",   tune=(40,180),  decay=(30,700),  sweep=(1,250), body=(0,300)),
  "mt": dict(name="Med Tom",   tune=(70,260),  decay=(30,600),  sweep=(1,250), body=(0,400)),
  "ht": dict(name="Hi Tom",    tune=(110,400), decay=(30,500),  sweep=(1,250), body=(0,600)),
}
DIST = ["Diode (909)","Hard Clip","Wavefolder","Bitcrush"]

# Every continuous control is a 0..127 pot, exactly like the hardware panel.
# The DSP maps each pot to its real range with a musical curve (er99_pots.h),
# so the UI never shows milliseconds or hertz - just a pot position.
def POT(k,n):
    d={"key":k,"name":n,"type":"int","min":0,"max":127}
    # Honest viz declarations for the 0.12.x param-pages renderer:
    #  - levels/volumes draw as faders
    #  - "Attack" is the CLICK LEVEL, not an envelope time; declare viz:false
    #    so the detector cannot pair it with Decay into a fake AD envelope.
    kl = k.lower()
    if kl.endswith("_level") or kl.endswith("_volume") or kl == "volume":
        d["viz"] = {"kind": "fader"}
    elif kl.endswith("_attack"):
        d["viz"] = False
    return d
def I(k,n,mn,mx,u=None): return POT(k,n)
def F(k,n,mn,mx,st):     return POT(k,n)

cp=[]; levels={}
# Root holds ONLY globals and navigation — no per-voice params leaking in.
root=[{"key":"circuit_model","name":"Engine"},{"key":"note_map","name":"Note Map"}]
cp+=[{"key":"note_map","name":"Note Map","type":"enum","options":["Drum Rack (36+)","General MIDI"]},
     {"key":"circuit_model","name":"Engine","type":"enum","options":["Stock er-99","Circuit 909"]}]

# One page per voice, and only the controls the machine itself has.
#
# The TR-909's own panel is TUNE / ATTACK / DECAY / LEVEL for the kick,
# TUNE / TONE / SNAPPY / LEVEL for the snare and TUNE / DECAY / LEVEL for each
# tom — that is the whole front panel. What we add on top is what the mod
# community added: the kick's PITCH and P.DEPTH (the stock TUNE pot sets only
# how fast the pitch envelope falls, not the pitch), a snare decay, and our own
# Drive + Distortion.
#
# Everything else the engine has — sub layer, tube stage, per-hit drift, click
# tone, second shell / tom body — keeps working at its tuned default but is no
# longer a knob. They were the parts nobody asked for and every one of them
# cost a page turn.
for vid,cfg in V.items():
    tl,th=cfg["tune"]; dl,dh=cfg["decay"]; sl,sh=cfg["sweep"]
    if vid=="bd":
        # RD-9 / modded-909 names: PITCH = base freq, P.DEPTH = how much the
        # note starts sharp, TUNE = how fast that falls back (the stock pot).
        # The original 909 bass drum panel: TUNE, ATTACK, DECAY, LEVEL
        # (Colin Fraser: Tune = decay of the initial pitch sweep; Attack =
        # noise burst + click level; Decay = ENV1 length) — plus our Drive and
        # Distortion. No mod pots: the sweep itself is stock and fixed, and
        # Tune spans the 39-66 Hz Gus measured on his own machine, original
        # position ~55.
        # ...plus the two mod pots after Decay (Fraser/Whittle: Tune Depth to
        # 2.2x the normal sweep, Pitch 0.43-4.7x of usual). P.Depth at zero is
        # the stock kick bit for bit, and Pitch does nothing until P.Depth has
        # some value — RD-9 behaviour, per Gus.
        ps=[I("bd_c_tune","Tune",39,66,"Hz"),
            F("bd_c_attack","Attack",0,1,0.01),
            I("bd_c_decay","Decay",dl,dh,"ms"),
            F("bd_c_sweep_depth","P. Depth",0,1,0.01),
            F("bd_c_pitch_mod","Pitch",0.43,4.7,0.01),
            F("bd_c_drive","Drive",0.2,8,0.1),
            {"key":"bd_c_dist_type","name":"Distortion","type":"enum","options":DIST},
            F("bd_c_level","Level",0,2,0.01)]
    elif vid=="sd":
        # The 909 snare panel, exactly: TUNE / TONE / SNAPPY / LEVEL, plus our
        # Drive and Distortion. TONE is what it is on the hardware — ENV4's
        # discharge resistor (VR7), i.e. how long the noise rings — NOT a
        # filter; the noise bandpass is fixed circuitry and stays hidden.
        ps=[I("sd_c_tune","Tune",tl,th,"Hz"),
            I("sd_c_noise_decay","Tone",60,2000,"ms"),
            F("sd_c_snappy","Snappy",0,1,0.01),
            F("sd_c_drive","Drive",0.2,8,0.1),
            {"key":"sd_c_dist_type","name":"Distortion","type":"enum","options":DIST},
            F("sd_c_level","Level",0,2,0.01)]
    else:
        ps=[I(f"{vid}_c_tune","Tune",tl,th,"Hz"),
            I(f"{vid}_c_decay","Decay",dl,dh,"ms"),
            F(f"{vid}_c_attack","Attack",0,1,0.01),
            F(f"{vid}_c_drive","Drive",0.2,8,0.1),
            {"key":f"{vid}_c_dist_type","name":"Distortion","type":"enum","options":DIST},
            F(f"{vid}_c_level","Level",0,2,0.01)]
    cp+=ps
    levels[vid]={"name":cfg["name"],
        "knobs":[x["key"] for x in ps[:8]],
        "params":[{"key":x["key"],"name":x["name"]} for x in ps]}
    root.append({"level":vid,"label":cfg["name"]})

# DIST(k) — every voice has Drive and a distortion type in the engine, so every
# voice declares them. Declared is what makes a param real to the rest of the
# system: an undeclared key still works from the web panel (which writes it by
# name) but has no knob on the device, no automation target and no entry for
# Movy or any other tool reading the parameter list.
def DIST_T(k,n="Distortion"): return {"key":k,"name":n,"type":"enum","options":DIST}

# The real 909 rim has ONE pot: Level. Its two resonances (210 and 480 Hz),
# their Q, the decay and the noise in the trigger pulse are all fixed
# circuitry — see the pins in er99_engine_trigger. Tune is ours; Decay was
# too, and went, because the ring length is the network's, not a setting.
rim=[I("rs_tune","Tune",0,127),
     I("rs_saturation","Drive",0,127), DIST_T("rs_dist_type"),
     I("rs_volume","Level",0,127)]
# The real 909 clap has ONE pot: Level. Tune and Tail stay because Gus likes
# them; spread, echo decay and tail share are the circuit's, pinned at trigger.
clap=[I("hc_tune","Tune",0,127), I("hc_decay","Tail",0,127),
      I("hc_drive","Drive",0,127), DIST_T("hc_dist_type"),
      I("hc_volume","Level",0,127)]
# Closed and open hat are one pair of cymbals but two voices: each has its own
# tuning, decay, drive and level, and triggering either chokes the other (the
# pedal cannot be shut and open at once). Ride and crash likewise get a page
# each rather than sharing one.
ohat=[I("ohh_decay","Decay",20,1200,"ms"), F("ohh_pitch","Tune",0.25,4,0.01),
      F("ohh_drive","Drive",0.2,8,0.1), DIST_T("ohh_dist_type"),
      F("ohh_volume","Level",0,2,0.01)]
chat=[I("chh_decay","Decay",15,300,"ms"), F("chh_pitch","Tune",0.25,4,0.01),
      F("chh_drive","Drive",0.2,8,0.1), DIST_T("chh_dist_type"),
      F("chh_volume","Level",0,2,0.01)]
ride=[I("rc_decay","Decay",100,3000,"ms"), F("rc_pitch","Tune",0.25,4,0.01),
      F("rc_drive","Drive",0.2,8,0.1), DIST_T("rc_dist_type"),
      F("rc_volume","Level",0,2,0.01)]
crash=[I("cr_decay","Decay",100,3000,"ms"), F("cr_pitch","Tune",0.25,4,0.01),
       F("cr_drive","Drive",0.2,8,0.1), DIST_T("cr_dist_type"),
       F("cr_volume","Level",0,2,0.01)]
glob=[{"key":"master_dist","name":"Master Dist","type":"enum",
       "options":["Off","Diode (909)","Hard Clip","Wavefolder","Bitcrush"]},
      F("master_drive","Master Drive",0,127,1),
      F("master_comp","Comp",0,1,0.01),
      F("volume","Volume",0,1,0.01), F("accent","Accent",1,4,0.05)]
cp+=rim+clap+chat+ohat+ride+crash+glob

for lid,label,ps in (("rim","Rim Shot",rim),("clap","Hand Clap",clap),
                     ("chh","Closed Hat",chat),("ohh","Open Hat",ohat),
                     ("ride","Ride",ride),("crash","Crash",crash)):
    levels[lid]={"name":label,"knobs":[x["key"] for x in ps[:8]],
                 "params":[{"key":x["key"],"name":x["name"]} for x in ps]}
    root.append({"level":lid,"label":label})

root+=[{"key":"master_dist","name":"Master Dist"},{"key":"master_drive","name":"Master Drive"},
       {"key":"master_comp","name":"Comp"},
       {"key":"volume","name":"Volume"},{"key":"accent","name":"Accent"}]
levels["root"]={"name":"9W9","knobs":["master_dist","master_drive","master_comp","volume","accent"],"params":root}

cpj=json.dumps(cp,separators=(",",":")); uhj=json.dumps({"levels":levels},separators=(",",":"))
def cstr(s):
    return "\n".join(f'    "{s[k:k+100].replace(chr(92),chr(92)*2).replace(chr(34),chr(92)+chr(34))}"'
                     for k in range(0,len(s),100))
root_dir = pathlib.Path(__file__).resolve().parent.parent
(root_dir/"src/dsp/er99_params.h").write_text(f"""/* Generated by scripts/gen_params.py — do not edit by hand.
 * module.json is capped at 8 KB by Schwung's loader, so these are served
 * dynamically from the DSP via get_param(). */
#ifndef ER99_PARAMS_H
#define ER99_PARAMS_H
#define ER99_CHAIN_PARAMS_LEN {len(cpj)}
static const char er99_chain_params_json[] =
{cstr(cpj)};
#define ER99_UI_HIERARCHY_LEN {len(uhj)}
static const char er99_ui_hierarchy_json[] =
{cstr(uhj)};
#endif
""")
# module.json's version is owned by the release process (bump by hand, tag
# must match); this generator must never touch it.
print(f"chain_params {len(cpj)}B  ui_hierarchy {len(uhj)}B  levels={len(levels)}  params={len(cp)}")
