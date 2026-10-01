"""@summary Converts verified external descriptor formats into Simoder symbols.toon."""

import argparse
import hashlib
import json
import re
import sqlite3
from contextlib import closing
from pathlib import Path


def read_properties(path, format_name):
    """@summary Reads numeric OpenSC5 declarations or OpenSCP-compatible SQLite descriptors."""
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    source = f"{format_name}:{path.name}; sha256={digest}"
    rows, skipped = [], []
    if format_name == "opensc5":
        for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
            text = line.split("#", 1)[0].strip()
            if not text:
                continue
            match = re.fullmatch(r"property\s+(\S+)\s+(0[xX][0-9a-fA-F]{1,8}|[0-9]+)(?:\s+(\S+))?", text)
            if not match:
                skipped.append(f"line {line_number}: {text}")
                continue
            identifier = int(match[2], 16 if match[2].lower().startswith("0x") else 10)
            if identifier > 0xFFFFFFFF:
                raise ValueError(f"Property ID exceeds uint32 at line {line_number}")
            rows.append((match[1], identifier, source + f"; line={line_number}", (match[3] or "").lower(), ""))
    else:
        with closing(sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)) as connection:
            for identifier, name, comments in connection.execute("SELECT id, name, comments FROM Properties"):
                if not isinstance(identifier, int) or not -(2**31) <= identifier <= 2**32 - 1:
                    raise ValueError(f"Invalid registry property ID: {identifier!r}")
                if not isinstance(name, str) or not name.strip():
                    skipped.append(f"id {identifier}: empty name")
                    continue
                if comments is not None and not isinstance(comments, str):
                    raise ValueError(f"Invalid registry description for {name!r}")
                rows.append((name, identifier & 0xFFFFFFFF, source, "", comments or ""))
    return rows, skipped


def convert(path, format_name, user_path=None):
    """@summary Produces deterministic aliases and rejects ambiguous external names."""
    rows, skipped = read_properties(path, format_name)
    if user_path:
        if format_name != "s3db":
            raise ValueError("--user is supported only for s3db")
        user_rows, user_skipped = read_properties(user_path, format_name)
        by_id = {row[1]: row for row in rows}
        by_id.update({row[1]: row for row in user_rows})
        rows = list(by_id.values())
        skipped.extend(user_skipped)
    aliases = {}
    for name, identifier, source, type_name, description in rows:
        if len(name) > 128 or any(ord(character) < 32 for character in name):
            raise ValueError(f"Unsupported alias: {name!r}")
        if name in aliases and aliases[name][0] != identifier:
            raise ValueError(f"Ambiguous alias {name!r}: multiple property IDs")
        if len(description) > 4096:
            raise ValueError(f"Description exceeds runtime limit for {name!r}")
        if any(ord(character) < 32 and character not in "\n\r\t" for character in description):
            raise ValueError(f"Unsupported control character in description for {name!r}")
        aliases.setdefault(name, (identifier, source, type_name, description))
    if not aliases:
        raise ValueError("No numeric property aliases found")
    output = ["symbols:", f"  properties[{len(aliases)}]{{name,id,source,type,description,origin}}:"]
    for name, (identifier, source, type_name, description) in sorted(aliases.items()):
        output.append(f"    {json.dumps(name, ensure_ascii=False)},0x{identifier:08X},{json.dumps(source, ensure_ascii=False)},"
                      f"{json.dumps(type_name, ensure_ascii=False)},{json.dumps(description, ensure_ascii=False)},{json.dumps('registry' if format_name == 's3db' else 'reverse_engineering')}")
    text = "\n".join(output) + "\n"
    if len(text.encode("utf-8")) > 1024 * 1024:
        raise ValueError("Catalog exceeds the runtime 1 MiB TOON limit")
    return text, skipped


def main():
    """@summary Converts a local dictionary without downloading files or modifying game packages."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("format", choices=["opensc5", "s3db"])
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--user", type=Path, help="Overlay database_user.s3db by property ID")
    parser.add_argument("--allow-skipped", action="store_true", help="Explicitly accept reported nonnumeric/empty entries")
    args = parser.parse_args()
    try:
        text, skipped = convert(args.input, args.format, args.user)
        if skipped:
            print("Skipped entries:\n" + "\n".join(skipped))
            if not args.allow_skipped:
                parser.error("Import contains skipped entries; inspect them and use --allow-skipped to accept")
        if args.output.exists():
            parser.error("Output exists; choose a new path to preserve the existing catalog")
        with args.output.open("x", encoding="utf-8", newline="\n") as output:
            output.write(text)
        print(f"Written {args.output}; skipped={len(skipped)}")
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, f"Import failed: {error}\n")


if __name__ == "__main__":
    main()
