/*
 * 9W9 ui_chain.js — a thin binding over Schwung's shared param_pages library
 * (host 0.12.1+), keeping every 9W9 gesture the stock hierarchy editor does
 * not have.
 *
 * Division of labour:
 *   param_pages (stock)          this file (9W9)
 *   ------------------          ---------------------------------
 *   knob grid, Movy layout      pads pass through to Move + page-follow
 *   viz graphics (faders...)    Shift+Pad silent select (white pad follows)
 *   jog page / Shift+Jog        Mute+Pad per-lane 9W9 mutes
 *   section picker (jog click)  focus gate (never steal another slot's pads)
 *   hold-knob name strip        pad_block lifecycle + self-heal
 *   Shift reveal + fine mode
 *   Mute+knob reset-to-default
 *
 * Why this file still exists at all: the host's enterComponentEdit prefers a
 * module's ui_hierarchy and would never load ui_chain.js if we served one — so
 * the DSP serves the hierarchy under "ui_pages" instead, and the injected
 * getParam below rewrites the controller's "ui_hierarchy" read to it. Pads are
 * not part of the stock grid's input model, so pad behaviour stays ours.
 *
 * GPL-3.0. param_pages © Schwung contributors; its level walk derives from
 * schwung-movy (MIT, megadake).
 */

import { createController } from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
import { decodeInput, applyInput } from '/data/UserData/schwung/shared/param_pages/page_input.mjs';
import { PAGE_KNOBS, PAGE_MENU } from '/data/UserData/schwung/shared/param_pages/page_plan.mjs';
import { LAYOUT_MOVY } from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';

(function () {
    "use strict";

    /* raw pad note -> hierarchy level key (page-follow) */
    var PAD2LEVEL = { 68: "bd", 69: "sd", 70: "lt", 71: "mt",
                      76: "ht", 77: "rim", 78: "clap", 79: "chh",
                      84: "ohh", 85: "crash", 86: "ride", 87: "root",
                      94: "fxrev", 95: "fxdly" };

    /* Pads with no voice behind them (master, reverb, delay): they switch
     * the page and are swallowed — injecting them would move Move's pad
     * selection onto an empty drum-rack slot. */
    function isPageOnlyPad(n) { return n === 87 || n === 94 || n === 95; }

    /* raw pad note -> 9W9 trigger lane (Mute+Pad) */
    var PAD2LANE = { 68: 0, 69: 1, 70: 2, 71: 3, 76: 4, 77: 5, 78: 6,
                     79: 8, 84: 7, 85: 10, 86: 9 };

    /* level key -> lane whose mute the title indicator shows (-1 = none) */
    var LEVEL2LANE = { bd: 0, sd: 1, lt: 2, mt: 3, ht: 4, rim: 5, clap: 6,
                       ohh: 7, chh: 8, ride: 9, crash: 10, root: -1,
                       fxrev: -1, fxdly: -1 };

    var mySlot = -1;
    var padBlocked = false;
    var muteHeld = false;
    var mutesMask = 0;
    var controller = null;

    /* Main-page lock: jog-click while ON the Main page toggles it. While
     * locked, pads still play (and record) but no longer switch the page —
     * so the master knobs (Comp, Volume...) stay under your hands while you
     * jam the kit. Shift+Pad still selects: that gesture IS an explicit
     * "take me there". Lives on globalThis so it survives the editor being
     * re-entered (the host re-evaluates this file on every open). */
    function mainLocked() { return !!globalThis.__9w9_main_lock; }

    /* First-run gesture hint: once per shadow_ui session, dismissed by any
     * input -- including pads, which are our layer -- or on its own after
     * HINT_MS. A hint nobody can wave away feels stuck. The "shown" flag lives
     * on globalThis: the host re-evaluates this file on every editor open, so
     * module-level state would reset each time; the shadow_ui process's global
     * object is what actually lives for the session. */
    var HINT_MS = 4000;
    var HINT_FLAG = "__9w9_hint_shown";
    var hintUntil = 0;

    function dismissHint() {
        hintUntil = 0;
        if (controller && controller.dismissHint) controller.dismissHint();
    }

    function has(fn) { return typeof globalThis[fn] === "function"; }

    function uiSlot() {
        return has("shadow_get_ui_slot") ? shadow_get_ui_slot() : 0;
    }

    function isFocused() {
        return mySlot >= 0 && uiSlot() === mySlot;
    }

    function setPadBlock(on) {
        if (padBlocked === on) return;
        if (has("host_pad_block")) { host_pad_block(on ? 1 : 0); padBlocked = on; }
    }

    function shiftHeld() {
        return has("shadow_get_shift_held") && !!shadow_get_shift_held();
    }

    /* The controller's device I/O. One special case: its "ui_hierarchy" read
     * is rewritten to "ui_pages" — the key the DSP actually serves, because
     * serving ui_hierarchy itself would stop this file from ever loading. */
    function ctlGetParam(key) {
        if (!has("shadow_get_param")) return null;
        if (key === "synth:ui_hierarchy") key = "synth:ui_pages";
        return shadow_get_param(mySlot, key);
    }

    function ctlSetParam(key, value) {
        if (has("shadow_set_param")) shadow_set_param(mySlot, key, String(value));
    }

    function announce(text) {
        if (has("host_announce_screenreader")) host_announce_screenreader(text);
    }

    function refreshMutes() {
        var m = parseInt(ctlGetParam("synth:mutes"), 10);
        mutesMask = isNaN(m) ? 0 : m;
    }

    function toggleLaneMute(lane) {
        mutesMask = (mutesMask ^ (1 << lane)) & 0x7FF;
        ctlSetParam("synth:mutes", String(mutesMask));
    }

    /* Jump the grid to the first page of a hierarchy level. */
    function goToLevel(levelKey) {
        if (!controller) return;
        var pages = controller.pages;
        for (var i = 0; i < pages.length; i++) {
            if (pages[i].level === levelKey ||
                (levelKey === "root" && pages[i].level === null)) {
                controller.goToPage(i);
                return;
            }
        }
        if (levelKey === "root") controller.goToPage(0);
    }

    /* Give the pad back to Move as a real press: plays the (HiJack-muted) kit,
     * records while REC is on, and updates Move's pad selection. */
    function injectToMove(data) {
        if (!has("move_midi_inject_to_move")) return;
        var type = (data[0] & 0xF0) === 0x90 ? 0x09
                 : (data[0] & 0xF0) === 0x80 ? 0x08
                 : (data[0] & 0xF0) === 0xA0 ? 0x0A : 0;
        if (!type) return;
        move_midi_inject_to_move([type, data[0], data[1], data[2]]);
    }

    /* ---------------- chain_ui hooks ---------------- */

    function init() {
        mySlot = uiSlot();
        setPadBlock(true);
        refreshMutes();

        controller = createController({
            getParam: ctlGetParam,
            setParam: ctlSetParam,
            announce: announce,
            /* The host's two trailing pages — "My Presets" and "Module" —
             * which every component the shadow UI paginates gets for free.
             * A module that draws its own grid has to ask for them, and the
             * menus are the host's to build: the preset record lives in the
             * slot config, not in us.
             *
             * Guarded because the binding is absent on an older host, and
             * absent for a Master FX position, which has no preset record.
             * Without it the array is empty and nothing is appended, which is
             * exactly how this behaved before. */
            trailingMenus: function () {
                return (typeof shadow_component_trailing_menus === "function")
                    ? (shadow_component_trailing_menus() || [])
                    : [];
            }
        });
        controller.load({ slot: mySlot, component: "synth", prefix: "synth" });
        controller.setLayout(LAYOUT_MOVY);
        if (!globalThis[HINT_FLAG]) {
            globalThis[HINT_FLAG] = true;
            controller.showHint([
                "Pad: play + select",
                "Sh+Pad: select only",
                "Mute+Pad: mute drum",
                "Jog: page  Click: list",
                "Shift: fine + values",
                "Mute+knob: default"
            ], "9W9");
            hintUntil = Date.now() + HINT_MS;
        }
        announce("9W9");
    }

    /* Title-bar text. The stock grid prints the page's own name on the right
     * of the bar, so this must NOT repeat it ("9W9 > BASS DRUM  BASS DRUM"):
     * just the module name plus the mute flag for the drum on screen. */
    function onMainPage() {
        var page = controller && controller.page;
        return !!page && (page.level === "root" || page.level == null);
    }

    function title() {
        var t = "9W9";
        if (mainLocked()) t += " [L]";
        var page = controller && controller.page;
        var lane = page ? LEVEL2LANE[page.level] : -1;
        if (lane !== undefined && lane >= 0 && (mutesMask & (1 << lane)))
            t += " [M]";
        return t;
    }

    function tick() {
        var shown = !has("shadow_get_display_mode") || shadow_get_display_mode() === 1;
        var active = shown && isFocused();
        setPadBlock(active);
        if (!active || !controller) return;

        if (hintUntil && Date.now() >= hintUntil) dismissHint();
        controller.setReveal(shiftHeld());
        controller.tick();

        /* The grid paces its own redraws; draw every tick like the stock
         * binding does (a full page render is ~1.6 ms, measured upstream). */
        clear_screen();
        var page = controller.page;
        /* PAGE_MENU as well as the grid. A menu page is a list of actions with
         * no params behind it, and the LIBRARY draws it (renderPicker with
         * header:false) — its own note says "a tool embedding the grid should
         * get menus without owning a screen", and we are such a tool. Excluding
         * it here would print the unsupported-page fallback over a page the
         * library was about to draw correctly.
         *
         * Nothing in 9W9's hierarchy declares one yet, so this changes nothing
         * today. It is what the host's own trailing "My Presets" and "Module"
         * pages arrive as, and those are handed to a controller through
         * io.trailingMenus — a hook the host does not currently expose to a
         * module-owned chain UI (see the note on ctlGetParam). This side is
         * ready for the day it does. */
        if (controller.pickerOpen ||
            (page && (page.kind === PAGE_KNOBS || page.kind === PAGE_MENU))) {
            controller.render(
                {
                    fillRect: fill_rect, print: print, textWidth: text_width,
                    line: typeof draw_line === "function" ? draw_line : undefined,
                    fillCircle: typeof fill_circle === "function" ? fill_circle : undefined,
                    drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
                    drawArc: typeof draw_arc === "function" ? draw_arc : undefined
                },
                { title: title() }
            );
            /*
             * THE SECOND HALF OF THE DRAW, and it is not optional.
             *
             * render() paints a page into a rect the CALLER owns; nothing in
             * param_pages clears the screen, which is what lets a consumer
             * host a page inside its own chrome. So anything FULL-SCREEN is
             * handed back to the frame owner — and that is us.
             *
             * Today that means the enum peek: turn a multi-option enum and its
             * option list rises over the grid for ~700 ms. Without this call
             * the controller still tracks the peek and applyInput still
             * swallows the Back that dismisses it; it is simply painted
             * nowhere, so a Back after a turn does nothing visible either.
             *
             * 9W9 shipped that way, silently, on every enum on every page:
             * fourteen of them, worst on Delay Time, where thirteen note
             * divisions hide behind one 30 px cell — you cannot see 1/8. from
             * 1/8T without turning past it. Diagnosed and fixed by Charles on
             * CW-78 (charlesvestal/schwung-78W#1); the same file, the same
             * omission, here.
             *
             * Guarded because renderOverlays landed in a later host than this
             * file's min_host_version, and an older host simply has no
             * overlays to draw.
             */
            if (typeof controller.renderOverlays === "function") {
                controller.renderOverlays(
                    { fillRect: fill_rect, print: print, textWidth: text_width },
                    { clearScreen: clear_screen }
                );
            }
        } else {
            /* Non-grid page kinds do not occur in 9W9's hierarchy; if one ever
             * does, show something honest instead of a stale frame. */
            print(2, 28, "9W9: unsupported page", 1);
        }
    }

    function onMidiMessageInternal(data) {
        var status = data[0] & 0xF0;
        var d1 = data[1];
        var d2 = data[2];

        /* Another slot is focused: never react; keep the surface alive. */
        if (!isFocused()) {
            if ((status === 0x90 || status === 0x80 || status === 0xA0) &&
                d1 >= 68 && d1 <= 99)
                injectToMove(data);
            return;
        }

        /* Mute button held-state (CC 88): ours for Mute+Pad, the library's
         * for Mute+knob reset — tracked here, passed to decodeInput below. */
        if (status === 0xB0 && d1 === 88) {
            muteHeld = (d2 > 0);
            return;
        }

        /* ---- Pads: 9W9's own layer ---- */
        if ((status === 0x90 || status === 0x80 || status === 0xA0) &&
            d1 >= 68 && d1 <= 99) {

            if (status === 0x90 && d2 > 0) dismissHint();

            /* Mute + Pad: toggle that lane's 9W9 mute; press still reaches
             * Move so its native state stays in step. */
            if (muteHeld) {
                var lane = PAD2LANE[d1];
                if (status === 0x90 && d2 > 0 && lane !== undefined)
                    toggleLaneMute(lane);
                if (!isPageOnlyPad(d1)) injectToMove(data);
                return;
            }

            var level = PAD2LEVEL[d1];

            /* Locked to Main: the pad plays, the page stays. */
            if (mainLocked() && !shiftHeld()) {
                if (!isPageOnlyPad(d1)) injectToMove(data);
                return;
            }

            if (shiftHeld()) {
                /* Silent select: page follows AND Move's white pad follows —
                 * the DSP swallows exactly the one note routed back (60 ms
                 * window). Accepted trade: with REC armed and playing, this
                 * press would be recorded; Gus does not use REC. */
                if (status === 0x90 && d2 > 0) {
                    if (level !== undefined) goToLevel(level);
                    if (!isPageOnlyPad(d1)) {
                        ctlSetParam("synth:mute_ms", "60");
                        injectToMove(data);
                    }
                } else {
                    if (!isPageOnlyPad(d1)) injectToMove(data);   /* release */
                }
                return;
            }

            /* Plain pad: page follows what you play; Move plays/records.
             * Page-only pads (master 87, reverb 94, delay 95) never sound. */
            if (status === 0x90 && d2 > 0 && level !== undefined)
                goToLevel(level);
            if (!isPageOnlyPad(d1)) injectToMove(data);
            return;
        }

        /* ---- Everything else: the stock grid's input model ---- */
        if (!controller) return;
        var intent = decodeInput(data, { shift: shiftHeld(), mute: muteHeld });
        if (!intent) return;
        /* SHIFT + jog click toggles the Main-page lock. A plain click is
         * Schwung's, and has to stay Schwung's: it opens the section list, and
         * on the host's trailing pages it is how you activate a row — Save As,
         * Delete, Swap Module. This used to take the plain click on Main, which
         * was fine while Main was ours alone and became wrong the moment the
         * host started appending "My Presets" and "Module" to us. Taking a
         * gesture the platform needs is not ours to do. */
        if (intent.type === "click" && shiftHeld() &&
            !controller.pickerOpen && onMainPage()) {
            globalThis.__9w9_main_lock = !globalThis.__9w9_main_lock;
            return;
        }
        var todo = applyInput(controller, intent, { nowMs: Date.now(), reveal: false });
        if (todo && todo.action === "exit") {
            /* Back never reaches us (the host consumes it); any other exit
             * intent just closes the picker. */
            if (controller.pickerOpen) controller.closePicker();
        }
        /* A menu row's action — Save, Save As, Delete, Swap Module, Help.
         * Performed by the SHADOW UI, not here: they reach the user preset
         * store, the preset browser, the component picker and the help
         * screen, none of which a module can address. A true return means a
         * screen opened over us, so stop touching the page. */
        if (todo && todo.action && todo.action !== "exit" &&
            typeof shadow_component_run_action === "function") {
            if (shadow_component_run_action(todo.action)) return;
        }
        /* 'open' (opaque param editors) cannot occur: every 9W9 param is an
         * int or an enum. Ignored if a future param ever produces one. */
    }

    function onMidiMessageExternal(data) { }

    function handleBack() {
        if (controller && controller.pickerOpen) {
            controller.closePicker();
            return true;                       /* consumed: close the list */
        }
        setPadBlock(false);
        return false;                          /* host exits the editor */
    }

    globalThis.chain_ui = {
        init: init,
        tick: tick,
        onMidiMessageInternal: onMidiMessageInternal,
        onMidiMessageExternal: onMidiMessageExternal,
        handleBack: handleBack,
        /* The host tells us a preset was saved or loaded while our grid is
         * on screen. Our "My Presets" row is built by OUR controller from the
         * host's menus, so nothing else would refresh it — it would go on
         * reading "(none)" after a Save. Re-plan the trailing pages only. */
        onPresetsChanged: function () {
            if (controller && typeof controller.refreshTrailing === "function")
                controller.refreshTrailing();
        }
    };
})();
