#!/usr/bin/env python3
"""Generate er99_params.h + module.json. Single source of truth for the
parameter surface. module.json stays tiny (8 KB loader cap); chain_params and
ui_hierarchy are served from the DSP via get_param()."""
import json, pathlib, sys

# Which MIDI note plays each voice, for Schwung's pad_layout / voices contract.
# A host reads these to lay out a pad grid and to say which voice is focused;
# see docs/MODULES.md, "Declaring your performance surface".
#
# KEYED BY LEVEL ID, never by position. Three orders exist in this codebase and
# no two of them agree:
#
#   nav order (below)   bd sd lt mt ht rim clap chh ohh ride  crash
#   er99_trigger_t      BD SD LT MT HT RS  HC   OHH CHH RC    CR
#   drum-rack notes     36 37 38 39 40 41  42   43  44  46    45
#
# So chh/ohh and ride/crash are transposed between the enum and the nav list,
# and a map built on position would silently mislabel four voices — the two
# hats and the two cymbals, which is exactly the kind of thing that ships.
#
# Mirrors drumrack_to_trigger() and the GM switch in src/dsp/er99_plugin.c.
# Those stay the authority: if they change, change these with them.
NOTE_MAPS = {
    "drumrack": {"bd": 36, "sd": 37, "lt": 38, "mt": 39, "ht": 40,
                 "rim": 41, "clap": 42, "chh": 43, "ohh": 44,
                 "crash": 45, "ride": 46},
    "gm":       {"bd": 36, "sd": 38, "lt": 41, "mt": 47, "ht": 50,
                 "rim": 37, "clap": 39, "chh": 42, "ohh": 46,
                 "crash": 49, "ride": 51},
}

# Free-form hints. No host behaviour depends on them; a consumer that does not
# recognise a value ignores it.
ROLES = {"bd": "kick", "sd": "snare", "lt": "tom", "mt": "tom", "ht": "tom",
         "rim": "rim", "clap": "clap", "chh": "hat", "ohh": "hat",
         "ride": "cymbal", "crash": "cymbal"}

# Tight, musical, per-voice ranges. A knob should sweep its USEFUL range end to
# end — not 2 seconds of decay nobody wants on a kick.
V = {
  "bd": dict(name="Bass Drum", tune=(20,120),  decay=(15,400),  sweep=(1,120), body=(0,200)),
  "sd": dict(name="Snare",     tune=(120,400), decay=(15,300),  sweep=(1,60),  body=(150,800)),
  "lt": dict(name="Low Tom",   tune=(40,180),  decay=(30,700),  sweep=(1,250), body=(0,300)),
  "mt": dict(name="Med Tom",   tune=(70,260),  decay=(30,600),  sweep=(1,250), body=(0,400)),
  "ht": dict(name="Hi Tom",    tune=(110,400), decay=(30,500),  sweep=(1,250), body=(0,600)),
}
DIST = ["Diode (909)","Hard Clip","SAT","BFZ","PDIST","Wavefolder","Bitcrush"]

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

# FX sends: every voice except the kick. Kick has none, deliberately.
def SENDS(pre): return [F(pre+"_rev","Rev",0,1,0.01), F(pre+"_dly","Dly",0,1,0.01)]

cp=[]; levels={}
# Root holds ONLY globals and navigation — no per-voice params leaking in.
root=[{"key":"note_map","name":"Note Map"}]
cp+=[{"key":"note_map","name":"Note Map","type":"enum","options":["Drum Rack (36+)","General MIDI"]}]

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
            F("sd_c_level","Level",0,2,0.01)]+SENDS("sd_c")
    else:
        ps=[I(f"{vid}_c_tune","Tune",tl,th,"Hz"),
            I(f"{vid}_c_decay","Decay",dl,dh,"ms"),
            F(f"{vid}_c_attack","Attack",0,1,0.01),
            F(f"{vid}_c_drive","Drive",0.2,8,0.1),
            {"key":f"{vid}_c_dist_type","name":"Distortion","type":"enum","options":DIST},
            F(f"{vid}_c_level","Level",0,2,0.01)]+SENDS(f"{vid}_c")
    cp+=ps
    levels[vid]={"name":cfg["name"],
        "knobs":[x["key"] for x in ps[:8]],
        "params":[{"key":x["key"],"name":x["name"]} for x in ps]}
    root.append({"level":vid,"label":cfg["name"]})

# DIST(k) — every voice has Drive and a distortion type in the engine, so every
# voice declares them. Declared is what makes a param real to the rest of the
# system: an undeclared key still works from the web panel (which writes it by
# name) but has no knob on the device, no automation target and no entry for
# any tool reading the parameter list.
def DIST_T(k,n="Distortion"): return {"key":k,"name":n,"type":"enum","options":DIST}


# The real 909 rim has ONE pot: Level. Its two resonances (210 and 480 Hz),
# their Q, the decay and the noise in the trigger pulse are all fixed
# circuitry — see the pins in er99_engine_trigger. Tune is ours; Decay was
# too, and went, because the ring length is the network's, not a setting.
rim=[I("rs_tune","Tune",0,127),
     I("rs_saturation","Drive",0,127), DIST_T("rs_dist_type"),
     I("rs_volume","Level",0,127)]+SENDS("rs")
# The real 909 clap has ONE pot: Level. Tune and Tail stay because Gus likes
# them; spread, echo decay and tail share are the circuit's, pinned at trigger.
clap=[I("hc_tune","Tune",0,127), I("hc_decay","Tail",0,127),
      I("hc_drive","Drive",0,127), DIST_T("hc_dist_type"),
      I("hc_volume","Level",0,127)]+SENDS("hc")
# Closed and open hat are one pair of cymbals but two voices: each has its own
# tuning, decay, drive and level, and triggering either chokes the other (the
# pedal cannot be shut and open at once). Ride and crash likewise get a page
# each rather than sharing one.
ohat=[I("ohh_decay","Decay",20,1200,"ms"), F("ohh_pitch","Tune",0.25,4,0.01),
      F("ohh_drive","Drive",0.2,8,0.1), DIST_T("ohh_dist_type"),
      F("ohh_volume","Level",0,2,0.01)]+SENDS("ohh")
chat=[I("chh_decay","Decay",15,300,"ms"), F("chh_pitch","Tune",0.25,4,0.01),
      F("chh_drive","Drive",0.2,8,0.1), DIST_T("chh_dist_type"),
      F("chh_volume","Level",0,2,0.01)]+SENDS("chh")
ride=[I("rc_decay","Decay",100,3000,"ms"), F("rc_pitch","Tune",0.25,4,0.01),
      F("rc_drive","Drive",0.2,8,0.1), DIST_T("rc_dist_type"),
      F("rc_volume","Level",0,2,0.01)]+SENDS("rc")
crash=[I("cr_decay","Decay",100,3000,"ms"), F("cr_pitch","Tune",0.25,4,0.01),
       F("cr_drive","Drive",0.2,8,0.1), DIST_T("cr_dist_type"),
       F("cr_volume","Level",0,2,0.01)]+SENDS("cr")
glob=[{"key":"master_dist","name":"Master Dist","type":"enum",
       "options":["Off","Diode (909)","Hard Clip","SAT","BFZ","PDIST","Wavefolder","Bitcrush"]},
      F("master_drive","Master Drive",0,127,1),
      F("master_comp","Comp",0,1,0.01),
      F("volume","Volume",0,1,0.01), F("accent","Accent",1,4,0.05),
      F("vel_depth","Velocity",0,1,0.01)]
cp+=rim+clap+chat+ohat+ride+crash+glob

# The send FX pages: simple on purpose, an input HPF on each so low end
# stays out of the wet path.
rev=[F("rev_decay","Decay",0,1,0.01), F("rev_tone","Tone",0,1,0.01),
     I("rev_hpf","HPF",30,800,"Hz"), F("rev_level","Level",0,1.2,0.01)]
DIVS=["1/32","1/16T","1/16","1/8T","1/16.","1/8","1/4T","1/8.","1/4",
      "1/2T","1/4.","1/2","1/2."]
dly=[{"key":"dly_time","name":"Time","type":"enum","options":DIVS},
     F("dly_fdbk","Fdbk",0,0.85,0.01),
     F("dly_tone","Tone",0,1,0.01), I("dly_hpf","HPF",30,800,"Hz"),
     F("dly_level","Level",0,1.2,0.01)]
cp+=rev+dly   # without this the grid has no type/range for them and toggles

for lid,label,ps in (("rim","Rim Shot",rim),("clap","Hand Clap",clap),
                     ("chh","Closed Hat",chat),("ohh","Open Hat",ohat),
                     ("ride","Ride",ride),("crash","Crash",crash),
                     ("fxrev","Reverb",rev),("fxdly","Delay",dly)):
    levels[lid]={"name":label,"knobs":[x["key"] for x in ps[:8]],
                 "params":[{"key":x["key"],"name":x["name"]} for x in ps]}
    root.append({"level":lid,"label":label})

root+=[{"key":"master_dist","name":"Master Dist"},{"key":"master_drive","name":"Master Drive"},
       {"key":"master_comp","name":"Comp"},
       {"key":"volume","name":"Volume"},{"key":"accent","name":"Accent"},
       {"key":"vel_depth","name":"Velocity"}]
levels["root"]={"name":"9W9","knobs":["master_dist","master_drive","master_comp","volume","accent","vel_depth"],"params":root}

# Two audiences, two spellings of the same control.
#
# The knob grid draws a param under a page that already says CLOSED HAT, so
# there it wants to read "Decay". Schwung's LFO target picker draws one FLAT
# list across the whole module, where eleven voices each contribute a "Decay"
# and nothing says which pad you are about to automate.
#
# param_meta.mjs merges the two sources as {...inlineHierarchy, ...chainParams}
# and then resolves `meta.label || meta.name`. chain_params spells the display
# string `name`, inline hierarchy entries spell it `label` — so a `label` on
# the hierarchy entry survives the merge AND wins the fallback. That is the
# seam: prefix `name` for the picker, keep `label` bare for the page.
# (validate_contract.mjs checks `<level>.<key>.label`, so this is also what the
# contract asked for all along; these entries had been emitting `name`.)
# The 909's own panel abbreviations, two letters each: rc_ is the RIDE cymbal
# and cr_ is the CRASH cymbal, so they read RD and CY, not the other way round.
PREFIX = [("bd_c_","BD"), ("sd_c_","SN"), ("lt_c_","LT"), ("mt_c_","MT"),
          ("ht_c_","HT"), ("rs_","RM"),   ("hc_","CP"),   ("chh_","CH"),
          ("ohh_","OH"), ("rc_","RD"),    ("cr_","CY"),
          ("rev_","Rev"), ("dly_","Dly")]

def qualify(key, name):
    """Voice-qualified name for the flat list; globals are already unique."""
    for pre, tag in PREFIX:
        if key.startswith(pre):
            return f"{tag} {name}"
    return name

for e in cp:
    e["name"] = qualify(e["key"], e["name"])

# Hierarchy params carry the bare label, so the pages stay readable.
for lvl in levels.values():
    for pentry in lvl.get("params", []):
        if "name" in pentry and "key" in pentry:
            pentry["label"] = pentry.pop("name")

cpj=json.dumps(cp,separators=(",",":"))

def hierarchy_for(map_name):
    """The contract, with the note map the engine is currently using.

    TWO of these are emitted because g_note_map is switchable at RUNTIME
    (drum rack by default, General MIDI on request). One static hierarchy
    would be wrong half the time, and a host laying out pads from it would
    put every voice in the wrong place with nothing to indicate why.
    get_param picks between them; that is exactly why the declaration lives
    in ui_hierarchy and not in module.json, which cannot change its mind.

    Only VOICES get a note. fxrev and fxdly are pages that sound nothing,
    and a page without a note is how the contract says so.
    """
    notes = NOTE_MAPS[map_name]
    out = {}
    for lid, lvl in levels.items():
        e = dict(lvl)
        if lid in notes:
            e["note"] = notes[lid]
            if lid in ROLES:
                e["role"] = ROLES[lid]
        out[lid] = e
    # focus_param names the key this plugin ALREADY publishes: ui_focus_level,
    # "<hit-count>:<level-id>". Nothing new is written for it. The count is
    # what makes re-hitting the pad you are already on navigate again, and
    # this module has had it since before the contract existed -- the host
    # accepts the counted form for that reason.
    #
    # NOT seq_voice: that is an INDEX, and three orders in this codebase
    # disagree (see NOTE_MAPS), so an index-based focus would light the wrong
    # hat and the wrong cymbal. A name is order-independent.
    return {"pad_layout": "drums", "focus_param": "ui_focus_level",
            "levels": out}

uhj  = json.dumps(hierarchy_for("drumrack"), separators=(",",":"))
uhjg = json.dumps(hierarchy_for("gm"),       separators=(",",":"))
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
/* The same contract with the General MIDI note map, for g_note_map == 1.
 * The map is switchable at runtime, so one static hierarchy would be wrong
 * half the time; get_param picks. */
#define ER99_UI_HIERARCHY_GM_LEN {len(uhjg)}
static const char er99_ui_hierarchy_gm_json[] =
{cstr(uhjg)};
#endif
""")
# module.json's version is owned by the release process (bump by hand, tag
# must match); this generator must never touch it.
print(f"chain_params {len(cpj)}B  ui_hierarchy {len(uhj)}B  gm {len(uhjg)}B  levels={len(levels)}  params={len(cp)}")
