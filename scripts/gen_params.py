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

for vid,cfg in V.items():
    tl,th=cfg["tune"]; dl,dh=cfg["decay"]; sl,sh=cfg["sweep"]; bl,bh=cfg["body"]
    # RD-9 / modded-909 panel names: PITCH = base freq, P.DEPTH = amount of
    # the initial sweep (pot 0 = flat), TUNE = how fast the sweep decays.
    ps=[I(f"{vid}_c_tune","Pitch",tl,th,"Hz"),
        F(f"{vid}_c_sweep_depth","P. Depth",1,6,0.05),
        I(f"{vid}_c_sweep_time","Tune",sl,sh,"ms"),
        F(f"{vid}_c_attack","Attack",0,1,0.01),
        I(f"{vid}_c_decay","Decay",dl,dh,"ms"),
        F(f"{vid}_c_drive","Drive",0.2,8,0.1),
        {"key":f"{vid}_c_dist_type","name":"Distortion","type":"enum","options":DIST},
        F(f"{vid}_c_level","Level",0,2,0.01),
        I(f"{vid}_c_click_tone","Click Tone",500,8000,"Hz")]
    if vid=="bd":
        ps+=[F("bd_c_sub","Sub",0,1,0.01),
             F("bd_c_tube","Tube",0,6,0.05),
             F("bd_c_drift","Drift",0,1,0.01)]
    if vid=="sd":
        ps+=[I("sd_c_tune2","Shell 2",bl,bh,"Hz"), F("sd_c_osc2_mix","Shell Mix",0,1,0.01),
             F("sd_c_snappy","Snappy",0,1,0.01), I("sd_c_noise_decay","Snare Decay",10,400,"ms"),
             I("sd_c_noise_hp","Snare Tone",200,6000,"Hz")]
    else:
        ps+=[I(f"{vid}_c_tune2","Body Freq",bl,bh,"Hz"), F(f"{vid}_c_osc2_mix","Body Mix",0,1,0.01)]
    cp+=ps
    levels[vid]={"name":cfg["name"],
        "knobs":[x["key"] for x in ps[:8]],
        "params":[{"key":x["key"],"name":x["name"]} for x in ps]}
    root.append({"level":vid,"label":cfg["name"]})

rim=[I("rs_tune","Tune",0,127), I("rs_tune2","Tone",0,127), I("rs_res","Res",0,127),
     I("rs_decay","Decay",0,127), I("rs_noise","Noise",0,127),
     I("rs_saturation","Drive",0,127), I("rs_volume","Level",0,127)]
clap=[I("hc_tune","Tune",0,127), I("hc_spread","Spread",0,127),
      I("hc_tone_decay","Burst",0,127), I("hc_decay","Tail",0,127),
      I("hc_tail","Tail Mix",0,127), I("hc_drive","Drive",0,127),
      I("hc_volume","Level",0,127)]
hat=[I("ohh_decay","Open Decay",20,1200,"ms"), I("ohh_decay_closed","Closed Decay",15,300,"ms"),
     F("ohh_pitch","Pitch",0.25,4,0.01), F("ohh_volume","Level",0,2,0.01)]
cym=[I("rc_decay","Ride Decay",100,3000,"ms"), F("rc_pitch","Ride Pitch",0.25,4,0.01),
     F("rc_volume","Ride Level",0,2,0.01),
     I("cr_decay","Crash Decay",100,3000,"ms"), F("cr_pitch","Crash Pitch",0.25,4,0.01),
     F("cr_volume","Crash Level",0,2,0.01)]
glob=[{"key":"master_dist","name":"Master Dist","type":"enum",
       "options":["Off","Diode (909)","Hard Clip","Wavefolder","Bitcrush"]},
      F("master_drive","Master Drive",0,127,1),
      F("volume","Volume",0,1,0.01), F("accent","Accent",1,4,0.05)]
cp+=rim+clap+hat+cym+glob

for lid,label,ps in (("rim","Rim Shot",rim),("clap","Hand Clap",clap),
                     ("hat","Hi-Hat",hat),("cym","Ride / Crash",cym)):
    levels[lid]={"name":label,"knobs":[x["key"] for x in ps[:8]],
                 "params":[{"key":x["key"],"name":x["name"]} for x in ps]}
    root.append({"level":lid,"label":label})

root+=[{"key":"master_dist","name":"Master Dist"},{"key":"master_drive","name":"Master Drive"},
       {"key":"volume","name":"Volume"},{"key":"accent","name":"Accent"}]
levels["root"]={"name":"9W9","knobs":["master_dist","master_drive","volume","accent"],"params":root}

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
