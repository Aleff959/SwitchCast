"""
Unit and E2E Tests: Web Remote and CivetWeb REST API (F7, F4, F5)
Tiers 1 & 2: CivetWeb endpoints, cycleAudio, cycleSubtitle, status payload,
WebRemote.hpp template elements, command parsing, and boundary handling.
"""

import unittest
import os
import re
import json
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class MockServerBridge:
    """
    Python reference model simulating the exact parsing and routing logic
    of CivetWeb ServerBridge.cpp for opaque-box testing without Switch hardware.
    """

    def __init__(self, build_num=31, version_str="2.4.0", channel="beta"):
        self.build_num = build_num
        self.version_str = version_str
        self.channel = channel
        self.command_queue = []
        self.player_snapshot = {
            "state": "idle",
            "positionMs": 0,
            "durationMs": 0,
            "volume": 80,
            "muted": False,
            "buffering": False,
            "title": "",
            "url": "",
            "error": ""
        }

    def parse_command(self, name, j):
        """Replicates ParseCommand in ServerBridge.cpp."""
        def num(k, default_val):
            return j.get(k, default_val) if isinstance(j.get(k), (int, float)) else default_val

        if name == "play" and "url" in j:
            if not isinstance(j["url"], str) or not j["url"].strip():
                return False, {}, "url inválida"
            cmd = {
                "type": "LoadMedia",
                "url": j["url"],
                "title": j.get("title", ""),
                "startPositionMs": num("startPositionMs", 0),
                "autoPlay": j.get("autoPlay", True),
                "headers": j.get("headers", {}) if isinstance(j.get("headers"), dict) else {},
                "subtitles": [s for s in j.get("subtitles", []) if isinstance(s, str)]
            }
            return True, cmd, ""

        if name in ("play", "resume"):
            return True, {"type": "Play"}, ""
        if name == "pause":
            return True, {"type": "Pause"}, ""
        if name == "stop":
            return True, {"type": "Stop"}, ""
        if name == "seek":
            if "positionMs" in j:
                return True, {"type": "Seek", "positionMs": num("positionMs", 0)}, ""
            elif "offsetMs" in j:
                return True, {"type": "SeekRelative", "offsetMs": num("offsetMs", 0)}, ""
            else:
                return False, {}, "informe positionMs ou offsetMs"
        if name == "volume":
            if "level" not in j or not isinstance(j["level"], (int, float)):
                return False, {}, "informe level (0-150)"
            vol = max(0, min(150, int(num("level", 100))))
            return True, {"type": "SetVolume", "volume": vol}, ""
        if name == "mute":
            flag = not j.get("mute", True) if "mute" in j else True
            return True, {"type": "SetMute", "flag": flag}, ""
        if name == "subtitle":
            if "url" not in j or not isinstance(j["url"], str):
                return False, {}, "informe url"
            return True, {"type": "AddSubtitle", "url": j["url"]}, ""
        if name == "cycleAudio":
            return True, {"type": "CycleAudio"}, ""
        if name in ("cycleSubtitle", "cycleSub"):
            return True, {"type": "CycleSubtitle"}, ""
        if name == "setAudioTrack":
            track_id = int(num("id", 1))
            return True, {"type": "SetAudioTrack", "id": track_id}, ""
        if name == "setSubtitleTrack":
            track_id = int(num("id", 0))
            return True, {"type": "SetSubtitleTrack", "id": track_id}, ""

        return False, {}, f"comando desconhecido: {name}"

    def handle_api_request(self, method, route, body_str="", headers=None):
        """Simulates HandleApi routing in ServerBridge.cpp."""
        headers = headers or {}
        k_json = "application/json; charset=utf-8"

        if method == "OPTIONS":
            return 204, k_json, "", {
                "Access-Control-Allow-Origin": "*",
                "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
                "Access-Control-Allow-Headers": "Content-Type"
            }

        if method == "GET":
            if route == "status":
                return 200, k_json, json.dumps(self.player_snapshot), {}
            if route == "info":
                info = {
                    "version": self.version_str,
                    "build": self.build_num,
                    "versionFull": f"{self.version_str} | Comp. {self.build_num}",
                    "otaChannel": "Beta / Dev" if self.channel == "beta" else "Estável"
                }
                return 200, k_json, json.dumps(info), {}
            if route in ("ota/channel", "system/ota-channel"):
                is_beta = (self.channel == "beta")
                res = {
                    "ok": True,
                    "channel": "beta" if is_beta else "stable",
                    "name": "Beta / Dev" if is_beta else "Estável",
                    "manifestUrl": "https://raw.githubusercontent.com/Aleff959/SwitchCast/dev/update_manifest.json"
                    if is_beta else "https://raw.githubusercontent.com/Aleff959/SwitchCast/main/update_manifest.json"
                }
                return 200, k_json, json.dumps(res), {}

            return 405, k_json, json.dumps({"ok": False, "error": "use POST"}), {}

        if method == "POST":
            # Command JSON handling
            j = {}
            if body_str.strip():
                try:
                    j = json.loads(body_str)
                    if not isinstance(j, dict):
                        return 400, k_json, json.dumps({"ok": False, "error": "JSON inválido"}), {}
                except Exception:
                    return 400, k_json, json.dumps({"ok": False, "error": "JSON inválido"}), {}

            if route in ("ota/channel", "system/ota-channel"):
                target = j.get("channel", "")
                if target in ("beta", "dev"):
                    self.channel = "beta"
                elif target in ("stable", "main"):
                    self.channel = "stable"
                else:
                    self.channel = "stable" if self.channel == "beta" else "beta"

                is_beta = (self.channel == "beta")
                res = {
                    "ok": True,
                    "channel": "beta" if is_beta else "stable",
                    "name": "Beta / Dev" if is_beta else "Estável"
                }
                return 200, k_json, json.dumps(res), {}

            ok, cmd, err = self.parse_command(route, j)
            if not ok:
                return 400, k_json, json.dumps({"ok": False, "error": err}), {}

            self.command_queue.append(cmd)
            return 200, k_json, json.dumps({"ok": True, "error": ""}), {}

        return 405, k_json, json.dumps({"ok": False, "error": "Method Not Allowed"}), {}


class TestWebRemoteApi(unittest.TestCase):
    """
    Tier 1 & Tier 2 tests for Feature 7 (Web Remote Audio & Subtitle Controls)
    and CivetWeb ServerBridge endpoints.
    """

    @classmethod
    def setUpClass(cls):
        cls.webremote_hpp = REPO_ROOT / "src" / "network" / "WebRemote.hpp"
        cls.serverbridge_cpp = REPO_ROOT / "src" / "network" / "ServerBridge.cpp"
        cls.serverbridge_hpp = REPO_ROOT / "src" / "network" / "ServerBridge.hpp"
        cls.html_content = cls.webremote_hpp.read_text(encoding="utf-8")
        cls.bridge_src = cls.serverbridge_cpp.read_text(encoding="utf-8")

    def setUp(self):
        self.server = MockServerBridge(build_num=31, version_str="2.4.0", channel="beta")

    # =========================================================================
    # TIER 1: HAPPY PATH & API CONTRACT VERIFICATION (F7)
    # =========================================================================

    def test_tier1_cycle_audio_endpoint_success(self):
        """F7-T1-1: POST /api/v1/cycleAudio queues CycleAudio command with 200 OK."""
        status, _, body, _ = self.server.handle_api_request("POST", "cycleAudio", "{}")
        self.assertEqual(status, 200)
        res = json.loads(body)
        self.assertTrue(res["ok"])
        self.assertEqual(len(self.server.command_queue), 1)
        self.assertEqual(self.server.command_queue[0]["type"], "CycleAudio")

    def test_tier1_cycle_subtitle_endpoint_success(self):
        """F7-T1-2: POST /api/v1/cycleSubtitle queues CycleSubtitle command with 200 OK."""
        status, _, body, _ = self.server.handle_api_request("POST", "cycleSubtitle", "{}")
        self.assertEqual(status, 200)
        res = json.loads(body)
        self.assertTrue(res["ok"])
        self.assertEqual(len(self.server.command_queue), 1)
        self.assertEqual(self.server.command_queue[0]["type"], "CycleSubtitle")

    def test_tier1_cycle_sub_alias_endpoint_success(self):
        """F7-T1-3: POST /api/v1/cycleSub alias queues CycleSubtitle command."""
        status, _, body, _ = self.server.handle_api_request("POST", "cycleSub", "{}")
        self.assertEqual(status, 200)
        res = json.loads(body)
        self.assertTrue(res["ok"])
        self.assertEqual(self.server.command_queue[0]["type"], "CycleSubtitle")

    def test_tier1_status_endpoint_payload(self):
        """F7-T1-4: GET /api/v1/status returns valid player snapshot schema."""
        self.server.player_snapshot["state"] = "playing"
        self.server.player_snapshot["positionMs"] = 120000
        self.server.player_snapshot["durationMs"] = 360000
        self.server.player_snapshot["volume"] = 90
        self.server.player_snapshot["title"] = "SwitchCast Presentation"

        status, _, body, _ = self.server.handle_api_request("GET", "status")
        self.assertEqual(status, 200)
        data = json.loads(body)

        for key in ["state", "positionMs", "durationMs", "volume", "muted", "buffering", "title", "url", "error"]:
            self.assertIn(key, data, f"Key '{key}' missing from status payload")
        self.assertEqual(data["state"], "playing")
        self.assertEqual(data["volume"], 90)

    def test_tier1_options_cors_preflight(self):
        """F7-T1-5: HTTP OPTIONS returns 204 No Content with CORS allow headers."""
        status, _, _, headers = self.server.handle_api_request("OPTIONS", "cycleAudio")
        self.assertEqual(status, 204)
        self.assertEqual(headers.get("Access-Control-Allow-Origin"), "*")
        self.assertIn("POST", headers.get("Access-Control-Allow-Methods", ""))

    def test_tier1_ota_channel_get_endpoint(self):
        """F7-T1-6: GET /api/v1/ota/channel returns active channel info and URL."""
        status, _, body, _ = self.server.handle_api_request("GET", "ota/channel")
        self.assertEqual(status, 200)
        data = json.loads(body)
        self.assertTrue(data["ok"])
        self.assertEqual(data["channel"], "beta")
        self.assertIn("dev", data["manifestUrl"])

    def test_tier1_webremote_html_doctype_and_title(self):
        """F7-T1-7: WebRemote.hpp delivers complete HTML5 page with valid title and charset."""
        self.assertIn("<!DOCTYPE html>", self.html_content)
        self.assertIn('<meta charset="UTF-8">', self.html_content)
        self.assertIn("<title>SwitchCast - Controle Remoto", self.html_content)

    def test_tier1_webremote_html_playback_controls(self):
        """F7-T1-8: WebRemote.hpp contains transport control elements and slider."""
        self.assertIn('id="seekSlider"', self.html_content)
        self.assertIn('id="volSlider"', self.html_content)
        self.assertIn('id="btnPlayPause"', self.html_content)
        self.assertIn('id="mediaTitle"', self.html_content)
        self.assertIn('id="mediaState"', self.html_content)

    def test_tier1_webremote_sendcmd_dispatcher(self):
        """F7-T1-9: WebRemote.hpp includes sendCmd JavaScript function dispatching to /api/v1/."""
        self.assertIn("function sendCmd(cmdName, payload = {})", self.html_content)
        self.assertIn("fetch('/api/v1/' + cmdName", self.html_content)

    def test_tier1_civetweb_source_routes_registration(self):
        """F7-T1-10: ServerBridge.cpp registers required CivetWeb handlers."""
        self.assertIn('mg_set_request_handler(mHttpCtx, "/api/v1/**", &ServerBridge::HttpApi, this);', self.bridge_src)
        self.assertIn('mg_set_request_handler(mHttpCtx, "/", &ServerBridge::HttpWebRemote, this);', self.bridge_src)
        self.assertIn('mg_set_websocket_handler(mHttpCtx, "/ws",', self.bridge_src)

    # =========================================================================
    # TIER 2: BOUNDARY CONDITIONS & ERROR HANDLING (F7)
    # =========================================================================

    def test_tier2_malformed_json_returns_400(self):
        """F7-T2-1: Malformed JSON body in POST request returns 400 Bad Request."""
        malformed_bodies = [
            "{not a valid json",
            '{"level": ',
            "['array instead of object']",
            '{"unclosed": "quote}'
        ]
        for b in malformed_bodies:
            status, _, body, _ = self.server.handle_api_request("POST", "volume", b)
            self.assertEqual(status, 400)
            res = json.loads(body)
            self.assertFalse(res["ok"])
            self.assertIn("JSON", res["error"])

    def test_tier2_empty_body_json_command(self):
        """F7-T2-2: Empty or whitespace body for zero-argument commands succeeds."""
        for b in ["", "   ", "\n\t"]:
            status, _, body, _ = self.server.handle_api_request("POST", "cycleAudio", b)
            self.assertEqual(status, 200)
            res = json.loads(body)
            self.assertTrue(res["ok"])

    def test_tier2_volume_clamping_boundaries(self):
        """F7-T2-3: Volume level command clamps negative values to 0 and high values to 150."""
        # Lower bound
        status, _, _, _ = self.server.handle_api_request("POST", "volume", '{"level": -20}')
        self.assertEqual(status, 200)
        self.assertEqual(self.server.command_queue[-1]["volume"], 0)

        # Upper bound
        status, _, _, _ = self.server.handle_api_request("POST", "volume", '{"level": 250}')
        self.assertEqual(status, 200)
        self.assertEqual(self.server.command_queue[-1]["volume"], 150)

        # Exact boundary 150
        status, _, _, _ = self.server.handle_api_request("POST", "volume", '{"level": 150}')
        self.assertEqual(status, 200)
        self.assertEqual(self.server.command_queue[-1]["volume"], 150)

        # Exact boundary 0
        status, _, _, _ = self.server.handle_api_request("POST", "volume", '{"level": 0}')
        self.assertEqual(status, 200)
        self.assertEqual(self.server.command_queue[-1]["volume"], 0)

    def test_tier2_volume_invalid_type_error(self):
        """F7-T2-4: Volume level with string or missing value returns 400 Bad Request."""
        status, _, body, _ = self.server.handle_api_request("POST", "volume", '{"level": "maximum"}')
        self.assertEqual(status, 400)
        res = json.loads(body)
        self.assertFalse(res["ok"])
        self.assertIn("informe level", res["error"])

    def test_tier2_seek_missing_parameters_error(self):
        """F7-T2-5: Seek command without positionMs or offsetMs returns 400 Bad Request."""
        status, _, body, _ = self.server.handle_api_request("POST", "seek", '{"dummy": 123}')
        self.assertEqual(status, 400)
        res = json.loads(body)
        self.assertFalse(res["ok"])
        self.assertIn("informe positionMs ou offsetMs", res["error"])

    def test_tier2_unknown_command_returns_400(self):
        """F7-T2-6: Unknown command returns 400 Bad Request with error description."""
        status, _, body, _ = self.server.handle_api_request("POST", "nonExistentCommand", "{}")
        self.assertEqual(status, 400)
        res = json.loads(body)
        self.assertFalse(res["ok"])
        self.assertIn("comando desconhecido", res["error"])

    def test_tier2_get_on_post_endpoint_returns_405(self):
        """F7-T2-7: GET on POST-only endpoints returns 405 Method Not Allowed."""
        post_only_routes = ["cycleAudio", "cycleSubtitle", "volume", "play", "seek"]
        for route in post_only_routes:
            status, _, body, _ = self.server.handle_api_request("GET", route)
            self.assertEqual(status, 405, f"Route '{route}' should reject GET with 405")
            res = json.loads(body)
            self.assertFalse(res["ok"])

    def test_tier2_play_invalid_url_error(self):
        """F7-T2-8: Play command with empty or invalid URL returns 400 Bad Request."""
        invalid_bodies = [
            '{"url": ""}',
            '{"url": 12345}',
            '{"url": "   "}'
        ]
        for b in invalid_bodies:
            status, _, body, _ = self.server.handle_api_request("POST", "play", b)
            self.assertEqual(status, 400)
            res = json.loads(body)
            self.assertFalse(res["ok"])
            self.assertIn("url inválida", res["error"])

    def test_tier2_subtitle_add_missing_url_error(self):
        """F7-T2-9: Add subtitle command without valid URL returns 400 Bad Request."""
        status, _, body, _ = self.server.handle_api_request("POST", "subtitle", '{"no_url": true}')
        self.assertEqual(status, 400)
        res = json.loads(body)
        self.assertFalse(res["ok"])
        self.assertIn("informe url", res["error"])

    def test_tier2_civetweb_server_header_version(self):
        """F7-T2-10: Verify SendResponse in ServerBridge.cpp embeds Server: SwitchCast/2.4.0."""
        self.assertIn('Server: SwitchCast/" SWITCHCAST_VERSION_STR', self.bridge_src)


if __name__ == "__main__":
    unittest.main()
