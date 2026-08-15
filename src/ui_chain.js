/*
 * 9W9 ui_chain.js — the module-owned editor (RD-9 style).
 *
 * Workflow (per Gus's spec, 2026-08-15):
 *   - MOVE'S OWN SEQUENCER is the sequencer. The Move track carries a (HiJack-
 *     muted) drum rack; its MIDI OUT ch feeds the 9W9 slot. This editor must
 *     never take that away.
 *   - Pads therefore PASS THROUGH to Move: every pad event is re-injected via
 *     move_midi_inject_to_move (cable 0 = real pad press to Move), so pads
 *     play the kit, record into Move's clips while REC is on, and move Move's
 *     pad selection — identical to the editor being closed. We only OBSERVE
 *     the hit to switch the parameter page.
 *   - Shift+Pad = select the page silently: consumed, NOT injected, nothing
 *     sounds, nothing records.
 *   - Knobs 1-8 edit the visible page; jog cycles pages; pad 87 -> MASTER.
 *
 * pad_block(1) is what lets us see the pads at all (the shim then forwards
 * them here); the injection gives them back to Move, making the block
 * transparent. Released on Back and self-healed against the display state.
 *
 * Knob dials drawn in the style of Movy's renderer (MIT, DimaDake/schwung-movy).
 *
 * GPL-3.0.
 */

(function () {
    "use strict";

    var DIST = ["Diod", "Clip", "Fold", "Crsh"];
    var MDIST = ["Off", "Diod", "Clip", "Fold", "Crsh"];

    function voicePage(id, name) {
        return { id: id, name: name, knobs: [
            { k: id + "_c_tune",        n: "Ptch" },
            { k: id + "_c_sweep_depth", n: "PDep" },
            { k: id + "_c_sweep_time",  n: "Tune" },
            { k: id + "_c_attack",      n: "Atk"  },
            { k: id + "_c_decay",       n: "Dcy"  },
            { k: id + "_c_drive",       n: "Drv"  },
            { k: id + "_c_dist_type",   n: "Dist", enum: DIST },
            { k: id + "_c_level",       n: "Lvl"  }
        ]};
    }

    var PAGES = [
        voicePage("bd", "BASS DRUM"),
        voicePage("sd", "SNARE"),
        voicePage("lt", "LOW TOM"),
        voicePage("mt", "MED TOM"),
        voicePage("ht", "HI TOM"),
        { id: "bd2", name: "BD MORE", knobs: [
            { k: "bd_c_sub",        n: "Sub"  },
            { k: "bd_c_tube",       n: "Tube" },
            { k: "bd_c_drift",      n: "Drft" },
            { k: "bd_c_click_tone", n: "Clik" },
            { k: "bd_c_tune2",      n: "Body" },
            { k: "bd_c_osc2_mix",   n: "BMix" },
            { k: "sd_c_snappy",     n: "Snpy" },
            { k: "sd_c_noise_decay",n: "SnDc" }
        ]},
        { id: "rim", name: "RIM SHOT", knobs: [
            { k: "rs_tune",       n: "Ptch" },
            { k: "rs_tune2",      n: "Tone" },
            { k: "rs_res",        n: "Res"  },
            { k: "rs_decay",      n: "Dcy"  },
            { k: "rs_noise",      n: "Nois" },
            { k: "rs_saturation", n: "Drv"  },
            { k: "rs_dist_type",  n: "Dist", enum: DIST },
            { k: "rs_volume",     n: "Lvl"  }
        ]},
        { id: "clap", name: "HAND CLAP", knobs: [
            { k: "hc_tune",       n: "Ptch" },
            { k: "hc_spread",     n: "Sprd" },
            { k: "hc_tone_decay", n: "Brst" },
            { k: "hc_decay",      n: "Tail" },
            { k: "hc_tail",       n: "TMix" },
            { k: "hc_drive",      n: "Drv"  },
            { k: "hc_dist_type",  n: "Dist", enum: DIST },
            { k: "hc_volume",     n: "Lvl"  }
        ]},
        { id: "hat", name: "HI-HAT", knobs: [
            { k: "ohh_decay",        n: "Open" },
            { k: "ohh_decay_closed", n: "Clsd" },
            { k: "ohh_pitch",        n: "Ptch" },
            { k: "ohh_drive",        n: "Drv"  },
            { k: "ohh_dist_type",    n: "Dist", enum: DIST },
            { k: "ohh_volume",       n: "Lvl"  }
        ]},
        { id: "ride", name: "RIDE", knobs: [
            { k: "rc_decay",     n: "Dcy"  },
            { k: "rc_pitch",     n: "Ptch" },
            { k: "rc_drive",     n: "Drv"  },
            { k: "rc_dist_type", n: "Dist", enum: DIST },
            { k: "rc_volume",    n: "Lvl"  }
        ]},
        { id: "crash", name: "CRASH", knobs: [
            { k: "cr_decay",     n: "Dcy"  },
            { k: "cr_pitch",     n: "Ptch" },
            { k: "cr_drive",     n: "Drv"  },
            { k: "cr_dist_type", n: "Dist", enum: DIST },
            { k: "cr_volume",    n: "Lvl"  }
        ]},
        { id: "master", name: "MASTER", knobs: [
            { k: "master_dist",  n: "Dist", enum: MDIST },
            { k: "master_drive", n: "Drv"  },
            { k: "volume",       n: "Vol"  },
            { k: "accent",       n: "Acnt" }
        ]}
    ];

    /* raw pad note -> 9W9 trigger lane (for Mute+Pad) */
    var PAD2LANE = { 68:0, 69:1, 70:2, 71:3, 76:4, 77:5, 78:6,
                     79:8, 84:7, 85:10, 86:9 };
    /* page id -> lane whose mute the header indicator shows */
    var PAGE2LANE = { bd:0, sd:1, lt:2, mt:3, ht:4, bd2:0, rim:5, clap:6,
                      hat:7, ride:9, crash:10, master:-1 };

    /* raw pad note -> page index (left 4x4 kit layout; 87 = MASTER) */
    var PAD2PAGE = { 68:0, 69:1, 70:2, 71:3, 76:4, 77:6, 78:7, 79:8,
                     84:8, 85:10, 86:9, 87:11 };

    var pageIdx = 0;
    var vals = {};
    var dirty = true;
    var padBlocked = false;
    var lastTouched = -1;
    var muteHeld = false;
    var mutesMask = 0;

    function has(fn) { return typeof globalThis[fn] === "function"; }

    function uiSlot() {
        return has("shadow_get_ui_slot") ? shadow_get_ui_slot() : 0;
    }

    function setPadBlock(on) {
        if (padBlocked === on) return;
        if (has("host_pad_block")) { host_pad_block(on ? 1 : 0); padBlocked = on; }
    }

    function readParam(key) {
        if (!has("shadow_get_param")) return 0;
        var v = parseFloat(shadow_get_param(uiSlot(), "synth:" + key));
        return isNaN(v) ? 0 : Math.round(v);
    }

    function loadPage() {
        var page = PAGES[pageIdx];
        for (var i = 0; i < page.knobs.length; i++)
            vals[page.knobs[i].k] = readParam(page.knobs[i].k);
        dirty = true;
    }

    function refreshMutes() {
        if (!has("shadow_get_param")) return;
        var m = parseInt(shadow_get_param(uiSlot(), "synth:mutes"), 10);
        mutesMask = isNaN(m) ? 0 : m;
    }

    function toggleLaneMute(lane) {
        mutesMask = (mutesMask ^ (1 << lane)) & 0x7FF;
        if (has("shadow_set_param"))
            shadow_set_param(uiSlot(), "synth:mutes", String(mutesMask));
        dirty = true;
    }

    function selectPage(idx, announce) {
        if (idx === pageIdx) { dirty = true; return; }
        pageIdx = idx;
        loadPage();
        if (announce && has("host_announce_screenreader"))
            host_announce_screenreader(PAGES[pageIdx].name);
    }

    /* Give the pad back to Move as a real press: it plays the (muted) kit,
     * records while REC is on, and updates Move's pad selection. */
    function injectToMove(data) {
        if (!has("move_midi_inject_to_move")) return;
        var type = (data[0] & 0xF0) === 0x90 ? 0x09
                 : (data[0] & 0xF0) === 0x80 ? 0x08
                 : (data[0] & 0xF0) === 0xA0 ? 0x0A : 0;
        if (!type) return;
        move_midi_inject_to_move([type, data[0], data[1], data[2]]);
    }

    function relDelta(v) { return v < 64 ? v : v - 128; }

    function adjustKnob(ki, delta) {
        var kn = PAGES[pageIdx].knobs[ki];
        if (!kn) return;
        var max = kn.enum ? (kn.enum.length - 1) : 127;
        var step = kn.enum ? (delta > 0 ? 1 : -1) : delta;
        var v = (vals[kn.k] | 0) + step;
        if (v < 0) v = 0;
        if (v > max) v = max;
        lastTouched = ki;
        if (v === vals[kn.k]) { dirty = true; return; }
        vals[kn.k] = v;
        if (has("shadow_set_param"))
            shadow_set_param(uiSlot(), "synth:" + kn.k, String(v));
        dirty = true;
    }

    /* ---------------- drawing (128x64, 1-bit) ---------------- */

    var DIAL_R = 6;
    var CIRC = [];
    (function () {
        for (var a = 0; a < 32; a++) {
            var t = (a / 32) * 6.28318;
            CIRC.push([Math.round(Math.cos(t) * DIAL_R), Math.round(Math.sin(t) * DIAL_R)]);
        }
    })();

    function drawDial(cx, cy, v127) {
        var i;
        for (i = 0; i < CIRC.length; i++)
            set_pixel(cx + CIRC[i][0], cy + CIRC[i][1], 1);
        var ang = (225 - (v127 / 127) * 270) * 0.0174533;
        var dx = Math.cos(ang), dy = -Math.sin(ang);
        for (i = 1; i <= DIAL_R - 1; i++)
            set_pixel(Math.round(cx + dx * i), Math.round(cy + dy * i), 1);
        set_pixel(cx, cy, 1);
    }

    function draw() {
        clear_screen();
        var page = PAGES[pageIdx];

        fill_rect(0, 0, 128, 9, 1);
        var lane = PAGE2LANE[page.id];
        var hdr = "9W9 " + page.name;
        if (lane !== undefined && lane >= 0 && (mutesMask & (1 << lane)))
            hdr += " [M]";
        print(2, 1, hdr, 0);
        if (lastTouched >= 0 && page.knobs[lastTouched]) {
            var hk = page.knobs[lastTouched];
            var hv = vals[hk.k];
            var htxt = hk.enum ? (hk.enum[hv] || "?") : String(hv);
            print(126 - text_width(htxt), 1, htxt, 0);
        }

        for (var i = 0; i < 8; i++) {
            var kn = page.knobs[i];
            if (!kn) continue;
            var x = (i % 4) * 32;
            var y = i < 4 ? 13 : 39;
            var v = vals[kn.k];
            if (kn.enum) {
                var txt = (kn.enum[v] || "?").substring(0, 4);
                draw_rect(x + 2, y + 1, 28, 12, 1);
                print(x + 4 + ((24 - text_width(txt)) >> 1), y + 3, txt, 1);
            } else {
                drawDial(x + 15, y + 6, v);
            }
            /* label centred under the dial/box, clipped to its column */
            var lbl = kn.n.substring(0, 5);
            var lw = text_width(lbl);
            print(x + ((32 - lw) >> 1), y + 15, lbl, 1);
        }

        dirty = false;
    }

    /* ---------------- chain_ui hooks ---------------- */

    function init() {
        setPadBlock(true);
        loadPage();
        refreshMutes();
        if (has("host_announce_screenreader"))
            host_announce_screenreader("9W9 " + PAGES[pageIdx].name);
    }

    function tick() {
        var shown = !has("shadow_get_display_mode") || shadow_get_display_mode() === 1;
        setPadBlock(shown);
        if (dirty) draw();
    }

    function onMidiMessageInternal(data) {
        var status = data[0] & 0xF0;
        var d1 = data[1];
        var d2 = data[2];

        /* Pads: transparent pass-through to Move + page-follow for us.
         * Shift+Pad: consumed — select silently, Move never sees it. */
        if ((status === 0x90 || status === 0x80 || status === 0xA0) &&
            d1 >= 68 && d1 <= 99) {
            var shiftHeld = has("shadow_get_shift_held") && shadow_get_shift_held();
            var target = PAD2PAGE[d1];

            /* Mute + Pad: toggle that lane's 9W9 mute. The press still goes
             * to Move so its native Mute+Pad state stays in step. */
            if (muteHeld) {
                var lane = PAD2LANE[d1];
                if (status === 0x90 && d2 > 0 && lane !== undefined)
                    toggleLaneMute(lane);
                injectToMove(data);
                return;
            }

            if (shiftHeld) {
                if (status === 0x90 && d2 > 0 && target !== undefined)
                    selectPage(target, true);
                /* Move the white pad too: arm the swallow-one gate (60 ms,
                 * eats exactly the one note routed back through Move), then
                 * hand the press over — selection follows, nothing sounds.
                 * Caveat, accepted: with REC armed and playing, this press
                 * would be recorded. Gus does not use REC. */
                if (status === 0x90 && d2 > 0 && has("shadow_set_param")) {
                    shadow_set_param(uiSlot(), "synth:mute_ms", "60");
                    injectToMove(data);
                } else if (status === 0x80 || (status === 0x90 && d2 === 0)) {
                    injectToMove(data);      /* matching release for Move */
                }
                return;                      /* never sounds */
            }
            if (status === 0x90 && d2 > 0 && target !== undefined)
                selectPage(target, true);    /* follow what you play */
            injectToMove(data);              /* Move plays/records/selects */
            return;
        }

        if (status !== 0xB0) return;

        if (d1 === 88) {                     /* Mute button held-state */
            muteHeld = (d2 > 0);
            return;
        }

        if (d1 >= 71 && d1 <= 78 && d2 > 0) {
            adjustKnob(d1 - 71, relDelta(d2));
            return;
        }
        if (d1 === 14 && d2 > 0) {
            var d = relDelta(d2);
            var n = PAGES.length;
            selectPage((pageIdx + (d > 0 ? 1 : n - 1)) % n, true);
            return;
        }
    }

    function onMidiMessageExternal(data) { }

    function handleBack() {
        setPadBlock(false);
        return false;
    }

    globalThis.chain_ui = {
        init: init,
        tick: tick,
        onMidiMessageInternal: onMidiMessageInternal,
        onMidiMessageExternal: onMidiMessageExternal,
        handleBack: handleBack
    };
})();
