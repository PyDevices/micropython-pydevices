#!/usr/bin/env bash
# Run this driver's probes against a desktop interpreter, the way CI does --
# every planted fault first, so a probe whose failing mode is never exercised
# is not mistaken for a gate.
#
#   tests/run_probes.sh                      # ../bin/micropython (beside the sibling checkouts)
#   tests/run_probes.sh ../bin/micropython.exe
#   tests/run_probes.sh <interpreter> <scratch-dir>
#
# Unlike the workflow beside it, which builds an interpreter from scratch and
# therefore knows exactly what is in it, this runs whatever binary you point
# it at -- and a `bin/` binary is built by hand and goes stale silently.
# On 2026-09-03 that cost a week of green gates in this workspace: a binary
# built thirteen minutes before a C change certified that change for a week.
# So the first thing here is a refusal, not a probe: the binary's
# provenance stamp has to name THIS repository's HEAD, or nothing runs.
#
# A stale binary is not a smaller version of a current one. It is a different
# driver that happens to answer to the same name, and every digest it prints
# is about code you are not looking at.
#
# AUDIOIF_SKIP_PROVENANCE=1 skips the check. It is for a checkout with no
# the workspace beside it, not for a run that would rather not know.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
workspace=$(cd "$repo/.." && pwd)

mp=${1:-$workspace/bin/micropython}
tmp=${2:-${TMPDIR:-/tmp}}

[[ -x "$mp" ]] || { echo "no interpreter at $mp" >&2; exit 1; }

provenance="$workspace/tools/provenance.py"
if [[ "${AUDIOIF_SKIP_PROVENANCE:-0}" == 1 ]]; then
    echo "provenance: skipped by the environment -- this binary may contain any audioif"
elif [[ ! -f "$provenance" ]]; then
    # Say so out loud rather than passing quietly: a skip that reads as a pass
    # is the failure this check is about.
    echo "error: no $provenance, so what is in $mp cannot be established." >&2
    echo "  Set AUDIOIF_SKIP_PROVENANCE=1 if you mean to run it anyway." >&2
    exit 1
elif ! python3 "$provenance" check "$mp" --source audioif; then
    echo >&2
    echo "error: $mp does not contain this repository's HEAD, so its probes" >&2
    echo "  would certify a driver that is not the one on disk." >&2
    echo "  Rebuild it:  $workspace/tools/build_interpreters.sh --only mp-unix" >&2
    exit 1
fi
echo

run() { "$mp" -X heapsize=256M "$here/$1" "${@:2}"; }

# Each of these MUST fail. A suite of only passes cannot tell a working probe
# from one that reports success whatever happens.
echo "--- planted faults, each of which must fail ---"
for planted in "busio_probe.py $tmp --fault deaf" \
               "sink_probe.py $tmp --fault flip" \
               "sink_probe.py $tmp --fault truncate"; do
    # shellcheck disable=SC2086
    if run $planted > /dev/null 2>&1; then
        echo "the planted fault PASSED, so the probe is not a gate: $planted" >&2
        exit 1
    fi
    echo "  refused as it should: $planted"
done

echo
echo "--- the probes ---"
run sink_probe.py "$tmp"
run busio_probe.py "$tmp"

echo
echo "--- CircuitPython's documented I2SOut example, unmodified ---"
out="$tmp/cp_example.raw"
# A windows interpreter run from WSL writes to a windows path, which this
# shell cannot stat. Translate it back before asking how big the file is --
# otherwise a probe that played perfectly reads as "played nothing".
seen="$out"
if [[ "$out" == *\\* ]] && command -v wslpath >/dev/null 2>&1; then
    seen=$(wslpath -u "$out")
fi
rm -f "$seen"
run cp_example_probe.py "$out"
[[ -s "$seen" ]] || { echo "the example played nothing" >&2; exit 1; }
echo "$(stat -c%s "$seen") bytes out of the docstring example"
