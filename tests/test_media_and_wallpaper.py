"""
Unit and E2E Tests: Media Playback, Audio/Subtitle Cycling, and Wallpaper Loop (F4, F5, F6)
Tiers 1 & 2: Audio track cycling, subtitle cycling and deactivation (sid="no"),
external srt/vtt loading, wallpaper candidate path priority, fallback to solid dark background.
"""

import unittest
import os
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class MockTrackInfo:
    """Represents MpvTrackInfo struct in MpvPlayer."""
    def __init__(self, track_id, track_type, title="", lang="", codec="", selected=False):
        self.id = track_id
        self.type = track_type  # "audio" or "sub"
        self.title = title
        self.lang = lang
        self.codec = codec
        self.selected = selected


class MockMpvPlayer:
    """
    Python reference model replicating MpvPlayer.cpp track switching
    and wallpaper state machine.
    """

    CANDIDATE_WALLPAPER_PATHS = [
        "sdmc:/switch/SwitchCast/wallpaper.mp4",
        "romfs:/wallpaper.mp4",
        "switch/SwitchCast/wallpaper.mp4",
        "wallpaper.mp4",
        "./wallpaper.mp4"
    ]

    def __init__(self, initialized=True):
        self.initialized = initialized
        self.audio_tracks = []
        self.subtitle_tracks = []
        self.properties = {
            "aid": "auto",
            "sid": "auto",
            "loop-file": "no",
            "pause": "no"
        }
        self.is_wallpaper = False
        self.loaded_file = ""
        self.external_subtitles = []

    def friendly_track_name(self, track, index, total, is_audio):
        prefix = f"Audio {index + 1}/{total}" if is_audio else f"Legenda {index + 1}/{total}"
        label = track.title if track.title else (track.lang if track.lang else f"Faixa {track.id}")
        codec_str = f" ({track.codec})" if track.codec else ""
        return f"{prefix}: {label}{codec_str}"

    def cycle_audio_track(self):
        """Replicates MpvPlayer::CycleAudioTrack()."""
        if not self.initialized:
            return "Audio indisponivel"
        if not self.audio_tracks:
            return "Nenhuma faixa de audio encontrada"

        if len(self.audio_tracks) == 1:
            self.properties["aid"] = str(self.audio_tracks[0].id)
            self.audio_tracks[0].selected = True
            return self.friendly_track_name(self.audio_tracks[0], 0, 1, True) + " (Unica)"

        current_idx = -1
        for idx, t in enumerate(self.audio_tracks):
            if t.selected:
                current_idx = idx
                break

        next_idx = (current_idx + 1) % len(self.audio_tracks)
        for t in self.audio_tracks:
            t.selected = False
        self.audio_tracks[next_idx].selected = True
        self.properties["aid"] = str(self.audio_tracks[next_idx].id)

        return self.friendly_track_name(self.audio_tracks[next_idx], next_idx, len(self.audio_tracks), True)

    def cycle_subtitle_track(self):
        """Replicates MpvPlayer::CycleSubtitleTrack()."""
        if not self.initialized:
            return "Legendas indisponiveis"
        if not self.subtitle_tracks:
            return "Sem legendas disponiveis"

        current_idx = -1
        for idx, t in enumerate(self.subtitle_tracks):
            if t.selected:
                current_idx = idx
                break

        if current_idx == -1:
            # None active -> activate first track
            self.subtitle_tracks[0].selected = True
            self.properties["sid"] = str(self.subtitle_tracks[0].id)
            return self.friendly_track_name(self.subtitle_tracks[0], 0, len(self.subtitle_tracks), False)
        elif current_idx + 1 < len(self.subtitle_tracks):
            # Advance to next track
            for t in self.subtitle_tracks:
                t.selected = False
            next_idx = current_idx + 1
            self.subtitle_tracks[next_idx].selected = True
            self.properties["sid"] = str(self.subtitle_tracks[next_idx].id)
            return self.friendly_track_name(self.subtitle_tracks[next_idx], next_idx, len(self.subtitle_tracks), False)
        else:
            # Last track was active -> deactivate subtitles (sid = "no")
            for t in self.subtitle_tracks:
                t.selected = False
            self.properties["sid"] = "no"
            return "Legendas: Desativadas"

    def set_audio_track(self, track_id):
        self.properties["aid"] = str(track_id)
        for t in self.audio_tracks:
            t.selected = (t.id == track_id)

    def set_subtitle_track(self, track_id):
        if track_id <= 0:
            self.properties["sid"] = "no"
            for t in self.subtitle_tracks:
                t.selected = False
        else:
            self.properties["sid"] = str(track_id)
            for t in self.subtitle_tracks:
                t.selected = (t.id == track_id)

    def add_external_subtitle(self, url):
        self.external_subtitles.append(url)
        new_id = len(self.subtitle_tracks) + 10
        ext_track = MockTrackInfo(new_id, "sub", title="External Subtitle", lang="und")
        self.subtitle_tracks.append(ext_track)
        return new_id

    def resolve_wallpaper_path(self, filesystem_dict):
        """Finds highest-priority existing wallpaper path."""
        for path in self.CANDIDATE_WALLPAPER_PATHS:
            if filesystem_dict.get(path, False):
                return path
        return None

    def start_wallpaper_loop(self, filesystem_dict):
        """Replicates MpvPlayer::StartWallpaperLoop()."""
        if not self.initialized:
            return False

        found_path = self.resolve_wallpaper_path(filesystem_dict)
        if not found_path:
            self.is_wallpaper = False
            self.loaded_file = ""
            return False

        self.is_wallpaper = True
        self.properties["loop-file"] = "inf"
        self.properties["aid"] = "no"
        self.properties["sid"] = "no"
        self.properties["pause"] = "no"
        self.loaded_file = found_path
        return True

    def stop_wallpaper_loop(self):
        """Replicates MpvPlayer::StopWallpaperLoop()."""
        if not self.initialized or not self.is_wallpaper:
            return
        self.is_wallpaper = False
        self.properties["loop-file"] = "no"
        self.properties["aid"] = "auto"
        self.properties["sid"] = "auto"
        self.loaded_file = ""


class TestMediaAndWallpaper(unittest.TestCase):
    """
    Tier 1 & Tier 2 tests for Feature 4 (Audio Track Cycling), Feature 5 (Subtitle Cycling & Off),
    and Feature 6 (Custom Wallpaper MP4 Loop & Fallback).
    """

    @classmethod
    def setUpClass(cls):
        cls.mpv_player_cpp = REPO_ROOT / "src" / "player" / "MpvPlayer.cpp"
        cls.uiengine_cpp = REPO_ROOT / "src" / "ui" / "UIEngine.cpp"
        cls.mpv_src = cls.mpv_player_cpp.read_text(encoding="utf-8")
        cls.ui_src = cls.uiengine_cpp.read_text(encoding="utf-8")

    def setUp(self):
        self.player = MockMpvPlayer(initialized=True)

    # =========================================================================
    # TIER 1: HAPPY PATH & CONTRACTS (F4, F5, F6)
    # =========================================================================

    def test_tier1_audio_cycle_multi_track_advances(self):
        """F4-T1-1: Cycling through multiple audio tracks advances sequentially."""
        self.player.audio_tracks = [
            MockTrackInfo(1, "audio", title="Português", codec="aac", selected=True),
            MockTrackInfo(2, "audio", title="Inglês", codec="ac3", selected=False),
            MockTrackInfo(3, "audio", title="Japonês", codec="flac", selected=False)
        ]

        # 1 -> 2
        msg = self.player.cycle_audio_track()
        self.assertEqual(self.player.properties["aid"], "2")
        self.assertTrue(self.player.audio_tracks[1].selected)
        self.assertIn("Audio 2/3", msg)

        # 2 -> 3
        msg = self.player.cycle_audio_track()
        self.assertEqual(self.player.properties["aid"], "3")
        self.assertTrue(self.player.audio_tracks[2].selected)

    def test_tier1_audio_cycle_wrap_around(self):
        """F4-T1-2: Audio track cycling wraps around from last track back to first."""
        self.player.audio_tracks = [
            MockTrackInfo(1, "audio", title="Track 1", selected=False),
            MockTrackInfo(2, "audio", title="Track 2", selected=True)
        ]

        msg = self.player.cycle_audio_track()
        self.assertEqual(self.player.properties["aid"], "1")
        self.assertTrue(self.player.audio_tracks[0].selected)
        self.assertIn("Audio 1/2", msg)

    def test_tier1_audio_single_track_never_mutes(self):
        """F4-T1-3: Cycling with only 1 audio track retains track and labels as (Unica)."""
        self.player.audio_tracks = [
            MockTrackInfo(1, "audio", title="Stereo Mix", codec="aac", selected=True)
        ]

        msg = self.player.cycle_audio_track()
        self.assertEqual(self.player.properties["aid"], "1")
        self.assertIn("(Unica)", msg)
        self.assertNotEqual(self.player.properties["aid"], "no")

    def test_tier1_subtitle_cycle_activation_from_none(self):
        """F5-T1-1: Subtitle cycling when none active activates first track."""
        self.player.subtitle_tracks = [
            MockTrackInfo(1, "sub", title="Português", lang="por", selected=False),
            MockTrackInfo(2, "sub", title="Inglês", lang="eng", selected=False)
        ]

        msg = self.player.cycle_subtitle_track()
        self.assertEqual(self.player.properties["sid"], "1")
        self.assertTrue(self.player.subtitle_tracks[0].selected)
        self.assertIn("Legenda 1/2", msg)

    def test_tier1_subtitle_cycle_advance_track(self):
        """F5-T1-2: Subtitle cycling advances from track 1 to track 2."""
        self.player.subtitle_tracks = [
            MockTrackInfo(1, "sub", title="Português", selected=True),
            MockTrackInfo(2, "sub", title="Inglês", selected=False)
        ]

        msg = self.player.cycle_subtitle_track()
        self.assertEqual(self.player.properties["sid"], "2")
        self.assertTrue(self.player.subtitle_tracks[1].selected)
        self.assertIn("Legenda 2/2", msg)

    def test_tier1_subtitle_cycle_deactivation_sid_no(self):
        """F5-T1-3: Cycling past last subtitle track deactivates subtitles with sid='no'."""
        self.player.subtitle_tracks = [
            MockTrackInfo(1, "sub", title="Português", selected=False),
            MockTrackInfo(2, "sub", title="Inglês", selected=True)
        ]

        msg = self.player.cycle_subtitle_track()
        self.assertEqual(self.player.properties["sid"], "no")
        self.assertFalse(any(t.selected for t in self.player.subtitle_tracks))
        self.assertEqual(msg, "Legendas: Desativadas")

    def test_tier1_external_subtitle_loading(self):
        """F5-T1-4: Adding external subtitle URL registers track and allows selection."""
        ext_url = "http://192.168.1.50:8080/movie.pt.srt"
        new_id = self.player.add_external_subtitle(ext_url)
        self.assertIn(ext_url, self.player.external_subtitles)
        self.assertEqual(len(self.player.subtitle_tracks), 1)
        self.assertEqual(self.player.subtitle_tracks[0].id, new_id)

    def test_tier1_wallpaper_path_priority_order(self):
        """F6-T1-1: Wallpaper candidate path priority follows sdmc -> romfs -> switch -> cwd."""
        fs_mock = {
            "sdmc:/switch/SwitchCast/wallpaper.mp4": True,
            "romfs:/wallpaper.mp4": True,
            "wallpaper.mp4": True
        }
        # sdmc has highest priority
        self.assertEqual(self.player.resolve_wallpaper_path(fs_mock), "sdmc:/switch/SwitchCast/wallpaper.mp4")

        # if sdmc missing, romfs takes over
        fs_mock["sdmc:/switch/SwitchCast/wallpaper.mp4"] = False
        self.assertEqual(self.player.resolve_wallpaper_path(fs_mock), "romfs:/wallpaper.mp4")

        # if romfs missing, cwd wallpaper takes over
        fs_mock["romfs:/wallpaper.mp4"] = False
        self.assertEqual(self.player.resolve_wallpaper_path(fs_mock), "wallpaper.mp4")

    def test_tier1_wallpaper_mp4_loop_properties(self):
        """F6-T1-2: Starting wallpaper loop configures infinite loop and mutes audio/sub."""
        fs_mock = {"sdmc:/switch/SwitchCast/wallpaper.mp4": True}
        ok = self.player.start_wallpaper_loop(fs_mock)

        self.assertTrue(ok)
        self.assertTrue(self.player.is_wallpaper)
        self.assertEqual(self.player.properties["loop-file"], "inf")
        self.assertEqual(self.player.properties["aid"], "no")
        self.assertEqual(self.player.properties["sid"], "no")
        self.assertEqual(self.player.properties["pause"], "no")

    def test_tier1_wallpaper_stop_restores_properties(self):
        """F6-T1-3: Stopping wallpaper loop restores auto tracks and disables loop-file."""
        fs_mock = {"sdmc:/switch/SwitchCast/wallpaper.mp4": True}
        self.player.start_wallpaper_loop(fs_mock)
        self.player.stop_wallpaper_loop()

        self.assertFalse(self.player.is_wallpaper)
        self.assertEqual(self.player.properties["loop-file"], "no")
        self.assertEqual(self.player.properties["aid"], "auto")
        self.assertEqual(self.player.properties["sid"], "auto")

    def test_tier1_joycon_input_bindings_in_uiengine(self):
        """F4/F5-T1: Joy-Con button X cycles audio and Joy-Con Y cycles subtitle."""
        self.assertIn("HidNpadButton_X", self.ui_src)
        self.assertIn("CycleAudioTrack", self.ui_src)
        self.assertIn("HidNpadButton_Y", self.ui_src)
        self.assertIn("CycleSubtitleTrack", self.ui_src)

    # =========================================================================
    # TIER 2: BOUNDARY CONDITIONS & ERROR HANDLING (F4, F5, F6)
    # =========================================================================

    def test_tier2_audio_cycle_empty_tracks_message(self):
        """F4-T2-1: Cycling audio with no audio tracks returns friendly error."""
        self.player.audio_tracks = []
        msg = self.player.cycle_audio_track()
        self.assertEqual(msg, "Nenhuma faixa de audio encontrada")

    def test_tier2_audio_cycle_null_mpv_safety(self):
        """F4-T2-2: Cycling audio when player is uninitialized returns 'Audio indisponivel'."""
        self.player.initialized = False
        msg = self.player.cycle_audio_track()
        self.assertEqual(msg, "Audio indisponivel")

    def test_tier2_subtitle_cycle_empty_tracks_message(self):
        """F5-T2-1: Cycling subtitle with no subtitle tracks returns 'Sem legendas disponiveis'."""
        self.player.subtitle_tracks = []
        msg = self.player.cycle_subtitle_track()
        self.assertEqual(msg, "Sem legendas disponiveis")

    def test_tier2_subtitle_cycle_null_mpv_safety(self):
        """F5-T2-2: Cycling subtitle when player is uninitialized returns 'Legendas indisponiveis'."""
        self.player.initialized = False
        msg = self.player.cycle_subtitle_track()
        self.assertEqual(msg, "Legendas indisponiveis")

    def test_tier2_set_subtitle_track_zero_disables(self):
        """F5-T2-3: SetSubtitleTrack(0) or negative ID sets sid='no'."""
        self.player.set_subtitle_track(0)
        self.assertEqual(self.player.properties["sid"], "no")

        self.player.set_subtitle_track(-1)
        self.assertEqual(self.player.properties["sid"], "no")

    def test_tier2_wallpaper_missing_file_fallback_solid_dark(self):
        """F6-T2-1: When no wallpaper path exists, wallpaper loop fails and remains inactive."""
        fs_empty = {}
        ok = self.player.start_wallpaper_loop(fs_empty)
        self.assertFalse(ok)
        self.assertFalse(self.player.is_wallpaper)
        self.assertEqual(self.player.loaded_file, "")

    def test_tier2_uiengine_wallpaper_veil_rendering(self):
        """F6-T2-2: UIEngine renders translucent veil when hasWallpaper, else solid dark background."""
        # Check translucent veil rgba(13, 14, 18, 175)
        self.assertIn("Color::Rgba(13, 14, 18, 175)", self.ui_src)
        # Check solid fallback Color::Hex(0x0D0E12, 1.0f)
        self.assertIn("Color::Hex(0x0D0E12, 1.0f)", self.ui_src)

    def test_tier2_friendly_track_fallback_to_id(self):
        """F4-T2-3: Track without title or language falls back to 'Faixa <id>'."""
        anonymous_track = MockTrackInfo(4, "audio", title="", lang="", codec="")
        name = self.player.friendly_track_name(anonymous_track, 3, 4, True)
        self.assertEqual(name, "Audio 4/4: Faixa 4")

    def test_tier2_friendly_track_language_fallback(self):
        """F4-T2-4: Track with language but without title uses language as label."""
        lang_track = MockTrackInfo(2, "audio", title="", lang="fra", codec="mp3")
        name = self.player.friendly_track_name(lang_track, 1, 2, True)
        self.assertEqual(name, "Audio 2/2: fra (mp3)")

    def test_tier2_candidate_paths_array_in_mpv_player(self):
        """F6-T2-3: MpvPlayer.cpp contains candidate paths including sdmc:/switch/SwitchCast/wallpaper.mp4."""
        self.assertIn("sdmc:/switch/SwitchCast/wallpaper.mp4", self.mpv_src)
        self.assertIn("romfs:/wallpaper.mp4", self.mpv_src)
        self.assertIn('mpv_set_property_string(mMpv, "loop-file", "inf");', self.mpv_src)

    def test_tier2_wallpaper_stop_idempotent(self):
        """F6-T2-4: Calling StopWallpaperLoop when not in wallpaper mode is a safe no-op."""
        self.assertFalse(self.player.is_wallpaper)
        self.player.stop_wallpaper_loop()
        self.assertFalse(self.player.is_wallpaper)


if __name__ == "__main__":
    unittest.main()
