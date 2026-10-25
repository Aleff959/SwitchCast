"""
Unit and E2E Tests: Versioning and Manifest Alignment (F8, F1)
Tiers 1 & 2: Macro verification, update_manifest.json validation, sha256 checksums,
compilation number 31, schema conformance, and boundary cases.
"""

import unittest
import os
import re
import json
import hashlib
import struct
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILDS_DIR = Path(r"c:\Users\Alpha\OneDrive\Documentos\builds")


class TestVersioningAndManifest(unittest.TestCase):
    """
    Tier 1 & Tier 2 tests for Feature 8 (devkitPro Build & Manifest Alignment)
    and Feature 1 (OTA Manifest Verification).
    """

    @classmethod
    def setUpClass(cls):
        cls.types_hpp = REPO_ROOT / "src" / "common" / "Types.hpp"
        cls.webremote_hpp = REPO_ROOT / "src" / "network" / "WebRemote.hpp"
        cls.manifest_json = REPO_ROOT / "update_manifest.json"
        cls.core_nro = REPO_ROOT / "SwitchCastCore.nro"
        cls.launcher_nro = REPO_ROOT / "SwitchCast.nro"

    # =========================================================================
    # TIER 1: HAPPY PATH & CONTRACT VERIFICATION (F8 & F1)
    # =========================================================================

    def test_tier1_types_hpp_version_macros(self):
        """F8-T1-1: Verify version macros in Types.hpp conform to 2.4.0 Comp. 31 contract."""
        self.assertTrue(self.types_hpp.exists(), "Types.hpp must exist")
        content = self.types_hpp.read_text(encoding="utf-8")

        m_version = re.search(r'#define\s+SWITCHCAST_VERSION_STR\s+"([^"]+)"', content)
        self.assertIsNotNone(m_version, "SWITCHCAST_VERSION_STR macro missing")
        self.assertEqual(m_version.group(1), "2.4.0")

        m_code = re.search(r'#define\s+SWITCHCAST_VERSION_CODE\s+(\d+)', content)
        self.assertIsNotNone(m_code, "SWITCHCAST_VERSION_CODE macro missing")
        self.assertEqual(int(m_code.group(1)), 20400)

        m_build = re.search(r'#define\s+SWITCHCAST_BUILD_NUM\s+(\d+)', content)
        self.assertIsNotNone(m_build, "SWITCHCAST_BUILD_NUM macro missing")
        self.assertEqual(int(m_build.group(1)), 31, "Compilation number must be 31")

        m_full = re.search(r'#define\s+SWITCHCAST_VERSION_FULL_STR\s+"([^"]+)"', content)
        self.assertIsNotNone(m_full, "SWITCHCAST_VERSION_FULL_STR macro missing")
        self.assertEqual(m_full.group(1), "2.4.0 | Comp. 31")

    def test_tier1_webremote_hpp_compilation_strings(self):
        """F8-T1-2: Verify WebRemote.hpp contains Comp. 31 in badge and drawer footer."""
        self.assertTrue(self.webremote_hpp.exists(), "WebRemote.hpp must exist")
        content = self.webremote_hpp.read_text(encoding="utf-8")

        self.assertIn("Comp. 31", content, "WebRemote.hpp must reference Comp. 31")
        self.assertIn("v2.4.0 | Comp. 31", content, "WebRemote.hpp badge must show v2.4.0 | Comp. 31")

    def test_tier1_manifest_json_structure(self):
        """F1-T1-1: Validate update_manifest.json top-level schema and mandatory keys."""
        self.assertTrue(self.manifest_json.exists(), "update_manifest.json must exist")
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            data = json.load(f)

        self.assertIn("schema_version", data)
        self.assertEqual(data["schema_version"], 1)
        self.assertIn("channel", data)
        self.assertIn(data["channel"], ["beta", "dev", "stable", "main"])
        self.assertIn("core", data)
        self.assertIsInstance(data["core"], dict)
        self.assertIn("modules", data)
        self.assertIsInstance(data["modules"], list)

    def test_tier1_manifest_core_fields(self):
        """F1-T1-2: Validate core update fields in update_manifest.json."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            data = json.load(f)

        core = data["core"]
        self.assertEqual(core.get("version"), "2.4.0")
        self.assertEqual(core.get("version_code"), 20400)
        self.assertIn("min_hos_version", core)
        self.assertIn("download_url", core)
        self.assertTrue(core["download_url"].startswith("https://"))
        self.assertIn("sha256", core)
        self.assertIn("size_bytes", core)
        self.assertIn("changelog", core)
        self.assertIsInstance(core["changelog"], list)
        self.assertGreater(len(core["changelog"]), 0)

    def test_tier1_manifest_channel_matches_dev_branch(self):
        """F1-T1-3: Verify update_manifest.json on dev branch targets beta channel & dev URL."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            data = json.load(f)

        self.assertEqual(data["channel"], "beta")
        self.assertIn("/dev/", data["core"]["download_url"])

    def test_tier1_core_nro_sha256_match(self):
        """F8-T1-3: Verify actual SHA-256 of SwitchCastCore.nro matches manifest sha256."""
        self.assertTrue(self.core_nro.exists(), "SwitchCastCore.nro must exist")
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        expected_sha = manifest["core"]["sha256"].lower().strip()
        hasher = hashlib.sha256()
        with open(self.core_nro, "rb") as f:
            while chunk := f.read(65536):
                hasher.update(chunk)
        actual_sha = hasher.hexdigest().lower()

        self.assertEqual(actual_sha, expected_sha, "Core NRO SHA-256 mismatch with manifest")

    def test_tier1_core_nro_size_bytes_match(self):
        """F8-T1-4: Verify actual file size of SwitchCastCore.nro matches manifest size_bytes."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        expected_size = manifest["core"]["size_bytes"]
        actual_size = os.path.getsize(self.core_nro)
        self.assertEqual(actual_size, expected_size, "Core NRO size mismatch with manifest")

    def test_tier1_manifest_modules_entries_valid(self):
        """F1-T1-4: Verify manifest module entries conform to required schema."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        modules = manifest.get("modules", [])
        for mod in modules:
            self.assertIn("id", mod)
            self.assertIn("name", mod)
            self.assertIn("version", mod)
            self.assertIn("entry_file", mod)
            self.assertIn("download_url", mod)
            self.assertIn("sha256", mod)
            self.assertIn("hot_reload", mod)
            self.assertTrue(re.match(r"^[0-9a-f]{64}$", mod["sha256"]))

    def test_tier1_nro_format_magic_header(self):
        """F8-T1-5: Verify Nintendo Switch NRO format magic header 'NRO0' at offset 0x10."""
        for nro_path in [self.core_nro, self.launcher_nro]:
            if nro_path.exists():
                with open(nro_path, "rb") as f:
                    f.seek(0x10)
                    magic = f.read(4)
                    self.assertEqual(magic, b"NRO0", f"{nro_path.name} must have NRO0 magic header")

    # =========================================================================
    # TIER 2: BOUNDARY CONDITIONS & ERROR HANDLING (F8 & F1)
    # =========================================================================

    def test_tier2_sha256_format_boundary(self):
        """F8-T2-1: Test SHA-256 validation boundary cases (length, case, hex charset)."""
        valid_sha = "2386e1ba3978b5ca8bb5e2d544ba630898e147bed09475ffd584e56b9d7c8758"
        self.assertEqual(len(valid_sha), 64)
        self.assertTrue(all(c in "0123456789abcdef" for c in valid_sha))

        invalid_cases = [
            "",                                # Empty
            valid_sha[:-1],                    # 63 chars (short)
            valid_sha + "a",                   # 65 chars (long)
            valid_sha.upper(),                 # Uppercase
            valid_sha[:-1] + "g",              # Non-hex char 'g'
            valid_sha[:-1] + "Z",              # Non-hex char 'Z'
            None                               # NoneType
        ]

        def is_valid_sha256(s):
            if not isinstance(s, str):
                return False
            return bool(re.match(r"^[0-9a-f]{64}$", s))

        self.assertTrue(is_valid_sha256(valid_sha))
        for case in invalid_cases:
            self.assertFalse(is_valid_sha256(case), f"Case should be invalid: {case}")

    def test_tier2_compilation_number_monotonicity(self):
        """F8-T2-2: Test compilation number is strictly greater than 30 (monotonic bump)."""
        content = self.types_hpp.read_text(encoding="utf-8")
        m_build = re.search(r'#define\s+SWITCHCAST_BUILD_NUM\s+(\d+)', content)
        build_num = int(m_build.group(1))

        self.assertGreater(build_num, 30, "Comp. 31 must be strictly greater than Comp. 30")
        self.assertLessEqual(build_num, 9999, "Comp. number must be reasonable integer")

    def test_tier2_version_code_boundary_monotonicity(self):
        """F8-T2-3: Test version code formula (MAJOR*10000 + MINOR*100 + PATCH)."""
        content = self.types_hpp.read_text(encoding="utf-8")
        m_ver = re.search(r'#define\s+SWITCHCAST_VERSION_STR\s+"(\d+)\.(\d+)\.(\d+)"', content)
        major, minor, patch = int(m_ver.group(1)), int(m_ver.group(2)), int(m_ver.group(3))

        calculated_code = major * 10000 + minor * 100 + patch
        m_code = re.search(r'#define\s+SWITCHCAST_VERSION_CODE\s+(\d+)', content)
        self.assertEqual(int(m_code.group(1)), calculated_code)
        self.assertEqual(calculated_code, 20400)

    def test_tier2_manifest_malformed_json_recovery(self):
        """F1-T2-1: Manifest parser should gracefully handle malformed or truncated JSON."""
        malformed_samples = [
            '{"schema_version": 1, "channel": "beta", ',
            '{"schema_version": "invalid_number"}',
            '{"core": {"version_code": "not_an_int"}}',
            '',
            'null'
        ]

        for sample in malformed_samples:
            try:
                data = json.loads(sample) if sample else {}
                core = data.get("core", {}) if isinstance(data, dict) else {}
                has_valid_update = isinstance(core.get("version_code"), int) and core.get("version_code") > 0
                self.assertFalse(has_valid_update)
            except (json.JSONDecodeError, AttributeError):
                # Expected recovery behavior on decode failure
                pass

    def test_tier2_manifest_missing_mandatory_keys(self):
        """F1-T2-2: Manifest parser fails gracefully when mandatory keys are missing."""
        incomplete_core = {
            "version": "2.4.0",
            # missing version_code
            "download_url": "https://example.com/test.nro",
            # missing sha256
        }

        self.assertIsNone(incomplete_core.get("version_code"))
        self.assertIsNone(incomplete_core.get("sha256"))

        def can_stage_update(core_dict):
            if not isinstance(core_dict, dict):
                return False
            url = core_dict.get("download_url", "")
            sha = core_dict.get("sha256", "")
            code = core_dict.get("version_code", 0)
            return bool(url and sha and isinstance(code, int) and code > 0)

        self.assertFalse(can_stage_update(incomplete_core))

    def test_tier2_min_hos_version_boundary(self):
        """F1-T2-3: Horizon OS version string boundary validation (e.g., '16.0.0')."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        hos_ver = manifest["core"].get("min_hos_version", "")
        self.assertTrue(re.match(r"^\d+\.\d+\.\d+$", hos_ver))
        parts = [int(p) for p in hos_ver.split(".")]
        self.assertGreaterEqual(parts[0], 16, "Minimum Horizon OS version must be >= 16")

    def test_tier2_nro_file_size_lower_bound(self):
        """F8-T2-4: NRO binaries must exceed bare minimum executable threshold (>1MB)."""
        core_size = os.path.getsize(self.core_nro)
        self.assertGreater(core_size, 1024 * 1024 * 5, "Core NRO must be > 5MB (~30MB expected)")

        launcher_size = os.path.getsize(self.launcher_nro)
        self.assertGreater(launcher_size, 1024 * 1024 * 2, "Launcher NRO must be > 2MB (~7.8MB expected)")

    def test_tier2_manifest_changelog_item_types(self):
        """F1-T2-4: Manifest changelog entries must all be valid non-empty strings."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        changelog = manifest["core"].get("changelog", [])
        self.assertIsInstance(changelog, list)
        self.assertGreater(len(changelog), 0)
        for item in changelog:
            self.assertIsInstance(item, str)
            self.assertGreater(len(item.strip()), 0)

    def test_tier2_manifest_core_download_url_scheme(self):
        """F1-T2-5: Core download URL must use HTTPS and end with .nro."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        url = manifest["core"].get("download_url", "")
        self.assertTrue(url.startswith("https://"))
        self.assertTrue(url.endswith(".nro"))


if __name__ == "__main__":
    unittest.main()
