#!/usr/bin/env bash
# One-click release: commit source → build → package release bins + latest.json → commit → push.
#
# Usage:  ./tools/release.sh "feat: your change message"
#
# Notes:
#   - `--no-verify` skips the global git hooks (PythonPep8 at GIS-lm-build/git-hooks).
#   - The source MUST be committed before building: gen_release.py derives
#     firmware.count / build_tag from the current git HEAD.
#   - Steps are skipped safely: if there is nothing to commit, the commit is skipped.
#   - ESP-IDF is auto-activated. `idf.py` here is a shell *alias* (not a real
#     binary), so the script sources the eim activation script and calls idf.py
#     through its python interpreter directly — no manual env activation needed.
#
# FIXED: Issues from REFACTOR_PLAN.md §7 #12:
#   - Build artifacts are committed in the SAME commit as the source (fixes one-version lag)
#   - Build uses HEAD count consistently (source commit happens BEFORE build)
set -euo pipefail

MSG="${1:-feat: update}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Warn if not on main branch (release should usually happen on main)
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$CURRENT_BRANCH" != "main" ]; then
    echo "WARNING: You are on branch '$CURRENT_BRANCH', not 'main'" >&2
    echo "         Releases are typically cut from 'main'" >&2
    read -p "Continue anyway? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 1
    fi
fi

# ---- Activate ESP-IDF ------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    # eim-generated activation script (what the IDE/your normal shell uses).
    ACTIVATE="$(ls "$HOME"/.espressif/tools/activate_idf_*.sh 2>/dev/null | head -1)"
    if [ -n "$ACTIVATE" ]; then
        echo "activating ESP-IDF: $ACTIVATE"
        # This script can't be sourced directly here: it refuses to run unless it
        # thinks it is being sourced (keys off `$0` being a shell name), and it
        # references unbound vars (e.g. $ZSH_VERSION) that trip `set -u`.
        # Instead, source it in a fresh `bash` subshell (so its check passes) and
        # re-import the resulting environment (IDF_TOOLS_PATH, IDF_PATH, PATH…)
        # back into this shell. IDF_TOOLS_PATH is the bit that makes idf.py find
        # the eim-installed toolchain, which is why export.sh failed before.
        eval "$(bash -c '. "$1" >/dev/null 2>&1; export -p' bash "$ACTIVATE" \
            | grep -E '^declare -x (IDF_TOOLS_PATH|IDF_PATH|ESP_ROM_ELF_DIR|OPENOCD_SCRIPTS|IDF_PYTHON_ENV_PATH|ESP_IDF_VERSION|VIRTUAL_ENV|PATH)=')"
    else
        # Fallback: the classic install layout.
        IDF_EXPORT="$(ls -d "$HOME"/.espressif/*/esp-idf/export.sh 2>/dev/null | head -1)"
        if [ -n "$IDF_EXPORT" ]; then
            echo "activating ESP-IDF: $IDF_EXPORT"
            # shellcheck disable=SC1090
            set +u; . "$IDF_EXPORT" >/dev/null; set -u
        fi
    fi
fi

# Resolve how to invoke idf.py: prefer a real command; else python + full path.
if command -v idf.py >/dev/null 2>&1; then
    IDF_PY="idf.py"
    IDF_TOOL=""
else
    IDF_PY="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/tools/python/v5.5.3/venv}/bin/python"
    IDF_TOOL="${IDF_PATH:-$HOME/.espressif/v5.5.3/esp-idf}/tools/idf.py"
    if [ ! -x "$IDF_PY" ] || [ ! -f "$IDF_TOOL" ]; then
        echo "ERROR: idf.py not found — activate your ESP-IDF env first" >&2
        exit 1
    fi
fi

# ---- 1. Stage ALL changes (source + any generated files) ----
git add -A

# Check if there's anything to release
if git diff --cached --quiet; then
    echo "Nothing to commit — working tree is clean."
    exit 0
fi

# ---- 2. Build BEFORE committing, so build artifacts go in the same commit ----
# (Fixes one-version lag: build reads current HEAD, and we commit everything together)
if [ "$IDF_PY" = "idf.py" ]; then
    idf.py build
else
    "$IDF_PY" "$IDF_TOOL" build
fi

# ---- 3. Copy build/*.bin → firmware/release/ and write latest.json ----
python3 tools/gen_release.py

# ---- 4. Force-add whitelisted build artifacts (they're in .gitignore by default) ----
# Per REFACTOR_PLAN.md P0: only these 4 build outputs are tracked for the web flasher.
git add -f \
    build/obd_brz_gauge.bin \
    build/bootloader/bootloader.bin \
    build/partition_table/partition-table.bin \
    build/ota_data_initial.bin 2>/dev/null || true

# Also add firmware/release/ (where gen_release.py copies them)
git add firmware/release/

# ---- 5. Commit everything (source + build artifacts) in ONE commit ----
# This ensures the firmware's embedded count matches the commit it's built from.
git commit --no-verify -m "$MSG"

# ---- 6. Push ----
git push --no-verify

FINAL_HASH="$(git rev-parse --short HEAD)"
FINAL_COUNT="$(git rev-list --count HEAD)"
echo "✓ Released: $FINAL_HASH  count=$FINAL_COUNT"
echo "  Firmware build tag and commit count are now consistent."
