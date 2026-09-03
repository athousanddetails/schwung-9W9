# Porting 9W9's distortion stage, Main-page lock and send FX to 6W6 / 8W8

Written for the agent working in **schwung-6W6** or **schwung-8W8**. Everything
below has already been built, measured and approved on hardware in 9W9; your
job is to move it across without changing how the existing kit sounds.

**Reference implementation:** `/Users/gustavolima/Developer/909-schwung/schwung-er99`
(`main`, v2.2.0+). Read it directly and copy the maths — this document tells
you *what* to copy, *what will bite you*, and *what to prove before committing*.

**Scope split — read this first.** Gus wants, in this order:

1. **The seven-type distortion stage** — do it now.
2. **The Main-page jog lock** — do it now.
3. **Velocity** (§4) — do it now; it is a bug in both kits today.
4. **Voice-qualified LFO names** (§5) — do it now; small and self-contained.
5. **The send FX (reverb + delay)** — port the **DSP only**. Do **not** design
   the pages, pad mapping or panel layout. Gus wants to change the UI/UX
   himself first and will say when to wire it up.

---

## Status — 6W6, as of v1.4.1

Filled in by the 6W6 session after the port. Two entries diverge from what this
document told it to do; both were Gus's call and are marked.

| § | 6W6 | note |
|---|---|---|
| 1 distortion | **done** | 7 types, verbatim. Plus a dry-at-zero crossfade this doc does not describe — see §7.1 |
| 2 Main-page lock | **done** | now on Shift + jog click, as §6b requires; plain click is Schwung's |
| 3 send FX | **done, UI included** | Gus later asked for the pages, pads and panel. **The kick got sends too** — his explicit reversal of the kick-stays-dry rule below |
| 4 velocity | **done** | went further: Accent **deleted**, its gain kept as a constant. See §7.2 |
| 5 voice-qualified names | **done** | the seam works exactly as described |
| 6b trailing pages | **done** | verified on the deployed `ui_chain.js`: `trailingMenus`, `PAGE_MENU`, `exitMenu`, `restorePage`, `onPresetsChanged`, `shadow_component_run_action` |
| 6b.1 `ui_hierarchy` as `""` | **done** | verified by running 9W9's loadtest against the deployed `dsp.so`: serves empty |

**§7 is new**, and is the part worth reading before you touch 8W8: six traps
the 6W6 port hit that nothing above predicted. Two of them shipped broken.

> **Rows updated 2026-09-03 by the 9W9 session.** The three §6b/§6b.1/lock rows
> read NOT DONE when this table was written; all three have since shipped and
> were verified on the device for **6W6, 8W8 and CW-78 alike** — the wiring by
> grepping each deployed `ui_chain.js`, the `ui_hierarchy` answer by running
> 9W9's loadtest against each deployed `dsp.so`. Swap into any of the four
> drum machines now lands on its first page.

---

## 0. Ground rules

**The existing kit must not change.** 6W6 and 8W8 were fitted against hardware
(`tools/fit_defaults.cpp`, `tools/kit_check.cpp`). The proof expected is an A/B
checksum: build the current `HEAD` and your tree from the same probe, render
the whole kit plus a pattern, hash the floats, compare. Identical, or a
difference with a measured number and a reason attached. "It sounds the same"
is not a proof.

```c
/* render every voice for 1 s at default pots, hash the floats */
for(int v=0; v<NUM_VOICES; ++v){
    engine_init(&e, 44100, SAMPLE_DIR); engine_seed_pots(&e);
    engine_trigger(&e, v, 110);
    engine_render(&e, o, 44100);
    for(int i=0;i<44100;++i){ union{float f;unsigned u;}b; b.f=o[i];
                              h = (h ^ b.u) * 16777619u; }
}
```

Print a **per-voice running hash**, not just the final one. When 9W9's FX port
diverged, the per-voice split named the first affected voice in one run.

Build probes natively on the VPS (`ssh vps`, repos in `~/schwung-dev/<proj>`),
not in the ARM container.

**Process traps that cost real time:**

- `rsync` skips files whose mtime looks unchanged and ninja then builds stale
  sources. Always `find src -type f -exec touch {} +` before building.
- Build the "old" side from `git archive <tag>`, and build **both sides with
  identical flags in the same shell**. A phantom 5% difference once turned out
  to be a months-old scratch copy plus a differing `-D`.
- Heredocs and `git commit -m` do not mix with backticks: `` `label` `` in a
  message is command substitution and the word vanishes from the commit.

---

## 1. The seven-type distortion stage

The current 4 types (Diode, Clip, Fold, Crush) become 7, in menu order:

| # | Name | Character |
|---|---|---|
| 0 | Diode | the machine's own back-to-back diode rounding (unchanged) |
| 1 | Clip | asymmetric soft clip, even harmonics |
| 2 | SAT | warm parallel saturation, keeps the transient |
| 3 | BFZ | thick fuzz wall |
| 4 | PDIST | biased cubic crunch |
| 5 | Fold | wavefolder (was #2) |
| 6 | Crush | quantise **and** decimate (was #3) |

Copy the maths verbatim from `src/dsp/er99_circuit.h`, `er99_shape_st()`. Do
not re-derive it — Gus has approved how these sound and asked explicitly that
nobody go looking for other references.

Two structural changes come with it:

- **`_st` state parameter.** Crush decimates as well as quantises, so it needs
  two floats of state per shaper instance (held sample, decimator phase). Add
  `float crush_st[2]` to the per-voice runtime struct (`VoiceRt`) and one for
  the master stage. Keep a stateless wrapper passing null so existing call
  sites still compile.
- **Option text is sized for the grid's enum box:** two lines of three
  characters. "SAT", "BFZ", "PDIST" were picked partly for that. Do not rename.

### The dead-zone fix, and the trap under it

Gus's complaint was *"Distortion only kinda kicks in around 53"*. The drive pot
is `(0.2, 8) EXP`, so unity sits at **pot 55** and the bottom 43% of the knob
only attenuates. 9W9 uses `(0.85, 12) EXP`, unity at pot ~7.8.

**This breaks saved patches if you rush it.** In 6W6/8W8 the state blob stores
pot positions and enum selections **by table index**
(`{"v":1,"pots":[...],"enums":[...]}`, see `sd606_serialize`). Changing a pot's
range silently changes what every stored position means; growing an enum's
option count silently changes every stored selection.

So the port **must** include a migration:

1. Bump `SD606_STATE_VERSION` / `SC808_STATE_VERSION` to `2`.
2. **Read the `"v"` field in `deserialize` — the current code parses the blob
   and ignores the version.** Add that first.
3. When `v == 1`:
   - **Drive pots:** recover `0.2 * (8/0.2)^(pot/127)`, then re-solve
     `127 * ln(value/0.85) / ln(12/0.85)`, clamped 0..127. Old pot 55
     (value 0.988) maps to new pot ~7, preserving the sound. Old positions
     below ~52 were attenuating settings the new range cannot express; they
     clamp to 0, which matches 9W9.
   - **`*_dist_type`:** 0→0, 1→1, **2→5** (Fold), **3→6** (Crush).
   - **`master_dist`:** 0→0 (Off), 1→1, 2→2, **3→6**, **4→7**.
4. Enum counts: `*_dist_type` 4→7, `master_dist` 5→8. `deserialize` clamps
   `v >= count` to `count-1`, so without step 3 every old "Crush" patch comes
   back as something else.

### Defaults

Set each drive pot's default to the position nearest unity (**pot 8** = 1.0043
on the new range). Measure what that 0.43% does at defaults and report the
number. If Gus wants bit-exact defaults, the min can be nudged to 0.84617 so
unity lands exactly on pot 8 — ask first, it makes the curve differ slightly
from 9W9's.

### Also update

`scripts/gen_params.py` (the `DIST` list and `DRIVE()` range — it is the single
source of truth, regenerate `*_params.h`, never hand-edit), `src/web_ui.html`.

---

## 2. The Main-page jog lock

Self-contained, entirely in `src/ui_chain.js`. 6W6's and 8W8's chain UI are
structurally the same file as 9W9's, so this is close to a copy.

**Behaviour:** a jog **click while already on the Main/root page** toggles a
lock. While locked, pads still play and still record, but the page stops
following them — so the master knobs stay under your hands while you jam.
`Shift+Pad` still navigates (an explicit "take me there"). Another click
unlocks. The title bar shows `[L]`.

Port from 9W9's `src/ui_chain.js`:

- `mainLocked()` — reads `globalThis.__9w9_main_lock`. **It must live on
  `globalThis`**, renamed per module (`__6w6_main_lock`). The host re-evaluates
  this file every time the editor opens, so module-level state resets and the
  lock would appear to drop itself.
- `onMainPage()` — true when `page.level === "root" || page.level == null`.
- In `title()` — append `" [L]"` when locked, before the existing `[M]` logic.
- In the pad handler, **after** the Mute+Pad branch and **before** Shift:

```js
if (mainLocked() && !shiftHeld()) {
    if (!isPageOnlyPad(d1)) injectToMove(data);
    return;
}
```

- In the "everything else" branch, intercept the click **before**
  `applyInput`, or the section picker eats it:

```js
if (intent.type === "click" && !controller.pickerOpen && onMainPage()) {
    globalThis.__6w6_main_lock = !globalThis.__6w6_main_lock;
    return;
}
```

- Add a line to the first-run hint.

**Verification.** 9W9 has a headless harness that loads a sed-rewritten copy of
`ui_chain.js` against a stubbed host and asserts the lock arms, blocks page
changes, shows `[L]`, honours Shift+Pad, and unlocks. 6W6 already has
`test/ui_chain.test.mjs` to extend.

---

## 3. Send FX — DSP only, UI deferred

**Build the engine, not the pages.** Do not add these to the page hierarchy,
the pad map or the web panel yet.

Two send buses, summed per sample from each voice through a per-voice send
amount, returned into the mix **before** the master distortion and Comp so
those work on the wet signal too.

**Every voice except the kick gets sends.** The kick stays dry — Gus's explicit
call in 9W9. Flag it for these kits rather than assuming.

> **Reversed for 6W6.** Asked directly, Gus wanted reverb and delay on *all*
> instruments including the kick. So flag it, do not copy it: the rule is "ask",
> not "the kick is dry". 6W6 ships `bd_rev`/`bd_dly` like every other voice.

Copy `er99_verb_t` / `er99_dly_t` and their ticks from `src/dsp/er99_engine.h`
and `er99_engine.c`:

- **Reverb:** 4 combs (1116, 1188, 1277, 1356) into 2 allpasses (556, 441),
  loop quantised to 12 bits. Params: Decay, Tone (loop damping), HPF, Level.
- **Delay:** one line, `ER99_DLY_MAX 88200` (2 s at 44.1 k), slewed read so time
  changes warp the echo instead of clicking, one-pole darkening in the
  feedback, 12-bit writes. Params: Time, Fdbk, Tone, HPF, Level.

### Delay Time is a note division, not milliseconds

13 divisions — 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T,
1/4., 1/2, 1/2. — in beats:

```c
{ 0.125, 1/6.f, 0.25, 1/3.f, 0.375, 0.5, 2/3.f, 0.75, 1.0, 4/3.f, 1.5, 2.0, 3.0 }
```

`dly_time` is therefore an **enum**, not a pot — it must be in the enum table
and in `is_enum_key()`, or the pot layer rescales it into nonsense.

The plugin feeds tempo per block from `g_host->get_bpm()`, writing a `dly_bpm`
raw key only when the value changes; the engine recomputes from
`beats * 60000 / bpm`, clamped to the line length. Default is the dotted eighth.

### Three bugs already hit — do not re-introduce

1. **The delay read can index one past the buffer.** The read pointer is a
   float; at 36000 the float spacing is 1/256, so a read a hair under the wrap
   point rounds **up** to exactly `DLY_MAX` and reads out of bounds, leaking
   the write counter into the audio as denormals. Clamp after the cast:
   `if(i0 >= DLY_MAX) i0 -= DLY_MAX;`
2. **`floorf` in a 12-bit feedback loop injects DC** — it biases every pass by
   -0.5 LSB, and a DC-fed comb loop settles into a -70 dB hum that never
   decays. Use `truncf`: same grain, always shrinks magnitude.
3. **Flush denormal feedback state to zero** (`if(fabsf(lp) < 1e-20f) lp = 0;`)
   or the tail never truly ends.

### Params and proof

Add the send pots (`<voice>_rev`, `<voice>_dly`, 0..1 LIN) and the nine FX
params by **appending** to the tables. Appending is safe; inserting or
reordering breaks every saved patch, because the blob is positional.

- **Sends at zero must be bit-identical to `HEAD`.** This is the proof Gus
  cares about most.
- Echo lands at the right millisecond for the division and tempo (9W9: 1/8 at
  120 BPM = 250.1 ms; 1/4 at 140 BPM = 428.6 ms, theory 428.6).
- Reverb leaves tail energy where the dry voice is dead, and decays to silence.
- Both HPFs measurably cut lows on the wet path.

---

## 4. Velocity — check this even if you port nothing else

Look at what `<engine>_trigger()` does with its velocity argument. In 9W9 it was

```c
const float accent = velocity >= ACCENT_VELOCITY ? master.accent : 1.0f;
```

— the 909's accent *switch*, modelled faithfully. Driven from Move's sequencer
that means every velocity from 1 to 99 is one level and everything from 100 up
is another: two flat shelves with a 6 dB cliff between. Users report it as
"velocity does nothing", and they are right. **Check whether 6W6/8W8 do the
same thing** — if so it is the same bug.

The law that survived, after three wrong ones:

```c
const int   vi = velocity < 0 ? 0 : (velocity > 127 ? 127 : velocity);
const float vgain = master.accent
                  * (1.0f - master.vel_depth
                            * (1.0f - (float)vi * (1.0f/127.0f)));
```

Accent is the **top**: a full-velocity hit reaches it whatever Velocity is set
to, and Velocity is how far below it a soft hit falls.

Four properties, each worth a loadtest check, because each was a separate bug:

1. **`vel_depth` 0 is a flat response** — every velocity, including whatever
   Move's Full Velocity sends, gives the identical sample. The first cut faded
   only the sub-threshold half and left the switch live, so 0 still jumped 6 dB.
2. **Velocity never boosts.** A full-velocity hit is the same at every depth.
   The second cut pivoted mid-range, so turning Velocity up made hard hits 6 dB
   *louder* — moving the kit's loudness, not just its dynamics.
3. **Anchor at Accent, not at 1.0.** The third cut anchored at 1.0 and deleted
   the Accent pot. That looks tidy and quietly drops the whole kit 6 dB,
   because 1.0 is the *unaccented* level and a sequencer pattern had always
   been playing at the accented one. Keeping Accent as the top means
   `vel_depth = 0` is **bit-identical** to the pre-velocity release for any
   velocity at or above the old threshold — worth asserting.
4. **Monotonic, with no step** at the old threshold.

**6W6 went one step further, and it is compatible with rule 3.** It deleted the
Accent pot outright and kept the *number* as a constant:

```c
#define SD606_FULL_VELOCITY_GAIN 1.9921260f   /* the old Accent default */
const float accent = SD606_FULL_VELOCITY_GAIN
                   * (1.0f - e->potv[e->p_vel_depth]
                             * (1.0f - (float)vi * (1.0f/127.0f)));
```

Rule 3 says "anchor at Accent, not at 1.0", and the trap it warns about is
anchoring at 1.0 — not keeping the pot. Freezing Accent at its default and
anchoring there satisfies it: a full-velocity hit lands exactly where an
accented hit always did, so nothing gets quieter. What it costs is the ability
to move that ceiling, which on a machine whose accent is a fixed bus level is
arguably correct. What it *buys* is that velocity becomes the only loudness
control, with no second one fighting it.

Deleting the pot is the expensive half, and not for the reason you would guess
— see §7.2.

Check where the gain is applied. In 9W9 every circuit voice uses it as a
post-distortion output gain, so it changes level only. If a voice applies it
before its shaper (9W9's samplers do), velocity also nudges the timbre — under
a dB, arguably more musical, but know which you have.

---

## 5. Voice-qualified names, but only where they help

Schwung's LFO target picker lists a module's params as one flat list. Eight or
eleven voices each contributing a "Decay" leaves the user unable to tell which
pad they are about to automate. The fix is to prefix the names — but naively
prefixing everything also clutters the knob grid, which draws the param under a
page header that already says CLOSED HAT.

There is a seam. `shared/param_pages/param_meta.mjs` merges the two sources as
`{...inlineHierarchyMeta, ...chainParamsMeta}` and then resolves
`meta.label = meta.label || meta.name || prettify(key)`. chain_params spells the
display string **name**; inline hierarchy entries spell it **label**. So a
label on the hierarchy entry both survives the merge and wins the fallback:

- **chain_params name** — voice-qualified: "CH Decay", "OH Decay". This is what
  the LFO picker shows.
- **hierarchy label** — bare: "Decay". This is what the page draws.

`validate_contract.mjs` checks `<level>.<key>.label`, so this is what the
contract asked for anyway; 9W9's generator had been emitting name there.

Do it as a post-processing pass in `gen_params.py` over the finished lists, not
by threading a prefix through every helper. Assert afterwards that no two
chain_params names collide — that assertion is the point of the change.

To verify without driving the device UI, copy `param_meta.mjs` and
`param_format.mjs` off the device and run
`buildMetaIndex({hierarchy, chainParams})` in node against your generated JSON.
It resolves the labels with the real code and prints what each page will draw.

---

## 6b. The host's trailing pages — My Presets and Module — on a module-owned grid

Every sound generator on the stock hierarchy editor (Tablor, for one) gets two
pages appended by the host: **My Presets** (Preset / Save / Save As / Delete)
and **Module** (Help / Add to List / Swap / Remove). A module that ships its
own `ui_chain.js` — all four drum machines — got neither, because it builds its
own controller and the host only ever handed those pages to its own.

Fixed host-side in **charlesvestal/schwung PR #396**. Until that ships, the
pages exist only on a patched host; wire your side anyway — every call below is
guarded, so on a stock host nothing changes and nothing appears.

**Hardware-verified on 9W9** through the whole flow: pages appear, Save As
raises the keyboard over the grid, Back steps out of the menu, Load/Delete/Swap/
Help come back to the module. Every item below exists because the first cut got
it wrong on the device.

### What the host gives you

Two bindings, installed beside `host_swap_module` with slot and component
already applied, plus two optional hooks it calls on your `chain_ui`:

| | |
|---|---|
| `shadow_component_trailing_menus()` | `[{name, entries}]` — feed to the controller as `io.trailingMenus` |
| `shadow_component_run_action(action)` | perform a row's action; `true` if a screen opened over you |
| `chain_ui.onPresetsChanged()` | called after a Save/Load while your grid is on screen |
| `chain_ui.restorePage(name, {enter})` | called after the host reloads you following Load/Delete/Swap/Help |

### The six things your `ui_chain.js` needs

1. **Ask for the pages** in `createController`:
   ```js
   trailingMenus: () => (typeof shadow_component_trailing_menus === "function"
       ? (shadow_component_trailing_menus() || []) : []),
   ```
2. **Draw them.** They arrive as `PAGE_MENU`, which the library draws itself.
   If your render guard admits only `PAGE_KNOBS` you will print your
   unsupported-page fallback over a page the library was about to draw.
   Import `PAGE_MENU` and admit it.
3. **Dispatch a row's action — from the ENTRY, not the intent.** A row
   activation comes back from `applyInput` as `{ action: "menu", entry }`.
   `"menu"` is the intent's *kind*; the key you want is `entry.action`.
   Handing the host the word "menu" runs nothing, silently — that was bug one.
   ```js
   if (todo && todo.action === "menu") {
       var act = todo.entry && todo.entry.action;
       if (act && typeof shadow_component_run_action === "function")
           shadow_component_run_action(act);
       return;
   }
   ```
4. **Climb the Back ladder.** The host consumes Back and calls your
   `handleBack()` first, so you have to climb the same rungs
   `page_input.mjs`'s `case "back"` does, in its order — or Back from inside
   My Presets leaves the module and skips the page bar. That was bug two.
   ```js
   if (controller.dismissHint && controller.dismissHint()) return true;
   if (controller.dismissPeek && controller.dismissPeek()) return true;
   if (controller.pickerOpen) { controller.closePicker(); return true; }
   if (controller.exitMenu && controller.exitMenu()) return true;   // out of the menu, not the module
   return false;                                                    // host exits the editor
   ```
5. **Refresh after a save**, or the Preset row goes on reading `(none)`:
   ```js
   onPresetsChanged: function () {
       if (controller && controller.refreshTrailing) controller.refreshTrailing();
   },
   ```
6. **Land where you left from** after the host reloads you:
   ```js
   restorePage: function (name, opts) {
       if (controller && controller.restorePage) controller.restorePage(name, opts || {});
   },
   ```
   The controller keeps the request armed until its pages arrive, so a
   contract still settling after the reload is fine.

### The gesture that had to move

Any lock or action you hung on a **plain jog click** now collides: on the last
two pages a plain click is how a row is activated, and on Main it opens the
section list. 9W9's Main-page lock moved to **Shift + jog click**. If your
device editor uses a plain click for anything of its own, it has to move too —
that gesture belongs to the platform now.

### What was wrong in the host, so nobody rediscovers it

Two defects in the first cut, both found only on hardware:

- `runComponentActionFromGrid` decided "did this action open a screen?" with
  `view !== VIEWS.PARAM_PAGES`, which is only right when the caller *was* the
  host's param pages. From a module grid every action looked like a hand-off,
  armed a return to a grid the module cannot host, and the device spun on
  `synth:ui_hierarchy` reads. Now compared against the view at entry.
- The return paths re-entered through `enterParamPages` regardless of who
  asked. They now record the origin and come back through
  `enterComponentEditFallback` for a module — the door the host opens a module
  UI with — then call `restorePage` so you land where you left from.

Generalisable rule, and the reason both were missed: **a host function that
reads `view` to decide what it did is coupled to whoever called it.** Any host
path a module-owned grid can now reach needs checking for that assumption.

---

### 6b.1 Swap Module into your module: answer `ui_hierarchy` with "" (not an error)

Reported from hardware on 9W9: stock grid → Module → Swap → pick 9W9 → click
sat on the host's "Loading..." card until the user pressed Back. The host's
component load gate (`shared/component_load_gate.mjs`) reads `<prefix>:ui_hierarchy`
with THREE answers: JSON = declared, `""` = served and empty (fall back to
`ui_chain.js` at once), null/error = the read did not complete (hold and ask
again). Our plugins returned `-1` for the key, which is the third answer, so the
gate waited forever.

Do this in your `get_param`:

```c
if(!strcmp(_key, "ui_hierarchy")) { if(_len < 1) return -1; _buf[0] = 0; return 0; }
```

and flip the loadtest assertion from `m < 0` to `m == 0 && buf[0] == 0`.
The normal entry path is unchanged: `getComponentHierarchy` treats `""` as
"no hierarchy" and loads `ui_chain.js` exactly as before.

The host side (PR #396) also got two fixes you need for the flow to work at all:
a completed swap now re-enters the NEW module through `openComponentEditor`
instead of restoring the old grid, and the gate falls back after three failed
reads on a named, not-loading module. With the `""` answer above your module
opens instantly; the gate rule is only the backstop for modules that do not.

### 6b.2 Step buttons: toggle on a TAP's release, never on the press

Schwung's parameter locks use a HELD step as the modifier: hold step 9, turn a
knob, and the value belongs to step 9 (Schwung `host/lock_common.h`). The
captured step note still reaches your `on_midi` — the shim adds a listener, it
does not divert — so a sequencer that toggles on the press flips a trig under
every lock the user places.

9W9 records the press and toggles on the release only if the hold was shorter
than a tap (`ER99_STEP_TAP_MAX_S`, 300 ms). Both release spellings count: Move
sends note-off and note-on with velocity 0. The one-shot filter that drops
note-offs has to come AFTER the step branch, or the release never arrives.
`loadtest.c` asserts all three: press alone does nothing, tap toggles, hold
does not.

## 6. Deployment gotchas

- Never `scp` over a live `dsp.so` — the shim has it `dlopen`ed, and
  overwriting mutates the running process's mapped pages and takes down the
  firmware. Upload to `.new`, then `mv`. `deploy.sh` already does this; do not
  "simplify" it.
- **Check that `deploy.sh` copies `web_ui.html` and `help.json`.** In 9W9 it did
  not, for months — every deploy shipped new DSP beside a remote panel from
  whenever the module was last installed from a tarball, so the browser editor
  kept showing controls the engine no longer had. Verify with `ls -la` on the
  device that the timestamps move.
- The Move's DHCP address changes on reboot. Re-resolve with `ping move.local`
  and update the `movedevice` entry in `~/.ssh/config`.
- Run the on-device loadtest after deploying, and add checks for what you
  added — it dlopens the real `.so` exactly as the chain host does. **That is
  not proof the device is running it** — see §7.4, which is the single most
  expensive process trap in this document.
- Reboot with `schwung-heal --reboot`, never a plain `reboot`, and verify
  `/proc/uptime` reset (§7.4).

---

## 7. What the 6W6 port actually hit

Six traps, none of them predicted above. Two shipped broken and were found by a
user rather than a test. Written from the 6W6 port; 8W8 is the same shape of
codebase and should assume all six apply.

### 7.1 Drive at zero must be *transparent*, not merely quiet

§1 moves the drive range so unity sits near pot 8. That leaves pots 1..7 as
settings that still shape the signal, so "Drive off" is not off — a user turning
Drive down gets a quieter distortion, not a clean voice. 6W6 added an explicit
dry path plus a crossfade over the bottom of the knob:

```c
const int dpot = e->pot[s.drive];
if(dpot <= 0) shaped = raw;                    /* exactly dry, not approximately */
else {
    shaped = sd606_shape_st(raw, e->potv[s.drive], e->env[s.dist], r.crush_st);
    if(dpot < SD606_DRIVE_WET_POT)             /* 8 */
        shaped = raw + ((float)dpot / (float)SD606_DRIVE_WET_POT) * (shaped - raw);
}
```

Prove it with a null test, not by ear: at pot 0 the output must be
**bit-identical** to the pre-distortion build.

The same rule applies at the master stage, and there it is also why a knob
sweep can look dead: 6W6 gates on `if(mdist > 0 && mpot > 0)` and `master_dist`
defaults to Off, so `master_drive` does nothing until a type is chosen. Correct,
and it will read as a broken knob to anyone testing it cold — see §7.6.

### 7.2 The positional state blob cannot survive a deletion

This is the big one, and §1 only half-warns about it. §1 says changing a pot's
*range* breaks saved patches. Deleting a pot is worse: the blob stores raw pot
positions **by table index**, so removing one shifts every entry after it and an
old patch loads as a different kit — different tuning, different decays, silently.

6W6 deleted three pots across two releases (`bd_drift`, `sd_decay`, then
`accent`). What works:

- **Never insert. Only append.** A new pot goes on the END of the registration
  order, whatever the page order is. 6W6's `gen_params.py` registers in historic
  order and lets pages list keys in whatever order reads well.
- **Migrate by NAME, not by index.** Keep the historic key tables embedded in
  the engine — `kV1PotKeys[40]`, `kV2PotKeys[64]` — and for any blob older than
  the current version, scatter values by looking each old key up in the current
  table. A key that no longer exists simply has nowhere to land.
- **A short blob is not an error.** Deserialize must apply the entries it has
  and leave the rest at their creation defaults, so a patch saved before a pot
  was appended still loads. Do NOT reset the whole table first — that would make
  a short blob clobber everything it does not mention.
- Assert both directions in the loadtest, building a real old blob rather than
  relabelling a current one. Relabelling only proves the code agrees with itself.

The payoff: when `sd_decay` came *back* by user request two releases later, it
was appended at index 63, and v1/v2 patches — which still carry an `sd_decay` —
restored it by name automatically.

### 7.3 Your binding must call `controller.tick()`

**The most expensive bug of the whole port, and it is one line.**

`ui_chain.js` owns the grid's heartbeat. `controller.tick()` is what advances
the library's VALUE CURSOR — it reads one param per tick around the page
(a bulk refresh was measured at ~186 ms/cycle, hence one at a time). Drop the
call and `state.values` stays `{}` forever.

Nothing throws. The page still draws. So it does not present as a crash, it
presents as a **data** regression:

- every enum box renders **completely EMPTY** — `render_page_movy.mjs`'s last
  resort is `String(shown ?? "")`, and `shown` is `undefined`
- every knob silently reads 0

6W6 lost the line while deleting unrelated code from `tick()`. The hunt then
burned a session on the DSP, the deployed files, the slot number,
`param_meta.mjs`'s `label`-vs-`name` merge and the shared library — all
innocent. Put it back where 8W8 has it, immediately after `setReveal`.

Diagnosis shortcut worth more than the fix: run the binding offline against the
real `param_pages` library with `shadow_get_param` logged. **If the only reads
are `ui_pages` / `chain_params` / `mutes` and never a param key, the cursor is
not running.** `test/ui_chain.test.mjs` now asserts every Main-page cell — and
every enum cell specifically — holds a non-empty value, and that guard is
verified to fail when the line is removed.

This matters directly for §6b: you are about to make six edits to `tick()` and
`handleBack()`. It is exactly the file and exactly the kind of edit that lost it.

### 7.4 A new `dsp.so` on disk is not the running one, and the slot is not 0

§6 says run the loadtest after deploying. That is not sufficient, and the way it
fails is nasty: the on-device loadtest `dlopen`s the file **itself**, so it
passes against the new build while the chain host keeps the old inode mapped.
**Green tests, stale code.** `kill shadow_ui` does not help — different process.

Force a real module reload, and find the slot **by module id**:
`scripts/reload_slot.py <host> -1 <module-id>` scans the slots and asks the one
actually running the module to re-`dlopen`. 6W6's `deploy.sh` hardcoded slot 0;
the moment another module occupied it, every reload for a whole session silently
did nothing.

Two more, both learned the hard way:

- **A plain `reboot` over ssh fails silently on the Move.** It returns cleanly
  and the box keeps running. Use `/data/UserData/schwung/bin/schwung-heal
  --reboot`, then verify `/proc/uptime` actually reset. Three "reboots" in a row
  did nothing before this was noticed.
- `reload_slot.py` only forces an already-loaded slot to re-load. It cannot load
  a module into an **empty** slot — that is a pick on the device.

### 7.5 `viz: false` on any `*_attack` that is a click level

The device grid's `viz.mjs` auto-pairs an adjacent `*_attack` and `*_decay` with
a matching stem into an AD envelope graphic, and hoists it to the front of the
page. On a 606 or an 808, Attack is a **click level**, not an envelope time — so
the graphic draws an envelope that does not exist and implies a time control
that is really an amount.

Declare `"viz": false` beside `key`/`name`/`type` in chain_params for those
params. 6W6 does; with it the Kick page resolves to a fader on `bd_level` and no
envelope. Check the engine before copying: if a voice's attack really is a time
constant, the graphic is honest and you want it.

Verify with the real resolver rather than by eye — and note it takes an
**object**: `resolveViz({ keys, metaIndex })`. Called positionally it returns
`{groups:[],invalid:[]}`, which is indistinguishable from "clean".

### 7.6 Nothing in these suites asserts a control has an EFFECT

Ranges, names, defaults, key resolution, storage order, both state migrations —
all of it passes with a knob wired to nothing. 8W8 shipped exactly that: a pot
that resolved its slot and reached no voice, dead at 0/64/127, with a green
suite.

`tools/knob_check.cpp` (6W6, ported from the CW-78 session's) renders each
control at both ends of its range and hashes the audio; equal hashes mean the
control changed nothing. 75 controls, all alive.

The hash is the easy half — **the context is the work, and getting it wrong
manufactures failures.** 6W6's first run reported two dead and both were the
harness. CW-78's first run reported 27 dead and 25 were false. Each control has
to be measured where it is *supposed* to work, and that context is a design
claim worth writing next to it:

- a distortion **type** does nothing while Drive is 0 — and Drive may default to 0
- 6W6's `master_drive` does nothing while `master_dist` is Off — the type gates
  the drive, the inverse of the above
- a send does nothing while the bus it feeds is silent
- `vel_depth` does nothing at velocity 127, by design
- **`hh_choke` needs a context that is not a parameter but TIME**: strike OH,
  let it ring 100 ms, *then* strike CH. Both in the same sample and the choke
  has nothing to cut, and reads as dead

Mutation-test the probe or it proves nothing: stub a voice so a pot still
resolves and still stores but never reaches it. `knob_check` must report DEAD
while `loadtest.c` reports ALL PASS on the same build. That gap is the point.
