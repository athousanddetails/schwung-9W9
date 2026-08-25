#!/usr/bin/env bash
#
# Install the ER-99 module on the Move SAFELY.
#
# Critical: never scp directly over a live dsp.so. The shim dlopen()s it into
# MoveOriginal, so overwriting the file mutates the mmap'd code pages of a
# running process — which segfaults the whole firmware (observed 2026-08-14:
# "SIGSEGV si_addr=0x... pc=<same>", i.e. a jump into garbage).
#
# Upload to a temp name, then mv. rename(2) is atomic and swaps the directory
# entry, leaving the old inode intact for the running process. The new code is
# picked up when the slot next loads the module.
#
#   ./scripts/deploy.sh [host]      (default: movedevice)
set -euo pipefail

HOST="${1:-movedevice}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="/data/UserData/schwung/modules/sound_generators/9w9"
BUILD="$SRC/build/dsp.so"

[ -f "$BUILD" ] || { echo "no build/dsp.so — run ./scripts/build.sh er99 first" >&2; exit 1; }

echo "==> $HOST:$DEST"
ssh "$HOST" "mkdir -p $DEST/samples"

scp -q "$BUILD" "$HOST:$DEST/dsp.so.new"
scp -q "$SRC/src/module.json" "$HOST:$DEST/module.json.new"
scp -q "$SRC/src/ui_chain.js" "$HOST:$DEST/ui_chain.js.new"
scp -q "$SRC/src/movy_config.json" "$HOST:$DEST/movy_config.json"
# The remote panel and the on-device help are served straight off these files.
# They were missing here for months: every deploy shipped new DSP with the
# panel from whenever the module was last installed from a tarball, so the
# browser editor still showed controls the engine had stopped having.
scp -q "$SRC/src/web_ui.html" "$HOST:$DEST/web_ui.html.new"
scp -q "$SRC/src/help.json"   "$HOST:$DEST/help.json.new"
scp -q "$SRC"/src/samples/*.wav "$HOST:$DEST/samples/"

# Atomic swap. Do NOT replace this with a direct scp.
ssh "$HOST" "cd $DEST && mv -f dsp.so.new dsp.so && mv -f module.json.new module.json && mv -f ui_chain.js.new ui_chain.js && mv -f web_ui.html.new web_ui.html && mv -f help.json.new help.json && chmod 755 dsp.so && ls -l dsp.so ui_chain.js web_ui.html help.json"

if [ -f "$SRC/src/patches/9W9.json" ]; then
    scp -q "$SRC/src/patches/9W9.json" "$HOST:/data/UserData/schwung/patches/9W9.json.new"
    ssh "$HOST" "cd /data/UserData/schwung/patches && mv -f 9W9.json.new 9W9.json && rm -f ER-99.json"
fi

echo "==> done. Reload the slot (or restart the Shadow UI) to pick up new code."
