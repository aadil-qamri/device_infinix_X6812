#!/usr/bin/env bash
# Host-side sepolicy validation without a build.
# Run from anywhere; locates $ANDROID_ROOT itself.
set -uo pipefail

TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$TREE/../../.." && pwd)"
HOSTBIN="$ROOT/out/host/linux-x86/bin"

[ -x "$HOSTBIN/checkpolicy" ] || { echo "FAIL: $HOSTBIN/checkpolicy missing (no build output)"; exit 1; }

extract_rule() {
    local module="$1" out="$2"
    MODULE="$module" OUT="$out" ROOT="$ROOT" python3 - <<'PY'
import glob, os, sys
mod = '# Module:  ' + os.environ['MODULE']
root = os.environ['ROOT']
candidates = [p for p in sorted(glob.glob(os.path.join(root, 'out/soong/build.lineage_*.ninja')))
              if mod in open(p, errors='replace').read()]
if not candidates:
    sys.exit('FAIL: rule for %s not found in any split ninja; out/ is stale, ask for a build'
             % os.environ['MODULE'])
lines = open(candidates[0], errors='replace').read().splitlines()
start = next(i for i, l in enumerate(lines) if l.strip() == mod)
for i in range(start, start + 40):
    if lines[i].startswith('    command = '):
        cmd = lines[i].split('command = ', 1)[1]
        j = i + 1
        while j < len(lines) and lines[j].startswith('        ') and '=' not in lines[j][:20]:
            cmd += lines[j].strip()
            j += 1
        open(os.environ['OUT'], 'w').write(cmd.split(' # hash')[0] + '\n')
        break
else:
    sys.exit('FAIL: command line for %s not found in %s' % (os.environ['MODULE'], candidates[0]))
PY
}

cd "$ROOT" || exit 1

for spec in \
    "vendor_sepolicy.conf|$ROOT/out/soong/.intermediates/system/sepolicy/vendor_sepolicy.conf/android_common/X6812/vendor_sepolicy.conf|-C -M" \
    "sepolicy_neverallows.checkpolicy.conf|$ROOT/out/soong/.intermediates/system/sepolicy/sepolicy_neverallows.checkpolicy.conf/android_common/sepolicy_neverallows.checkpolicy.conf|-M"
do
    module="${spec%%|*}"
    rest="${spec#*|}"
    conf="${rest%%|*}"
    flags="${rest##*|}"

    echo "=== $module"
    if [ ! -f "$conf" ]; then
        echo "FAIL: $conf missing; out/ is stale, ask for a build"
        continue
    fi

    cmd="$ROOT/out/sepolicy-run-$module.sh"
    mkdir -p "$(dirname "$cmd")"
    if ! MODULE="$module" OUT="$cmd" extract_rule "$module" "$cmd"; then
        continue
    fi

    bash "$cmd" || { echo "FAIL: m4 failed for $module"; continue; }

    out="/tmp/sepolicy-$module.cil"
    rm -f "$out"
    "$HOSTBIN/checkpolicy" $flags -c 30 -o "$out" "$conf"
    if [ ! -s "$out" ]; then
        echo "FAIL: checkpolicy produced no output for $module (its exit code is unreliable)"
    else
        echo "OK: $out ($(wc -c < "$out") bytes)"
    fi
done
