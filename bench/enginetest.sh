#!/usr/bin/env bash
# Runs go-mysql-server's engine tests against cell-tnx and records, per
# suite, how many cases pass and which fail.
# Usage: bench/enginetest.sh [out.txt]
set -uo pipefail
out=${1:-enginetest.txt}
cd "$(dirname "$0")/.."
json=$(mktemp)
CELLTNX_ENGINETEST=1 go test -count=1 -timeout 60m -run '^TestEngine$' -json ./sqlgms/ >"$json" 2>/dev/null
python3 - "$json" "$out" <<'PY'
import json, sys, collections
results = {}
for line in open(sys.argv[1]):
    try:
        e = json.loads(line)
    except ValueError:
        continue
    if e.get("Action") in ("pass", "fail", "skip") and e.get("Test", "").startswith("TestEngine/"):
        results[e["Test"]] = e["Action"]
# Leaf cases only: drop any test that has subtests.
names = set(results)
leaves = {t: a for t, a in results.items() if not any(n.startswith(t + "/") for n in names)}
by_suite = collections.defaultdict(collections.Counter)
failing = collections.defaultdict(list)
for t, a in leaves.items():
    suite = t.split("/")[1]
    by_suite[suite][a] += 1
    if a == "fail":
        failing[suite].append(t)
with open(sys.argv[2], "w") as f:
    total = collections.Counter()
    f.write("suite                 pass  fail  skip\n")
    for s in sorted(by_suite):
        c = by_suite[s]
        total.update(c)
        f.write(f"{s:20s} {c['pass']:5d} {c['fail']:5d} {c['skip']:5d}\n")
    f.write(f"{'TOTAL':20s} {total['pass']:5d} {total['fail']:5d} {total['skip']:5d}\n\nFailing cases:\n")
    for s in sorted(failing):
        for t in sorted(failing[s]):
            f.write(t + "\n")
print(open(sys.argv[2]).read().split("\nFailing")[0])
PY
