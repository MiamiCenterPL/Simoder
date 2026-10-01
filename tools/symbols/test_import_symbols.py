"""@summary Tests dictionary conversion with synthetic, redistributable fixtures."""

import sqlite3
import tempfile
import unittest
import os
import subprocess
from contextlib import closing
from pathlib import Path

from import_symbols import convert
from openscp_diff import make_mod


class ImportTests(unittest.TestCase):
    """@summary Checks explicit IDs, provenance, ambiguity and the actual s3db table contract."""

    def test_opensc5(self):
        """@summary Verifies deterministic output and reporting of unresolved hash expressions."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Properties.txt"
            path.write_text("# fixture\nproperty cost 0x09AE19D7 float\nproperty unknown (hash(unknown)) float\n", encoding="utf-8")
            text, skipped = convert(path, "opensc5")
            self.assertIn('"cost",0x09AE19D7,', text)
            self.assertIn("sha256=", text)
            self.assertEqual(len(skipped), 1)
            self.assertEqual(convert(path, "opensc5")[0], text)
            path.write_text("property cost 0x01 float\nproperty cost 0x02 float\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Ambiguous"):
                convert(path, "opensc5")

    def test_s3db(self):
        """@summary Verifies read-only SQLite import, signed IDs, quoted names and empty descriptors."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "registry.s3db"
            with closing(sqlite3.connect(path)) as connection:
                connection.execute("CREATE TABLE Properties(id INTEGER, name TEXT, comments TEXT)")
                connection.executemany("INSERT INTO Properties VALUES (?, ?, ?)", [
                    (-1, 'Cost, "hourly"', "fixture"), (12, None, None)])
                connection.commit()
            original = path.read_bytes()
            text, skipped = convert(path, "s3db")
            self.assertIn("0xFFFFFFFF", text)
            self.assertIn(r'\"hourly\"', text)
            self.assertEqual(len(skipped), 1)
            self.assertEqual(path.read_bytes(), original)

    @unittest.skipUnless(os.environ.get("SC13_MOD_INSPECTOR"), "Inspector supplied by CTest")
    def test_runtime_parser(self):
        """@summary Passes a converted catalog through the production C++ mod parser."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dictionary = root / "Properties.txt"
            dictionary.write_text("property cost 0x09AE19D7 float\n", encoding="utf-8")
            (root / "symbols.toon").write_text(convert(dictionary, "opensc5")[0], encoding="utf-8")
            (root / "mod.toon").write_text("id: fixture.mod\nname: Fixture\nversion: 1.0.0\n", encoding="utf-8")
            (root / "overrides.toon").write_text(
                "patches[1]:\n  - target:\n      type: 0x00B1B104\n"
                "      group: 0x61EFC000\n      instance: 0x719436BD\n"
                "    properties[1]{name,type,operation,value}:\n      cost,float,set,345\n", encoding="utf-8")
            result = subprocess.run([os.environ["SC13_MOD_INSPECTOR"], "mod", str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("cost id=0x9ae19d7 set 345", result.stdout)
            self.assertIn("sha256=", result.stdout)
            dictionary.write_text("property cost 0x09AE19D7 uint32\n", encoding="utf-8")
            (root / "symbols.toon").write_text(convert(dictionary, "opensc5")[0], encoding="utf-8")
            result = subprocess.run([os.environ["SC13_MOD_INSPECTOR"], "mod", str(root)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("incompatible", result.stderr)

    def test_openscp_diff(self):
        """@summary Tests the upstream display-string JSON contract and unsupported edit reporting."""
        import json
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = {"name": "sanitizer", "propertyCount": 2, "properties": [
                {"name": "Operating cost", "hash": "0x09ae19d7", "type": "Float", "value": "300"},
                {"name": None, "hash": "0x00000002", "type": "Bool", "value": "False"}]}
            new = json.loads(json.dumps(old))
            new["properties"][0]["value"] = "345"
            new["properties"][1]["value"] = "True"
            before, after = root / "before.json", root / "after.json"
            before.write_text(json.dumps(old), encoding="utf-8")
            after.write_text(json.dumps(new), encoding="utf-8")
            files, unsupported = make_mod(before, after, "fixture.openscp", "00B1B104:61EFC000:719436BD")
            self.assertEqual(len(unsupported), 1)
            self.assertIn("property_09AE19D7,float,set,345", files["overrides.toon"])
            self.assertIn("Operating cost", files["symbols.toon"])
            if os.environ.get("SC13_MOD_INSPECTOR"):
                output = root / "mod"
                output.mkdir()
                for name, text in files.items():
                    (output / name).write_text(text, encoding="utf-8")
                result = subprocess.run([os.environ["SC13_MOD_INSPECTOR"], "mod", str(output)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            new["properties"][0]["value"] = "NaN"
            after.write_text(json.dumps(new), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Non-finite"):
                make_mod(before, after, "fixture.openscp", "00B1B104:61EFC000:719436BD")

    def test_registry_user_overlay(self):
        """@summary Verifies user overrides by ID and preserves selected provenance and comments."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main, user = root / "main.s3db", root / "user.s3db"
            for path, name in [(main, "Original"), (user, "UserAlias")]:
                with closing(sqlite3.connect(path)) as connection:
                    connection.execute("CREATE TABLE Properties(id INTEGER, name TEXT, comments TEXT)")
                    connection.execute("INSERT INTO Properties VALUES (1, ?, 'cost description')", (name,))
                    connection.commit()
            text, skipped = convert(main, "s3db", user)
            self.assertIn("UserAlias", text)
            self.assertNotIn("Original", text)
            self.assertIn("cost description", text)
            self.assertIn("user.s3db", text)
            self.assertEqual(skipped, [])


if __name__ == "__main__":
    unittest.main()
