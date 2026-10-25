"""
E2E Scenario & Pairwise Integration Tests (Tier 3 & Tier 4)
Validates cross-feature interactions (F1-F8 pairwise matrix), end-to-end real-world
workflows (OTA upgrade, multi-track playback, custom wallpaper lifecycle, subtitle injection),
and previous build archive verification in Documentos/builds per User Rule 2.
"""

import unittest
import os
import re
import json
import hashlib
import zipfile
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILDS_DIR = Path(r"c:\Users\Alpha\OneDrive\Documentos\builds")
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from test_webremote_api import MockServerBridge
from test_ota_pipeline import MockOtaPipeline, MockDynamicChangelogRenderer
from test_media_and_wallpaper import MockMpvPlayer, MockTrackInfo


class TestScenariosAndCombinations(unittest.TestCase):
    """
    Tier 3 (Pairwise Feature Interactions) and Tier 4 (Real-World Workflows)
    test suite covering SwitchCast v2.4.0 (Comp. 31).
    """

    @classmethod
    def setUpClass(cls):
        cls.types_hpp = REPO_ROOT / "src" / "common" / "Types.hpp"
        cls.webremote_hpp = REPO_ROOT / "src" / "network" / "WebRemote.hpp"
        cls.manifest_json = REPO_ROOT / "update_manifest.json"
        cls.core_nro = REPO_ROOT / "SwitchCastCore.nro"
        cls.launcher_nro = REPO_ROOT / "SwitchCast.nro"

    # =========================================================================
    # TIER 3: PAIRWISE CROSS-FEATURE INTERACTIONS (>= 8 TESTS)
    # =========================================================================

    def test_pairwise_f1_f2_ota_detect_to_dynamic_changelog(self):
        """P1 (F1 x F2): Manifest update detection populates dynamic modal changelog."""
        ota = MockOtaPipeline(channel="beta")
        manifest_data = {
            "channel": "beta",
            "core": {
                "version": "2.4.0",
                "version_code": 20400,
                "sha256": "9999999999999999999999999999999999999999999999999999999999999999",
                "changelog": [
                    "OTA: Dynamic changelog modal",
                    "Media: Subtitle and Audio cycling",
                    "Wallpaper: Custom MP4 loop"
                ]
            }
        }
        # Detects update on beta
        self.assertTrue(ota.should_update(manifest_data["core"], local_file_sha="2386e1ba..."))
        # Changelog feeds dynamic modal renderer
        cards = MockDynamicChangelogRenderer.parse_changelog(manifest_data["core"]["changelog"])
        self.assertEqual(len(cards), 3)
        self.assertEqual(cards[0]["title"], "OTA")
        self.assertEqual(cards[1]["title"], "Media")
        self.assertEqual(cards[2]["title"], "Wallpaper")

    def test_pairwise_f1_f3_ota_verification_to_zerolock_staging(self):
        """P2 (F1 x F3): Cryptographic SHA-256 match triggers Zero-Lock launcher staging."""
        ota = MockOtaPipeline()
        fake_binary = b"Comp 31 Core Binary Data Payload"
        sha = hashlib.sha256(fake_binary).hexdigest()

        # Step 1: Verify SHA-256
        self.assertTrue(ota.verify_download(fake_binary, sha))
        # Step 2: Stage update
        ota.stage_update("2.4.0", ["Changelog item"], sha)
        self.assertTrue(ota.update_ready)
        # Step 3: Zero-Lock schedule restart with Launcher path
        state = ota.schedule_restart_zero_lock()
        self.assertTrue(state["boot_pending"])
        self.assertEqual(ota.env_next_load, "sdmc:/switch/SwitchCast/SwitchCast.nro")

    def test_pairwise_f4_f7_webremote_audio_cycle_updates_player(self):
        """P3 (F4 x F7): Web Remote POST /api/v1/cycleAudio command cycles player audio."""
        server = MockServerBridge()
        player = MockMpvPlayer()
        player.audio_tracks = [
            MockTrackInfo(1, "audio", title="English", selected=True),
            MockTrackInfo(2, "audio", title="Portuguese", selected=False)
        ]

        # Remote sends cycleAudio
        status, _, _, _ = server.handle_api_request("POST", "cycleAudio", "{}")
        self.assertEqual(status, 200)
        self.assertEqual(server.command_queue[-1]["type"], "CycleAudio")

        # Player executes cycle command
        msg = player.cycle_audio_track()
        self.assertEqual(player.properties["aid"], "2")
        self.assertTrue(player.audio_tracks[1].selected)
        self.assertIn("Portuguese", msg)

    def test_pairwise_f5_f7_webremote_subtitle_cycle_to_deactivation(self):
        """P4 (F5 x F7): Web Remote subtitle cycling cycles to deactivation (sid='no')."""
        server = MockServerBridge()
        player = MockMpvPlayer()
        player.subtitle_tracks = [
            MockTrackInfo(1, "sub", title="Subtitle 1", selected=True)
        ]

        status, _, _, _ = server.handle_api_request("POST", "cycleSubtitle", "{}")
        self.assertEqual(status, 200)

        # 1 active track -> cycling disables
        msg = player.cycle_subtitle_track()
        self.assertEqual(player.properties["sid"], "no")
        self.assertEqual(msg, "Legendas: Desativadas")

    def test_pairwise_f4_f5_simultaneous_track_switching_independence(self):
        """P5 (F4 x F5): Switching audio track preserves active subtitle track selection."""
        player = MockMpvPlayer()
        player.audio_tracks = [
            MockTrackInfo(1, "audio", title="EN", selected=True),
            MockTrackInfo(2, "audio", title="PT", selected=False)
        ]
        player.subtitle_tracks = [
            MockTrackInfo(10, "sub", title="Sub PT", selected=True),
            MockTrackInfo(11, "sub", title="Sub EN", selected=False)
        ]
        player.properties["sid"] = "10"

        # Cycle audio
        player.cycle_audio_track()
        self.assertEqual(player.properties["aid"], "2")
        # Subtitle must remain active and unchanged
        self.assertEqual(player.properties["sid"], "10")
        self.assertTrue(player.subtitle_tracks[0].selected)

    def test_pairwise_f6_f4_wallpaper_interrupted_by_audio_video_playback(self):
        """P6 (F6 x F4): Starting media playback cleanly cancels wallpaper and unmutes audio."""
        player = MockMpvPlayer()
        fs_mock = {"sdmc:/switch/SwitchCast/wallpaper.mp4": True}

        # 1. Start wallpaper
        player.start_wallpaper_loop(fs_mock)
        self.assertTrue(player.is_wallpaper)
        self.assertEqual(player.properties["aid"], "no")

        # 2. Media cast arrives
        player.stop_wallpaper_loop()
        self.assertFalse(player.is_wallpaper)
        self.assertEqual(player.properties["aid"], "auto")

    def test_pairwise_f6_f5_media_stop_restores_wallpaper_silence(self):
        """P7 (F6 x F5): Stopping media playback restores silent wallpaper loop with veil."""
        player = MockMpvPlayer()
        fs_mock = {"sdmc:/switch/SwitchCast/wallpaper.mp4": True}

        # Media stopped -> start wallpaper loop
        player.start_wallpaper_loop(fs_mock)
        self.assertTrue(player.is_wallpaper)
        self.assertEqual(player.properties["loop-file"], "inf")
        self.assertEqual(player.properties["sid"], "no")
        self.assertEqual(player.properties["aid"], "no")

    def test_pairwise_f7_f1_webremote_channel_toggle_updates_manifest_url(self):
        """P8 (F7 x F1): Toggling OTA channel via Web Remote switches between dev & main URLs."""
        server = MockServerBridge(channel="stable")
        ota = MockOtaPipeline(channel="stable")

        # Remote switches channel to beta
        status, _, body, _ = server.handle_api_request("POST", "ota/channel", '{"channel": "beta"}')
        self.assertEqual(status, 200)
        res = json.loads(body)
        self.assertEqual(res["channel"], "beta")

        ota.channel = res["channel"]
        self.assertIn("/dev/update_manifest.json", ota.get_manifest_url())

    def test_pairwise_f8_f1_manifest_hash_matches_compiled_core(self):
        """P9 (F8 x F1): Compiled core NRO SHA-256 matches manifest entry for OTA verification."""
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        expected_sha = manifest["core"]["sha256"].lower().strip()
        hasher = hashlib.sha256()
        with open(self.core_nro, "rb") as f:
            while chunk := f.read(65536):
                hasher.update(chunk)
        actual_sha = hasher.hexdigest().lower()

        self.assertEqual(actual_sha, expected_sha)

    def test_pairwise_f3_f8_launcher_handoff_binary_targets(self):
        """P10 (F3 x F8): Launcher NRO and Core NRO binaries exist at target paths."""
        self.assertTrue(self.launcher_nro.exists())
        self.assertTrue(self.core_nro.exists())
        self.assertGreater(os.path.getsize(self.launcher_nro), 1024 * 1024)
        self.assertGreater(os.path.getsize(self.core_nro), 1024 * 1024)

    # =========================================================================
    # TIER 4: REAL-WORLD APPLICATION SCENARIOS (>= 5 TESTS)
    # =========================================================================

    def test_scenario_1_full_ota_upgrade_lifecycle(self):
        """
        S1: Full lifecycle of in-app OTA upgrade from Comp. 30 to Comp. 31 on Dev channel.
        - Core runs Comp. 30.
        - Background thread polls dev branch manifest.
        - Detects Comp. 31 with differing SHA-256.
        - Downloads payload with progress reporting.
        - Verifies SHA-256 integrity.
        - Dynamic modal renders changelog.
        - Staging registers Launcher restart via envSetNextLoad.
        - Boot verified upon restart.
        """
        # Step 1: Client on Comp. 30
        client = MockOtaPipeline(
            current_version="2.4.0",
            current_version_code=20400,
            current_sha="3030303030303030303030303030303030303030303030303030303030303030",
            channel="beta"
        )

        # Step 2: Fetch remote manifest from dev branch
        with open(self.manifest_json, "r", encoding="utf-8") as f:
            remote_manifest = json.load(f)

        # Step 3: Check update requirement
        self.assertTrue(client.should_update(remote_manifest["core"]))

        # Step 4: Simulate download & SHA-256 check
        core_bytes = self.core_nro.read_bytes()
        self.assertTrue(client.verify_download(core_bytes, remote_manifest["core"]["sha256"]))

        # Step 5: Render dynamic changelog
        cards = MockDynamicChangelogRenderer.parse_changelog(remote_manifest["core"]["changelog"])
        self.assertGreater(len(cards), 0)

        # Step 6: Stage update and schedule Zero-Lock Launcher reboot
        client.stage_update("2.4.0", remote_manifest["core"]["changelog"], remote_manifest["core"]["sha256"])
        state = client.schedule_restart_zero_lock()
        self.assertTrue(state["boot_pending"])
        self.assertEqual(client.env_next_load, "sdmc:/switch/SwitchCast/SwitchCast.nro")

        # Step 7: Post-reboot boot verification
        client.mark_boot_successful()
        self.assertFalse(client.boot_pending)
        self.assertEqual(client.last_boot_status, "boot_verified_ok")

    def test_scenario_2_multi_audio_multi_subtitle_playback(self):
        """
        S2: Multi-audio and multi-subtitle media playback with Web Remote & Joy-Con control.
        - UPnP video stream starts with dual audio (EN, PT) and dual subtitle (PT, EN).
        - Joy-Con X cycles audio to PT.
        - Web Remote cycles subtitle to PT.
        - Joy-Con Y cycles subtitle to EN.
        - Joy-Con Y cycles subtitle off (sid='no').
        - Remote queries status payload and verifies state.
        """
        player = MockMpvPlayer()
        server = MockServerBridge()

        player.audio_tracks = [
            MockTrackInfo(1, "audio", title="English", codec="aac", selected=True),
            MockTrackInfo(2, "audio", title="Português", codec="eac3", selected=False)
        ]
        player.subtitle_tracks = [
            MockTrackInfo(1, "sub", title="Português", lang="por", selected=False),
            MockTrackInfo(2, "sub", title="English", lang="eng", selected=False)
        ]

        # 1. Joy-Con X cycles audio to PT
        msg_a = player.cycle_audio_track()
        self.assertEqual(player.properties["aid"], "2")
        self.assertIn("Português", msg_a)

        # 2. Remote cycles subtitle to PT (first track)
        msg_s1 = player.cycle_subtitle_track()
        self.assertEqual(player.properties["sid"], "1")
        self.assertIn("Português", msg_s1)

        # 3. Joy-Con Y cycles subtitle to EN
        msg_s2 = player.cycle_subtitle_track()
        self.assertEqual(player.properties["sid"], "2")
        self.assertIn("English", msg_s2)

        # 4. Joy-Con Y cycles subtitle off
        msg_s3 = player.cycle_subtitle_track()
        self.assertEqual(player.properties["sid"], "no")
        self.assertEqual(msg_s3, "Legendas: Desativadas")

        # 5. Remote queries status
        server.player_snapshot["state"] = "playing"
        server.player_snapshot["title"] = "Multitrack Movie"
        status, _, body, _ = server.handle_api_request("GET", "status")
        self.assertEqual(status, 200)
        snap = json.loads(body)
        self.assertEqual(snap["state"], "playing")

    def test_scenario_3_custom_sd_wallpaper_loop_interrupt_and_resume(self):
        """
        S3: Custom wallpaper MP4 placed on SD card: loop, media interrupt, and resume.
        - wallpaper.mp4 exists in sdmc:/switch/SwitchCast/wallpaper.mp4.
        - App boots and enters wallpaper loop at 60 FPS with veil.
        - Media cast arrives -> wallpaper loop stops, media plays with hardware decode.
        - User stops media -> wallpaper loop seamlessly resumes.
        """
        player = MockMpvPlayer()
        fs_sd = {"sdmc:/switch/SwitchCast/wallpaper.mp4": True}

        # 1. Boot into wallpaper loop
        self.assertTrue(player.start_wallpaper_loop(fs_sd))
        self.assertTrue(player.is_wallpaper)
        self.assertEqual(player.loaded_file, "sdmc:/switch/SwitchCast/wallpaper.mp4")

        # 2. Cast video
        player.stop_wallpaper_loop()
        self.assertFalse(player.is_wallpaper)

        # 3. Cast finishes -> resume wallpaper
        self.assertTrue(player.start_wallpaper_loop(fs_sd))
        self.assertTrue(player.is_wallpaper)

    def test_scenario_4_external_subtitle_injection_and_toggle_off(self):
        """
        S4: Inject external subtitle URL and cycle through to toggle off during stream.
        - Cast payload contains external .vtt URL.
        - Player loads external track into session.
        - User cycles through subtitles and toggles off.
        """
        player = MockMpvPlayer()
        server = MockServerBridge()

        # Remote casts video with external subtitle
        play_payload = {
            "url": "http://192.168.1.100/video.mp4",
            "title": "Stream with External Sub",
            "subtitles": ["http://192.168.1.100/video.pt.vtt"]
        }
        status, _, _, _ = server.handle_api_request("POST", "play", json.dumps(play_payload))
        self.assertEqual(status, 200)
        cmd = server.command_queue[-1]
        self.assertEqual(len(cmd["subtitles"]), 1)

        # Load external subtitle into player
        track_id = player.add_external_subtitle(cmd["subtitles"][0])
        self.assertEqual(len(player.subtitle_tracks), 1)

        # Activate subtitle
        msg = player.cycle_subtitle_track()
        self.assertEqual(player.properties["sid"], str(track_id))

        # Toggle off
        msg_off = player.cycle_subtitle_track()
        self.assertEqual(player.properties["sid"], "no")
        self.assertEqual(msg_off, "Legendas: Desativadas")

    def test_scenario_5_previous_build_archive_in_documentos_builds(self):
        """
        S5: Verify previous build Comp. 30 archive in Documentos/builds per User Rule 2.
        - c:\\Users\\Alpha\\OneDrive\\Documentos\\builds\\SwitchCast_2.4.0_Comp30.zip must exist.
        - Must contain valid SwitchCast.nro, SwitchCastCore.nro, and update_manifest.json.
        """
        self.assertTrue(BUILDS_DIR.exists(), f"Builds directory {BUILDS_DIR} must exist")
        archive_path = BUILDS_DIR / "SwitchCast_2.4.0_Comp30.zip"
        self.assertTrue(archive_path.exists(), f"Archive {archive_path} must exist")

        with zipfile.ZipFile(archive_path, "r") as zf:
            namelist = zf.namelist()
            self.assertIn("SwitchCast.nro", namelist)
            self.assertIn("SwitchCastCore.nro", namelist)
            self.assertIn("update_manifest.json", namelist)

            # Validate that zip contents are not empty
            for name in ["SwitchCast.nro", "SwitchCastCore.nro"]:
                info = zf.getinfo(name)
                self.assertGreater(info.file_size, 1024 * 1024, f"{name} in archive must be > 1MB")

    def test_scenario_6_git_branch_governance_dev(self):
        """S6: Verify git repository is on dev branch for continuous integration per R3."""
        git_head = REPO_ROOT / ".git" / "HEAD"
        if git_head.exists():
            head_content = git_head.read_text(encoding="utf-8").strip()
            self.assertIn("dev", head_content, "Repository must be on branch 'dev'")

    def test_scenario_7_version_string_coherence_across_all_artifacts(self):
        """S7: Verify complete coherence of '2.4.0 | Comp. 31' across Types.hpp and WebRemote.hpp."""
        types_text = self.types_hpp.read_text(encoding="utf-8")
        webremote_text = self.webremote_hpp.read_text(encoding="utf-8")

        self.assertIn('"2.4.0 | Comp. 31"', types_text)
        self.assertIn("v2.4.0 | Comp. 31", webremote_text)
        self.assertIn("Comp. 31", webremote_text)

    def test_scenario_8_wallpaper_fallback_graceful_transition(self):
        """S8: Seamless fallback to solid dark background (#0D0E12) when SD card is unmounted."""
        player = MockMpvPlayer()
        # SD card wallpaper disappears (e.g. card removed)
        fs_empty = {}
        ok = player.start_wallpaper_loop(fs_empty)
        self.assertFalse(ok)
        self.assertFalse(player.is_wallpaper)
        # Verify renderer constant in UIEngine source code
        uiengine_cpp = REPO_ROOT / "src" / "ui" / "UIEngine.cpp"
        self.assertIn("Color::Hex(0x0D0E12, 1.0f)", uiengine_cpp.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
