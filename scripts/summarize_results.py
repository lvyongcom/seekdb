#!/usr/bin/env python3
"""Summarize a protocol_check / local-driver result.json.

Usage:
    python3 scripts/summarize_results.py <result.json> [more.json ...]
"""

import json
import sys
from pathlib import Path


def summarize(path: Path) -> bool:
    data = json.loads(path.read_text(encoding="utf-8"))
    cases = data.get("cases", [])
    passed = [case for case in cases if case.get("status") == "PASS"]
    failed = [case for case in cases if case.get("status") != "PASS"]

    print(f"{path}")
    print(f"  passed={data.get('passed')}  total={len(cases)}  "
          f"pass={len(passed)}  fail={len(failed)}")
    for case in failed:
        message = (case.get("message") or "").splitlines()
        first = message[0][:160] if message else ""
        print(f"  - {case.get('id')}: {case.get('status')} {first}")
    return not failed


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    ok = True
    for arg in argv[1:]:
        ok = summarize(Path(arg)) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
