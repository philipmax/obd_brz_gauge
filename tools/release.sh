#!/usr/bin/env bash
# One-click release: commit source → reconfigure + build → package release bins
# + latest.json → commit artifacts (build:) → push.
#
# Usage:  ./tools/release.sh "feat: your change message"
#
# Notes:
#   - The source is committed FIRST and the build is reconfigured so CMake re-reads
#     HEAD: the firmware's build tag / count and latest.json then all name the
#     source commit N. The artifacts follow in a separate `build:` commit (N+1),
#     as AGENTS.md requires. gen_release.py refuses a firmware whose embedded
#     build tag differs from HEAD (the old "one release behind" bug).
#   - With a clean tree the source commit is skipped and HEAD is released as is.
#   - `--no-verify` skips the global git hooks (PythonPep8 at GIS-lm-build/git-hooks).
#   - ESP-IDF is auto-activated. `idf.py` here is a shell *alias* (not a real
#     binary), so the script sources the eim activation script and calls idf.py
#     through its python interpreter directly — no manual env activation needed.
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

idf() {
    if [ "$IDF_PY" = "idf.py" ]; then idf.py "$@"; else "$IDF_PY" "$IDF_TOOL" "$@"; fi
}

# ---- 1. Commit the source (never the build artifacts) ----
git add -A
git reset -q -- build/ firmware/release/ 2>/dev/null || true
if git diff --cached --quiet; then
    echo "No source changes — releasing HEAD as is."
else
    git commit --no-verify -m "$MSG"
fi

# ---- 2. Build from HEAD (reconfigure: CMake reads the git count at configure time) ----
idf reconfigure build

# ---- 3. Copy build/*.bin → firmware/release/ and write latest.json (checks the build tag) ----
python3 tools/gen_release.py

# ---- 4. Commit the whitelisted build artifacts (.gitignore keeps build/* out by default) ----
git add -f \
    build/obd_brz_gauge.bin \
    build/bootloader/bootloader.bin \
    build/partition_table/partition-table.bin \
    build/ota_data_initial.bin \
    build/bootmedia.bin
git add firmware/release/
SRC_COUNT="$(git rev-list --count HEAD)"
SRC_HASH="$(git rev-parse --short=12 HEAD)"
if git diff --cached --quiet; then
    echo "Artifacts unchanged — nothing to commit."
else
    git commit --no-verify -m "build: release firmware count ${SRC_COUNT} (${SRC_HASH})"
fi

# ---- 5. Push ----
git push --no-verify

echo "✓ Released firmware count=${SRC_COUNT} git=${SRC_HASH} (artifacts at $(git rev-parse --short HEAD))"
