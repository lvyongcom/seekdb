#!/usr/bin/env bash
# Profile the query path with gprof (perf and valgrind are unavailable in WSL,
# and the WSL kernel has perf_event_paranoid=2 with no matching linux-tools).
#
# Two runs on purpose: the load phase is dominated by row encoding, and at -O0
# that dwarfs everything else (100M std::string internals). Pre-load the data
# with an unprofiled run, then profile a query-only workload so the numbers are
# about the query path.
#
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_profile.sh [queries]
set -uo pipefail

QUERIES=${1:-200}
cd "$HOME/work/seekdb-round2/minivectordb" || exit 1

echo "=== rebuild with -pg ==="
make clean > /dev/null 2>&1
# -O0 on purpose: at -O2 the inlining makes gprof attribute time to the wrong
# symbol and inflate call counts (it reported 1.23M Coerce calls for 30k real
# ones). -O0 gives honest attribution at the cost of absolute speed.
make CXXFLAGS="-std=c++17 -O0 -g -pg -Wall -Wextra -Isrc" LDFLAGS="-pg" \
  > /tmp/mvdb-pg-build.log 2>&1
if [ $? -ne 0 ]; then
  tail -20 /tmp/mvdb-pg-build.log
  exit 1
fi
echo "ok"

echo
echo "=== generating workloads ==="
python3 - "$QUERIES" <<'PY' > /tmp/mvdb-gen.log
import json
import random
import sys

queries = int(sys.argv[1])
rng = random.Random(11)
DIM = 128
ROWS = 10000

def vec():
    return "[" + ", ".join(f"{rng.uniform(-1, 1):.3f}" for _ in range(DIM)) + "]"

def dump(path, statements):
    with open(path, "w", encoding="utf-8") as handle:
        for index, sql in enumerate(statements, 1):
            handle.write(json.dumps({"id": index, "op": "execute", "sql": sql,
                                     "search_mode": "exact"}) + "\n")
        handle.write(json.dumps({"id": len(statements) + 1, "op": "shutdown"}) + "\n")

load = [f"CREATE TABLE docs (id BIGINT PRIMARY KEY, g BIGINT, embedding VECTOR({DIM}));"]
batch = 500
for start in range(1, ROWS + 1, batch):
    values = []
    for i in range(start, min(start + batch, ROWS + 1)):
        values.append(f"({i}, {i % 4}, '{vec()}')")
    load.append("INSERT INTO docs VALUES " + ", ".join(values) + ";")

query = [f"SELECT id, L2_DISTANCE(embedding, '{vec()}') AS distance "
         f"FROM docs WHERE g = 2 ORDER BY distance LIMIT 10;" for _ in range(queries)]

dump("/tmp/mvdb-load.jsonl", load)
dump("/tmp/mvdb-query.jsonl", query)
print(f"load statements={len(load)} query statements={len(query)}")
PY
cat /tmp/mvdb-gen.log

echo
echo "=== pre-load (not profiled) ==="
rm -rf /tmp/mvdb-profdb
/usr/bin/time -f "  load elapsed %e s  maxrss %M KB" \
  ./build/minivectordb --data-dir /tmp/mvdb-profdb \
  < /tmp/mvdb-load.jsonl > /dev/null 2> /tmp/mvdb-load-time.txt
tail -2 /tmp/mvdb-load-time.txt

echo
echo "=== query-only run (profiled) ==="
rm -f gmon.out
/usr/bin/time -f "  query elapsed %e s  maxrss %M KB" \
  ./build/minivectordb --data-dir /tmp/mvdb-profdb \
  < /tmp/mvdb-query.jsonl > /dev/null 2> /tmp/mvdb-prof-time.txt
tail -2 /tmp/mvdb-prof-time.txt

echo
echo "=== flat profile (top 18) ==="
gprof -b -p ./build/minivectordb gmon.out 2>/dev/null | sed -n '5,24p' | cut -c1-140
