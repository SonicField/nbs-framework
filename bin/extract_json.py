#!/usr/bin/env python3
"""Extract the first valid JSON object from a file that may contain non-JSON text.

Usage: extract_json.py <file>

Reads the file, finds the first '{' ... '}' block that parses as valid JSON,
and prints it to stdout. Exits 0 on success, 1 if no valid JSON found.
"""

import json
import sys


def extract_json(text: str) -> str | None:
    """Find and return the first valid JSON object in text."""
    depth = 0
    start = None
    in_string = False
    escape = False

    for i, ch in enumerate(text):
        if escape:
            escape = False
            continue
        if ch == '\\' and in_string:
            escape = True
            continue
        if ch == '"' and not escape:
            in_string = not in_string
            continue
        if in_string:
            continue

        if ch == '{':
            if depth == 0:
                start = i
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0 and start is not None:
                candidate = text[start:i + 1]
                try:
                    parsed = json.loads(candidate)
                    if isinstance(parsed, dict):
                        return json.dumps(parsed)
                except json.JSONDecodeError:
                    start = None
                    continue

    return None


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file>", file=sys.stderr)
        return 1

    try:
        with open(sys.argv[1]) as f:
            text = f.read()
    except (OSError, IOError) as e:
        print(f"Error reading file: {e}", file=sys.stderr)
        return 1

    result = extract_json(text)
    if result is None:
        print("No valid JSON object found", file=sys.stderr)
        return 1

    print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
