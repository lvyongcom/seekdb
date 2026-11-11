#!/usr/bin/env python3
"""Validate agent/conversation.json against the published rules.

Checks what the review pipeline can check mechanically:

  * valid UTF-8 JSON, top level is an object, <= 128 KiB
  * no duplicate object keys, no NaN / Infinity
  * every cite resolves through an RFC6901 pointer to a *string* field
  * the quote is an exact substring of that field and <= 240 characters
  * student_evidence and result_evidence never cite the same field
  * a 0.5-tier instance has an empty result citation
  * no quote is reused across instances

Usage:
    python3 scripts/check_conversation.py agent/conversation.json
"""

import json
import sys
from pathlib import Path

MAX_BYTES = 128 * 1024
MAX_QUOTE = 240


class DuplicateKey(ValueError):
    pass


def load_strict(text):
    def reject_constant(value):
        raise ValueError(f"non-finite JSON number: {value}")

    def unique_pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise DuplicateKey(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    return json.loads(text, parse_constant=reject_constant, object_pairs_hook=unique_pairs)


def resolve(document, pointer):
    """RFC6901 resolution. Returns (found, value)."""
    if pointer == "":
        return True, document
    if not pointer.startswith("/"):
        return False, None
    node = document
    for raw in pointer[1:].split("/"):
        token = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(node, dict):
            if token not in node:
                return False, None
            node = node[token]
        elif isinstance(node, list):
            if not token.isdigit():
                return False, None
            index = int(token)
            if index >= len(node):
                return False, None
            node = node[index]
        else:
            return False, None
    return True, node


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip())
        return 2

    path = Path(argv[1])
    raw = path.read_bytes()
    problems = []

    if len(raw) > MAX_BYTES:
        problems.append(f"file is {len(raw)} bytes, limit is {MAX_BYTES}")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        problems.append(f"not valid UTF-8: {exc}")
        text = None

    document = None
    if text is not None:
        try:
            document = load_strict(text)
        except DuplicateKey as exc:
            problems.append(str(exc))
        except ValueError as exc:
            problems.append(f"invalid JSON: {exc}")

    if document is not None and not isinstance(document, dict):
        problems.append("top level must be a JSON object")
        document = None

    if isinstance(document, dict) and not document.get("synthetic", False):
        pass  # real material; nothing to check

    instances = []
    if isinstance(document, dict):
        instances = document.get("evidence", [])
        if not isinstance(instances, list):
            problems.append("evidence must be an array")
            instances = []

    seen_quotes = {}
    total_tier = 0.0
    for position, instance in enumerate(instances):
        label = instance.get("id", f"#{position}")
        if not isinstance(instance, dict):
            problems.append(f"{label}: instance is not an object")
            continue

        tier = instance.get("claimed_tier")
        dimension = instance.get("dimension")
        if not dimension:
            problems.append(f"{label}: missing dimension")
        if tier not in (0.5, 1, 1.5, 2):
            problems.append(f"{label}: tier {tier!r} is not one of 0.5/1/1.5/2")
        else:
            total_tier += tier

        cites = {}
        for key in ("student_evidence", "result_evidence"):
            cite = instance.get(key)
            if not isinstance(cite, dict):
                problems.append(f"{label}: {key} must be an object")
                continue
            pointer = cite.get("pointer", "")
            quote = cite.get("quote", "")
            if not isinstance(pointer, str) or not isinstance(quote, str):
                problems.append(f"{label}: {key} pointer/quote must be strings")
                continue

            if pointer == "":
                if key == "student_evidence":
                    problems.append(f"{label}: student_evidence needs a pointer")
                if tier != 0.5:
                    problems.append(f"{label}: only a 0.5 instance may omit {key}")
                continue

            if len(quote) > MAX_QUOTE:
                problems.append(f"{label}: {key} quote is {len(quote)} chars, limit {MAX_QUOTE}")
            if quote == "":
                problems.append(f"{label}: {key} has a pointer but an empty quote")
                continue

            found, value = resolve(document, pointer)
            if not found:
                problems.append(f"{label}: {key} pointer {pointer} does not resolve")
                continue
            if not isinstance(value, str):
                problems.append(f"{label}: {key} pointer {pointer} is not a string field")
                continue
            if quote not in value:
                problems.append(f"{label}: {key} quote is not an exact substring of {pointer}")
            cites[key] = pointer

            if quote in seen_quotes:
                problems.append(
                    f"{label}: quote reused from {seen_quotes[quote]} - "
                    f"the same text must not score twice")
            else:
                seen_quotes[quote] = label

        if cites.get("student_evidence") and cites.get("student_evidence") == cites.get("result_evidence"):
            problems.append(f"{label}: student and result evidence cite the same field")

    print(f"file        : {path}")
    print(f"size        : {len(raw)} bytes (limit {MAX_BYTES})")
    print(f"instances   : {len(instances)}")
    print(f"claimed sum : {total_tier}")
    print()

    if problems:
        print("PROBLEMS:")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    print("OK: structure, pointers and quotes all check out")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
