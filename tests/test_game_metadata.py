"""Provider, identity and offline-cache contracts; never calls the network."""
import configparser
import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("metadata", Path(__file__).resolve().parents[1] / "scripts/game_metadata.py")
metadata = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metadata)


class MetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.root / "cache"), "XDG_CONFIG_HOME": str(self.root / "config"), "STEAMGRIDDB_API_KEY": ""})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.addCleanup(self.temp.cleanup)

    def config(self, values):
        path = self.root / "config/epinephrine/library.ini"
        path.parent.mkdir(parents=True, exist_ok=True)
        config = configparser.ConfigParser()
        config.read_dict(values)
        with path.open("w") as stream:
            config.write(stream)

    def test_steam_identity(self):
        for command in ("steam steam://rungameid/570", "flatpak run com.valvesoftware.Steam steam://launch/570/dialog", "steam -applaunch 570", "steam steam://run/570"):
            self.assertEqual(metadata.steam_id(command), "570")
        self.assertEqual(metadata.steam_id("heroic launch 570"), "")
        self.assertEqual(metadata.steam_id("xdg-open 'hydralauncher://run?shop=steam&objectId=1091500'"), "1091500")
        self.assertEqual(metadata.steam_id("hydralauncher://run?shop=launchbox&objectId=570"), "")

    def test_online_then_cached_without_network(self):
        def provider(url, headers=None, image=False):
            return b"test-image" if image else {"tier": "gold", "total": 359}
        with patch.object(metadata, "fetch", side_effect=provider) as fetch:
            result = metadata.metadata("Dota 2", "steam steam://rungameid/570")
            self.assertEqual(result["tier"], "gold")
            self.assertEqual(result["reports"], "359")
            self.assertTrue(Path(result["hero"]).is_file())
            fetch.reset_mock()
            self.assertEqual(metadata.metadata("Dota 2", "steam steam://rungameid/570"), result)
            fetch.assert_not_called()

    def test_offline_and_negative_cache(self):
        with patch.object(metadata, "fetch", side_effect=OSError("offline")) as fetch:
            result = metadata.metadata("Dota 2", "steam steam://rungameid/570")
            self.assertEqual(result["status"], "unavailable")
            self.assertEqual(result["tier"], "")
            fetch.reset_mock()
            metadata.metadata("Dota 2", "steam steam://rungameid/570")
            fetch.assert_not_called()

    def test_expired_cache_survives_outage(self):
        with patch.object(metadata, "fetch", side_effect=lambda url, headers=None, image=False: b"image" if image else {"tier": "platinum", "total": 42}):
            original = metadata.metadata("Game", "steam steam://rungameid/570")
        for path in (self.root / "cache").rglob("*.json"):
            os.utime(path, (1, 1))
        with patch.object(metadata, "fetch", side_effect=OSError("offline")):
            result = metadata.metadata("Game", "steam steam://rungameid/570")
        self.assertEqual(result["status"], "cached")
        self.assertEqual(result["tier"], "platinum")
        self.assertEqual(result["hero"], original["hero"])

    def test_non_steam_game_can_be_linked(self):
        command = "heroic launch example"
        identity = hashlib.sha256(command.encode()).hexdigest()
        self.config({identity: {"appid": "570"}})
        with patch.object(metadata, "fetch", side_effect=OSError("offline")):
            self.assertEqual(metadata.metadata("Example", command)["appid"], "570")

    def test_no_guessed_proton_rating_for_unlinked_game(self):
        with patch.object(metadata, "fetch") as fetch:
            result = metadata.metadata("Unlinked game", "heroic launch example")
        fetch.assert_not_called()
        self.assertEqual(result["tier"], "")

    def test_sgdb_exact_match_and_author_credit(self):
        self.config({"services": {"steamgriddb_key": "test-key"}})
        def provider(url, headers=None, image=False):
            if image:
                self.assertIsNone(headers)  # Never send bearer credentials to image hosts.
                return b"test-image"
            self.assertEqual(headers["Authorization"], "Bearer test-key")
            if "/search/" in url:
                return {"data": [{"id": 12, "name": "Example™"}]}
            self.assertIn("/heroes/game/12", url)
            return {"data": [{"id": 99, "url": "https://cdn.steamgriddb.com/test.jpg", "author": {"name": "Artist"}}]}
        with patch.object(metadata, "fetch", side_effect=provider):
            result = metadata.metadata("Example", "heroic launch example")
        self.assertEqual(result["credit"], "SteamGridDB · Artist")
        self.assertEqual(result["art_url"], "https://www.steamgriddb.com/hero/99")

    def test_ambiguous_artwork_is_not_assigned(self):
        self.config({"services": {"steamgriddb_key": "test-key"}})
        with patch.object(metadata, "fetch", return_value={"data": [{"id": 1, "name": "Game"}, {"id": 2, "name": "Game"}]}) as fetch:
            result = metadata.metadata("Game", "heroic launch example")
        self.assertEqual(fetch.call_count, 1)
        self.assertEqual(result["hero"], "")

    def test_bad_key_keeps_steam_fallback(self):
        self.config({"services": {"steamgriddb_key": "invalid"}})
        def provider(url, headers=None, image=False):
            if "steamgriddb.com" in url:
                raise OSError("unauthorized")
            return b"test-image" if image else {"tier": "gold", "total": 10}
        with patch.object(metadata, "fetch", side_effect=provider):
            result = metadata.metadata("Game", "steam steam://rungameid/570")
        self.assertTrue(result["hero"])
        self.assertEqual(result["art_error"], "true")

    def test_response_size_and_image_type_are_bounded(self):
        with self.assertRaises(ValueError):
            metadata.fetch("file:///etc/passwd")

    def test_serialization_escapes_multiline_provider_text(self):
        serialized = metadata.serialize({"credit": "Artist\n[other]\\name"})
        self.assertIn(r"Artist\n[other]\\name", serialized)
        self.assertEqual(serialized.count("\n["), 0)

    def test_custom_artwork_wins_offline_and_preserves_credit(self):
        path = self.root / "custom.png"
        path.write_bytes(b"custom-image")
        command = "heroic launch example"
        identity = hashlib.sha256(command.encode()).hexdigest()
        self.config({identity: {"cover_path": str(path), "cover_credit": "Artist", "cover_source": "https://example.com/art"}})
        with patch.object(metadata, "fetch") as fetch:
            result = metadata.artwork_asset("Example", command, "cover")
        fetch.assert_not_called()
        self.assertEqual(result["path"], str(path))
        self.assertEqual(result["credit"], "Artist")

    def test_non_steam_cover_is_found_by_exact_store_title(self):
        def provider(url, headers=None, image=False):
            if image:
                return b"image"
            self.assertIn("storesearch", url)
            return {"items": [{"id": 367520, "name": "Hollow Knight"}, {"id": 1030300, "name": "Hollow Knight: Silksong"}]}
        with patch.object(metadata, "fetch", side_effect=provider):
            result = metadata.artwork_asset("Hollow Knight", "heroic launch hollow", "cover")
        self.assertEqual(result["credit"], "Steam")
        self.assertIn("367520", result["source"])
        self.assertTrue(Path(result["path"]).is_file())

    def test_similar_title_is_never_automatically_assigned(self):
        with patch.object(metadata, "fetch", return_value={"items": [{"id": 1030300, "name": "Hollow Knight: Silksong"}]}) as fetch:
            result = metadata.artwork_asset("Hollow Knight", "heroic launch hollow", "cover")
        self.assertEqual(result["path"], "")
        self.assertEqual(fetch.call_count, 1)

    def test_selected_sgdb_identity_overrides_title_search(self):
        command = "heroic launch custom"
        identity = hashlib.sha256(command.encode()).hexdigest()
        self.config({"services": {"steamgriddb_key": "test"}, identity: {"sgdb_id": "120"}})
        def provider(url, headers=None, image=False):
            self.assertIn("/grids/game/120", url)
            return {"data": [{"id": 3, "url": "https://cdn.steamgriddb.com/cover.jpg", "author": {"name": "Author"}}]}
        with patch.object(metadata, "fetch", side_effect=provider) as fetch:
            results = metadata.gallery("Wrong imported title", command, "cover")
        self.assertEqual(fetch.call_count, 1)
        self.assertEqual(results[0]["credit"], "SteamGridDB · Author")

    def test_failed_images_have_a_short_negative_cache(self):
        with patch.object(metadata, "fetch", side_effect=OSError("404")) as fetch:
            for _ in range(2):
                with self.assertRaises(OSError):
                    metadata.download_image("https://example.com/missing.jpg")
        self.assertEqual(fetch.call_count, 1)

    def test_banner_override_bypasses_fresh_metadata_cache(self):
        command = "steam steam://rungameid/570"
        identity = hashlib.sha256(command.encode()).hexdigest()
        custom = self.root / "hero.png"
        custom.write_bytes(b"custom")
        self.config({identity: {"hero_path": str(custom), "hero_credit": "Custom"}})
        with patch.object(metadata, "metadata", return_value={"hero": "/old/cached.jpg", "tier": "gold"}):
            output = metadata.main(["Game", command])
        self.assertIn(str(custom), output)
        self.assertNotIn("/old/cached.jpg", output)

    def test_gallery_kind_is_validated(self):
        with self.assertRaises(ValueError):
            metadata.gallery("Game", "launch", "../invalid")

    def test_stable_identity_survives_changed_launch_command(self):
        old, new = "old command", "new command"
        stable = "d0a11111-1111-4111-8111-111111111111"
        image = self.root / "chosen.png"
        image.write_bytes(b"chosen")
        self.config({"identities": {hashlib.sha256(old.encode()).hexdigest(): stable, hashlib.sha256(new.encode()).hexdigest(): stable}, stable: {"cover_path": str(image), "cover_credit": "Artist"}})
        with patch.object(metadata, "fetch") as fetch:
            self.assertEqual(metadata.artwork_asset("Old name", old, "cover"), metadata.artwork_asset("New name", new, "cover"))
            fetch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
