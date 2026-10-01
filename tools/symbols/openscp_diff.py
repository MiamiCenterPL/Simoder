"""@summary Converts OpenSCP export-prop JSON before/after files into a complete Simoder mod."""

import argparse
import hashlib
import json
import math
import re
import struct
from pathlib import Path


def read_dump(path):
    """@summary Validates the upstream {name,propertyCount,properties[{name,hash,type,value}]} contract."""
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("Export exceeds 16 MiB")
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    properties = data.get("properties")
    if not isinstance(data.get("name"), str) or not isinstance(properties, list) or type(data.get("propertyCount")) is not int or data.get("propertyCount") != len(properties):
        raise ValueError("Invalid OpenSCP export root")
    result = {}
    for entry in properties:
        if not isinstance(entry, dict) or not isinstance(entry.get("hash"), str) or not re.fullmatch(r"0x[0-9a-fA-F]{8}", entry["hash"]):
            raise ValueError("Invalid property hash")
        if not isinstance(entry.get("type"), str) or not isinstance(entry.get("value"), str):
            raise ValueError("OpenSCP type/value must be strings")
        if entry.get("name") is not None and not isinstance(entry["name"], str):
            raise ValueError("OpenSCP name must be string or null")
        identifier = int(entry["hash"], 16)
        if identifier in result:
            raise ValueError("Duplicate property hash")
        result[identifier] = entry
    return data["name"], result


def make_mod(before_path, after_path, mod_id, tgi_text):
    """@summary Exports supported changes only and reports unsupported edits without silently dropping them."""
    if not re.fullmatch(r"[a-z0-9]+(?:[._-][a-z0-9]+)*", mod_id) or len(mod_id) > 128:
        raise ValueError("Invalid mod ID")
    if not re.fullmatch(r"[0-9a-fA-F]{8}:[0-9a-fA-F]{8}:[0-9a-fA-F]{8}", tgi_text):
        raise ValueError("TGI must be TYPE:GROUP:INSTANCE with eight hex digits each")
    components = [int(part, 16) for part in tgi_text.split(":")]
    if components[0] != 0x00B1B104:
        raise ValueError("Only PROP resource type 00B1B104 is supported")
    resource_name, before = read_dump(before_path)
    after_name, after = read_dump(after_path)
    if resource_name != after_name:
        raise ValueError("Before/after exports describe different resource names")
    changed, unsupported = [], []
    for identifier in sorted(before.keys() | after.keys()):
        old, new = before.get(identifier), after.get(identifier)
        if old and new and old["type"] == new["type"] and old["value"] == new["value"]:
            continue
        if old is None or new is None or old["type"] != "Float" or new["type"] != "Float":
            unsupported.append(f"0x{identifier:08X}: added/deleted or unsupported type change")
            continue
        value = float(new["value"])
        old_value = float(old["value"])
        if not math.isfinite(value) or not math.isfinite(old_value):
            raise ValueError("Non-finite float export")
        value = struct.unpack("f", struct.pack("f", value))[0]
        if not math.isfinite(value):
            raise ValueError("Float operand exceeds runtime range")
        changed.append((identifier, new, value))
    if not changed:
        raise ValueError("No supported scalar float changes found; " + "; ".join(unsupported))
    source = "OpenSCP export-prop JSON; before=" + hashlib.sha256(before_path.read_bytes()).hexdigest() + "; after=" + hashlib.sha256(after_path.read_bytes()).hexdigest()
    manifest = f"id: {mod_id}\nname: {json.dumps(mod_id)}\nversion: 1.0.0\n"
    symbols = ["symbols:", "  resources[1]{name,type,group,instance,source}:",
               f'    resource,0x{components[0]:08X},0x{components[1]:08X},0x{components[2]:08X},{json.dumps(source)}',
               f"  properties[{len(changed)}]{{name,id,source,type,description,origin}}:"]
    overrides = ["patches[1]:", "  - target:", "      name: resource", f"    properties[{len(changed)}]{{name,type,operation,value}}:"]
    for identifier, entry, value in changed:
        alias = f"property_{identifier:08X}"
        description = entry["name"] or ""
        if len(description) > 4096:
            raise ValueError("Property description exceeds runtime limit")
        if any(ord(character) < 32 and character not in "\n\r\t" for character in description):
            raise ValueError("Unsupported control character in property description")
        # @summary OpenSCP display names may be registry comments, so stable aliases use the exact hash.
        symbols.append(f'    {alias},0x{identifier:08X},{json.dumps(source)},float,{json.dumps(description, ensure_ascii=False)},registry_display')
        overrides.append(f"      {alias},float,set,{value:.9g}")
    return {"mod.toon": manifest, "symbols.toon": "\n".join(symbols) + "\n", "overrides.toon": "\n".join(overrides) + "\n"}, unsupported


def main():
    """@summary Creates a new reviewable mod directory without installing or enabling it."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--id", required=True)
    parser.add_argument("--tgi", required=True, help="Exact TGI; exported display names alone do not identify resources")
    parser.add_argument("--allow-unsupported", action="store_true")
    args = parser.parse_args()
    try:
        files, unsupported = make_mod(args.before, args.after, args.id, args.tgi)
        if unsupported:
            print("Unsupported changes:\n" + "\n".join(unsupported))
            if not args.allow_unsupported:
                parser.error("Review unsupported edits before accepting a partial conversion")
        if any(len(text.encode("utf-8")) > 1024 * 1024 for text in files.values()):
            raise ValueError("Generated file exceeds runtime 1 MiB limit")
        args.output.mkdir()
        for name, text in files.items():
            (args.output / name).write_text(text, encoding="utf-8")
        print(f"Created {args.output}; unsupported={len(unsupported)}; validate against source packages before enabling")
    except (OSError, ValueError, OverflowError, TypeError, AttributeError) as error:
        parser.exit(1, f"Conversion failed: {error}\n")


if __name__ == "__main__":
    main()
