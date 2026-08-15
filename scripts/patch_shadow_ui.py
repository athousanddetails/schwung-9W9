#!/usr/bin/env python3
"""
Patch Schwung's shadow_ui.js to add PAD-FOLLOW for chain modules.

Problem
-------
The hierarchy editor's page never follows what you play. `hierEditorChildIndex`
and `hierEditorLevel` are only ever set from list navigation, so on a drum
module you hit a pad and the editor stubbornly stays on whatever page you left
it on. There is no hook for a module to say "show this page now".

Design
------
A module publishes an optional param:

    get_param("ui_focus_level") -> "<counter>:<level-id>"

`drawHierarchyEditor()` already polls a component param each draw (is_loading),
so we reuse exactly that idiom. The UI navigates only when the COUNTER changes,
i.e. only on a genuinely new trigger — so browsing to another page manually is
never yanked away underneath you.

Modules that don't implement the key are unaffected: the read returns empty and
we bail. No MIDI plumbing, no new SHM, ~25 lines.

Usage
-----
    ./patch_shadow_ui.py <shadow_ui.js> <output.js>     # apply
    ./patch_shadow_ui.py --check <file.js>              # is it patched?
"""
import re
import sys

MARK = "ER99_PAD_FOLLOW"

STATE_ANCHOR = 'let hierEditorChildIndex = -1;    // selected child index for child_prefix levels'
STATE_PATCH = STATE_ANCHOR + f"""
let hierEditorFocusStamp = "";    // {MARK}: last seen module focus counter"""

# Insert right after the existing is_loading poll block inside drawHierarchyEditor.
HOOK_ANCHOR = """    /* Poll is_loading and re-fetch hierarchy + chain_params on the
     * loading→ready transition. */
    {"""

HOOK_PATCH = f"""    /* {MARK}: follow the module's published focus level.
     * A module may expose get_param("ui_focus_level") returning
     * "<counter>:<level-id>". We navigate only when the counter changes, so a
     * new hit moves the page but manual browsing is never overridden. */
    if (!hierEditorEditMode && !hierEditorIsMasterFx && hierEditorSlot >= 0) {{
        const focusPrefix = getComponentParamPrefix(hierEditorComponent);
        const focusRaw = getSlotParam(hierEditorSlot, `${{focusPrefix}}:ui_focus_level`);
        if (focusRaw) {{
            const sep = focusRaw.indexOf(":");
            if (sep > 0) {{
                const stamp = focusRaw.substring(0, sep);
                const lvl = focusRaw.substring(sep + 1);
                if (stamp !== hierEditorFocusStamp) {{
                    hierEditorFocusStamp = stamp;
                    const levels = hierEditorHierarchy && hierEditorHierarchy.levels;
                    if (levels && levels[lvl] && lvl !== hierEditorLevel) {{
                        hierEditorLevel = lvl;
                        hierEditorSelectedIdx = 0;
                        hierEditorChildIndex = -1;
                        hierEditorPresetEditMode = false;
                        hierEditorPath = [levels[lvl].name || levels[lvl].label || lvl];
                        loadHierarchyLevel();
                        invalidateKnobContextCache();
                    }}
                }}
            }}
        }}
    }}

""" + HOOK_ANCHOR


def apply_patch(src: str) -> str:
    if MARK in src:
        raise SystemExit("already patched")
    if src.count(STATE_ANCHOR) != 1:
        raise SystemExit(f"state anchor found {src.count(STATE_ANCHOR)}x, expected 1")
    if src.count(HOOK_ANCHOR) != 1:
        raise SystemExit(f"hook anchor found {src.count(HOOK_ANCHOR)}x, expected 1")
    src = src.replace(STATE_ANCHOR, STATE_PATCH, 1)
    src = src.replace(HOOK_ANCHOR, HOOK_PATCH, 1)
    return src


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        txt = open(sys.argv[2]).read()
        print("PATCHED" if MARK in txt else "NOT PATCHED")
        return
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    src = open(sys.argv[1]).read()
    out = apply_patch(src)

    # sanity: brace balance must be unchanged by our insertion
    def balance(t):
        return t.count("{") - t.count("}")
    if balance(out) != balance(src):
        raise SystemExit("brace balance changed — refusing to write")

    open(sys.argv[2], "w").write(out)
    print(f"patched: {len(src)} -> {len(out)} bytes (+{len(out)-len(src)})")


if __name__ == "__main__":
    main()
