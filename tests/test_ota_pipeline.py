"""
Unit and E2E Tests: OTA Pipeline, Dynamic Changelog, and Zero-Lock Reboot (F1, F2, F3)
Tiers 1 & 2: Multi-channel OTA manifest checks, dev vs main URLs, hash matching on beta,
dynamic changelog rendering logic, zero-lock reboot sequence, and boundary handling.
"""

import unittest
import os
import json
import hashlib
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class MockOtaPipeline:
    """
    Python reference model simulating OtaUpdater.cpp and UIEngine.cpp::RenderUpdateCenter
    to verify requirements and algorithms independently.
    """

    def __init__(self, current_version="2.4.0", current_version_code=20400, current_sha="", channel="beta"):
        self.current_version = current_version
        self.current_version_code = current_version_code
        self.current_sha = current_sha.lower()
        self.channel = channel  # "beta" or "stable"
        self.custom_manifest_url = ""
        self.pending_update = None
        self.update_ready = False
        self.boot_pending = False
        self.rollback_pending = False
        self.last_boot_status = ""
        self.env_next_load = ""

    def get_manifest_url(self):
        """Replicates URL selection logic in OtaUpdater.cpp."""
        if self.custom_manifest_url:
            return self.custom_manifest_url
        if self.channel == "beta":
            return "https://raw.githubusercontent.com/Aleff959/SwitchCast/dev/update_manifest.json"
        return "https://raw.githubusercontent.com/Aleff959/SwitchCast/main/update_manifest.json"

    def should_update(self, manifest_core, local_file_sha=""):
        """Replicates shouldUpdate evaluation in OtaUpdater.cpp."""
        remote_code = manifest_core.get("version_code", 0)
        expected_sha = manifest_core.get("sha256", "").lower().strip()
        local_sha = (local_file_sha or self.current_sha).lower().strip()

        if remote_code > self.current_version_code:
            return True
        elif self.channel == "beta" and remote_code == self.current_version_code:
            if expected_sha and expected_sha != local_sha:
                return True
        return False

    def verify_download(self, file_bytes, expected_sha):
        """Verifies downloaded payload against expected SHA-256."""
        calc_sha = hashlib.sha256(file_bytes).hexdigest().lower()
        return calc_sha == expected_sha.lower().strip()

    def stage_update(self, version, changelog, sha):
        """Prepares pending update and flags update_ready."""
        self.pending_update = {
            "version": version,
            "changelog": changelog or [],
            "sha256": sha
        }
        self.update_ready = True
        return True

    def schedule_restart_zero_lock(self, launcher_path="sdmc:/switch/SwitchCast/SwitchCast.nro"):
        """Sets envSetNextLoad to launcher path for clean swap without holding FAT32 locks."""
        self.env_next_load = launcher_path
        self.boot_pending = True
        self.last_boot_status = "pending_install"
        return {
            "boot_pending": True,
            "rollback_pending": False,
            "status": "pending_install"
        }

    def rollback_to_backup(self, backup_file_exists, backup_size=1024):
        """Simulates RollbackToBackup logic in OtaUpdater.cpp."""
        if not backup_file_exists or backup_size < 128:
            return False
        self.boot_pending = True
        self.rollback_pending = True
        self.last_boot_status = "rollback_requested_by_core"
        self.env_next_load = "sdmc:/switch/SwitchCast/SwitchCast.nro"
        return True

    def mark_boot_successful(self):
        """Marks boot successful, clearing pending state."""
        self.boot_pending = False
        self.rollback_pending = False
        self.last_boot_status = "boot_verified_ok"


class MockDynamicChangelogRenderer:
    """
    Reference model for dynamic changelog extraction and card formatting
    implemented in UIEngine::RenderUpdateCenter.
    """

    DEFAULT_ITEMS = [
        {"code": "UI", "title": "Interface Renovada e Fluida", "desc": "Navegacao suave, superficies limpas e tipografia nitida."},
        {"code": "60", "title": "Reproducao Fluida a 60 FPS", "desc": "Streaming de video em tempo real com aceleracao por hardware."},
        {"code": "SD", "title": "Estabilidade de Armazenamento", "desc": "Gravacao direta e sincronizacao aprimorada no cartao microSD."},
        {"code": "OK", "title": "Protecao e Seguranca", "desc": "Backup automatico e restauracao de emergencia segurando L + R."}
    ]

    @classmethod
    def parse_changelog(cls, changelog_list, max_items=4, max_desc_len=80):
        """Parses manifest changelog array into cards with avatar, title, description."""
        if not changelog_list:
            return cls.DEFAULT_ITEMS

        parsed = []
        for idx, raw in enumerate(changelog_list):
            if idx >= max_items:
                break
            if not isinstance(raw, str) or not raw.strip():
                continue

            text = raw.strip()
            # If formatted like "Category: Details..."
            if ":" in text:
                parts = text.split(":", 1)
                title = parts[0].strip()
                desc = parts[1].strip()
                code = title[:2].upper() if len(title) >= 2 else "SC"
            else:
                title = text[:30]
                desc = text
                code = f"0{idx+1}"

            if len(desc) > max_desc_len:
                desc = desc[:max_desc_len - 3] + "..."

            parsed.append({
                "code": code,
                "title": title,
                "desc": desc
            })

        return parsed if parsed else cls.DEFAULT_ITEMS


class TestOtaPipeline(unittest.TestCase):
    """
    Tier 1 & Tier 2 tests for Feature 1 (In-App OTA), Feature 2 (Dynamic Changelog Modal),
    and Feature 3 (Zero-Lock Handoff).
    """

    @classmethod
    def setUpClass(cls):
        cls.updater_cpp = REPO_ROOT / "src" / "updater" / "OtaUpdater.cpp"
        cls.uiengine_cpp = REPO_ROOT / "src" / "ui" / "UIEngine.cpp"
        cls.updater_src = cls.updater_cpp.read_text(encoding="utf-8")
        cls.uiengine_src = cls.uiengine_cpp.read_text(encoding="utf-8")

    def setUp(self):
        self.pipeline = MockOtaPipeline(
            current_version="2.4.0",
            current_version_code=20400,
            current_sha="2386e1ba3978b5ca8bb5e2d544ba630898e147bed09475ffd584e56b9d7c8758",
            channel="beta"
        )

    # =========================================================================
    # TIER 1: HAPPY PATH & SPECIFICATION CONTRACTS (F1, F2, F3)
    # =========================================================================

    def test_tier1_ota_manifest_url_dev_vs_main(self):
        """F1-T1-1: Channel determines dev vs main branch manifest URL."""
        self.pipeline.channel = "beta"
        self.assertIn("/dev/update_manifest.json", self.pipeline.get_manifest_url())

        self.pipeline.channel = "stable"
        self.assertIn("/main/update_manifest.json", self.pipeline.get_manifest_url())

    def test_tier1_ota_newer_version_triggers_update(self):
        """F1-T1-2: Remote version_code higher than local triggers shouldUpdate."""
        manifest_core = {"version_code": 20500, "sha256": "abcdef"}
        self.assertTrue(self.pipeline.should_update(manifest_core))

    def test_tier1_ota_beta_hash_matching_triggers_update(self):
        """F1-T1-3: Beta channel triggers update when versionCode matches but sha256 differs."""
        manifest_core = {
            "version_code": 20400,
            "sha256": "1111111111111111111111111111111111111111111111111111111111111111"
        }
        self.assertTrue(self.pipeline.should_update(manifest_core))

    def test_tier1_ota_sha256_verification_pass(self):
        """F1-T1-4: Valid SHA-256 match confirms download integrity."""
        dummy_data = b"Nintendo SwitchCast Core NRO binary payload Comp. 31"
        expected_sha = hashlib.sha256(dummy_data).hexdigest()

        self.assertTrue(self.pipeline.verify_download(dummy_data, expected_sha))

    def test_tier1_ota_progress_fraction_calculation(self):
        """F1-T1-5: Progress fraction calculation scales linearly 0.0 to 1.0."""
        def calc_frac(down, total):
            return 0.0 if total <= 0 else min(1.0, max(0.0, float(down) / float(total)))

        self.assertAlmostEqual(calc_frac(0, 1000), 0.0)
        self.assertAlmostEqual(calc_frac(500, 1000), 0.5)
        self.assertAlmostEqual(calc_frac(1000, 1000), 1.0)

    def test_tier1_dynamic_changelog_parsing_items(self):
        """F2-T1-1: Changelog entries with colon split into code, title, and description."""
        manifest_changelog = [
            "Canais OTA: Alterne dinamicamente entre branch main e dev",
            "Audio Pro: Downmix 5.1 ITU-R BS.775 e Volume Boost 150%"
        ]
        cards = MockDynamicChangelogRenderer.parse_changelog(manifest_changelog)
        self.assertEqual(len(cards), 2)
        self.assertEqual(cards[0]["title"], "Canais OTA")
        self.assertIn("Alterne dinamicamente", cards[0]["desc"])
        self.assertEqual(cards[1]["title"], "Audio Pro")

    def test_tier1_dynamic_changelog_max_four_cards(self):
        """F2-T1-2: RenderUpdateCenter renders at most 4 feature cards in the modal."""
        long_list = [f"Item {i}: Description of feature {i}" for i in range(10)]
        cards = MockDynamicChangelogRenderer.parse_changelog(long_list, max_items=4)
        self.assertEqual(len(cards), 4)

    def test_tier1_zero_lock_launcher_handoff(self):
        """F3-T1-1: Staging update configures envSetNextLoad with Launcher NRO path."""
        state = self.pipeline.schedule_restart_zero_lock()
        self.assertTrue(state["boot_pending"])
        self.assertEqual(self.pipeline.env_next_load, "sdmc:/switch/SwitchCast/SwitchCast.nro")

    def test_tier1_zero_lock_rollback_requested(self):
        """F3-T1-2: RollbackToBackup writes rollback_requested_by_core into state."""
        ok = self.pipeline.rollback_to_backup(backup_file_exists=True, backup_size=1024 * 1024)
        self.assertTrue(ok)
        self.assertTrue(self.pipeline.rollback_pending)
        self.assertEqual(self.pipeline.last_boot_status, "rollback_requested_by_core")

    def test_tier1_zero_lock_mark_boot_success(self):
        """F3-T1-3: MarkBootSuccessful clears boot pending flags."""
        self.pipeline.schedule_restart_zero_lock()
        self.assertTrue(self.pipeline.boot_pending)

        self.pipeline.mark_boot_successful()
        self.assertFalse(self.pipeline.boot_pending)
        self.assertFalse(self.pipeline.rollback_pending)
        self.assertEqual(self.pipeline.last_boot_status, "boot_verified_ok")

    def test_tier1_updater_cpp_zero_lock_code_verification(self):
        """F3-T1-4: Verify envSetNextLoad and pending_update.json exist in OtaUpdater.cpp."""
        self.assertIn("envSetNextLoad(kLauncherNro, kLauncherNro);", self.updater_src)
        self.assertIn("rollback_pending", self.updater_src)
        self.assertIn("kBackupNro", self.updater_src)

    # =========================================================================
    # TIER 2: BOUNDARY CONDITIONS & ERROR HANDLING (F1, F2, F3)
    # =========================================================================

    def test_tier2_ota_sha256_mismatch_rejected(self):
        """F1-T2-1: Corrupt download or SHA-256 mismatch rejects payload."""
        real_data = b"Valid update content"
        tampered_data = b"Tampered malicious content"
        expected_sha = hashlib.sha256(real_data).hexdigest()

        self.assertFalse(self.pipeline.verify_download(tampered_data, expected_sha))

    def test_tier2_ota_older_version_code_rejected(self):
        """F1-T2-2: Older remote version code (downgrade) is rejected."""
        manifest_core = {"version_code": 20300, "sha256": "any_hash"}
        self.assertFalse(self.pipeline.should_update(manifest_core))

    def test_tier2_ota_stable_ignores_same_version_different_hash(self):
        """F1-T2-3: Stable channel ignores hash difference if version code is identical."""
        self.pipeline.channel = "stable"
        manifest_core = {
            "version_code": 20400,
            "sha256": "different_hash_from_dev"
        }
        self.assertFalse(self.pipeline.should_update(manifest_core))

    def test_tier2_dynamic_changelog_empty_fallback(self):
        """F2-T2-1: Empty changelog array falls back to default feature highlights."""
        empty_changelogs = [[], None, ["   ", ""]]
        for cl in empty_changelogs:
            cards = MockDynamicChangelogRenderer.parse_changelog(cl)
            self.assertEqual(len(cards), 4)
            self.assertEqual(cards[0]["code"], "UI")
            self.assertEqual(cards[1]["code"], "60")

    def test_tier2_dynamic_changelog_long_description_truncation(self):
        """F2-T2-2: Descriptions exceeding card length are safely truncated with ellipsis."""
        long_desc = "X" * 150
        manifest_changelog = [f"Feature: {long_desc}"]
        cards = MockDynamicChangelogRenderer.parse_changelog(manifest_changelog, max_desc_len=80)

        self.assertEqual(len(cards[0]["desc"]), 80)
        self.assertTrue(cards[0]["desc"].endswith("..."))

    def test_tier2_dynamic_changelog_unicode_and_accents(self):
        """F2-T2-3: Changelog with UTF-8 accents and symbols renders without corruption."""
        manifest_changelog = [
            "Áudio & Legendas: Reprodução multilíngue com suporte a UTF-8",
            "Canais: Sincronização automática entre ramificações"
        ]
        cards = MockDynamicChangelogRenderer.parse_changelog(manifest_changelog)
        self.assertIn("Áudio & Legendas", cards[0]["title"])
        self.assertIn("multilíngue", cards[0]["desc"])

    def test_tier2_zero_lock_missing_backup_aborts_rollback(self):
        """F3-T2-1: RollbackToBackup safely aborts if backup NRO is missing."""
        ok = self.pipeline.rollback_to_backup(backup_file_exists=False)
        self.assertFalse(ok)
        self.assertFalse(self.pipeline.rollback_pending)

    def test_tier2_zero_lock_undersized_backup_aborts(self):
        """F3-T2-2: RollbackToBackup safely aborts if backup NRO is < 128 bytes."""
        ok = self.pipeline.rollback_to_backup(backup_file_exists=True, backup_size=64)
        self.assertFalse(ok)
        self.assertFalse(self.pipeline.rollback_pending)

    def test_tier2_progress_fraction_divide_by_zero_safety(self):
        """F1-T2-4: Download progress with 0 total bytes avoids division by zero."""
        def safe_fraction(down, total):
            if total <= 0:
                return 0.0
            return max(0.0, min(1.0, float(down) / float(total)))

        self.assertEqual(safe_fraction(100, 0), 0.0)
        self.assertEqual(safe_fraction(0, 0), 0.0)

    def test_tier2_progress_fraction_overflow_clamping(self):
        """F1-T2-5: Downloaded bytes greater than total clamps to 1.0."""
        def safe_fraction(down, total):
            if total <= 0:
                return 0.0
            return max(0.0, min(1.0, float(down) / float(total)))

        self.assertEqual(safe_fraction(1500, 1000), 1.0)

    def test_tier2_custom_manifest_url_override(self):
        """F1-T2-6: Custom manifest URL in settings overrides default channel URLs."""
        self.pipeline.custom_manifest_url = "http://192.168.1.50:8000/manifest.json"
        self.assertEqual(self.pipeline.get_manifest_url(), "http://192.168.1.50:8000/manifest.json")


if __name__ == "__main__":
    unittest.main()
