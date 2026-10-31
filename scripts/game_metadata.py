#!/usr/bin/env python3
"""Selected-game enrichment. Stdlib only; stdout is a GLib key file.

No launcher commands are executed. SteamDB and HLTB are browser links only.
The ProtonDB summary endpoint is public but unofficial: failure is expected.
"""
import configparser
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time
import urllib.parse
import urllib.request

TTL = 24 * 60 * 60
MAX_BYTES = 12 * 1024 * 1024


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix=".metadata-")
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def fetch(url, headers=None, image=False):
    if urllib.parse.urlparse(url).scheme != "https":
        raise ValueError("HTTPS required")
    request = urllib.request.Request(url, headers={"User-Agent": "Epinephrine/0.1", **(headers or {})})
    with urllib.request.urlopen(request, timeout=6) as response:
        if image and not response.headers.get_content_type().startswith("image/"):
            raise ValueError("Not an image")
        data = response.read(MAX_BYTES + 1)
        if len(data) > MAX_BYTES:
            raise ValueError("Response too large")
        return data if image else json.loads(data)


def steam_id(command):
    match = re.search(r"steam://(?:rungameid|run|launch)/(\d+)", command)
    if not match:
        match = re.search(r"(?:^|\s)-applaunch\s+(\d+)(?:\s|$)", command)
    if match:
        return match.group(1)
    # Hydra retains the catalog identity even though it launches the local copy.
    link = re.search(r"hydralauncher://run\?[^\s\"']+", command)
    if link:
        query = urllib.parse.parse_qs(urllib.parse.urlparse(link.group(0)).query)
        appid = query.get("objectId", [""])[0]
        if query.get("shop") == ["steam"] and re.fullmatch(r"[0-9]{1,10}", appid):
            return appid
    return ""


def normalized(name):
    return re.sub(r"[^\w]", "", name.casefold().replace("™", "").replace("®", ""))


def metadata(name, command):
    config_dir = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "epinephrine"
    cache_dir = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "epinephrine" / "library"
    config = configparser.ConfigParser(interpolation=None)
    try:
        config.read(config_dir / "library.ini")
    except (OSError, configparser.Error):
        pass
    identity = game_identity(config, command)
    appid = config.get(identity, "appid", fallback="") or steam_id(command)
    if not re.fullmatch(r"[0-9]{1,10}", appid):
        appid = ""
    api_key = config.get("services", "steamgriddb_key", fallback="").strip() or os.environ.get("STEAMGRIDDB_API_KEY", "")
    # Key fingerprint invalidates missing-art results after credentials change.
    digest = hashlib.sha256((name + command + appid + api_key).encode()).hexdigest()
    cache_file = cache_dir / (digest + ".json")
    cached = {}
    try:
        cached = json.loads(cache_file.read_text())
        if not isinstance(cached, dict):
            cached = {}
            raise ValueError("Invalid cache")
        if time.time() - cache_file.stat().st_mtime < TTL:
            return cached
    except (OSError, ValueError):
        pass
    result = {"appid": appid, "tier": "", "reports": "", "hero": "", "credit": "", "art_url": "", "status": "unavailable"}
    if appid:
        try:
            summary = fetch(f"https://www.protondb.com/api/v1/reports/summaries/{appid}.json")
            tier = summary.get("tier", "")
            if tier in ("native", "platinum", "gold", "silver", "bronze", "borked", "pending"):
                result.update(tier=tier, reports=str(int(summary.get("total", 0))), status="online")
        except (OSError, ValueError, TypeError, AttributeError):
            # Retain stale successful data when the service is unavailable.
            if cached.get("tier"):
                result.update(tier=cached["tier"], reports=cached.get("reports", ""), status="cached")
    if api_key:
        try:
            base = "https://www.steamgriddb.com/api/v2"
            headers = {"Authorization": "Bearer " + api_key}
            if appid:
                target = "steam/" + appid
            else:
                matches = fetch(base + "/search/autocomplete/" + urllib.parse.quote(name, safe=""), headers).get("data", [])
                exact = [g for g in matches if normalized(g.get("name", "")) == normalized(name)]
                target = "game/" + str(int(exact[0]["id"])) if len(exact) == 1 else ""
            if target:
                arts = fetch(base + "/heroes/" + target + "?types=static&nsfw=false&humor=false", headers).get("data", [])
                if arts:
                    art = arts[0]
                    path = cache_dir / (digest + ".image")
                    atomic_write(path, fetch(art["url"], image=True))
                    result.update(hero=str(path), credit="SteamGridDB · " + art.get("author", {}).get("name", ""), art_url="https://www.steamgriddb.com/hero/" + str(int(art["id"])))
        except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError):
            result["art_error"] = "true"
    if not result["hero"] and cached.get("hero") and Path(cached["hero"]).is_file():
        for field in ("hero", "credit", "art_url"):
            result[field] = cached.get(field, "")
    if not result["hero"] and appid:
        try:
            path = cache_dir / (digest + "-steam.image")
            atomic_write(path, fetch(f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/library_hero.jpg", image=True))
            result.update(hero=str(path), credit="Steam", art_url=f"https://store.steampowered.com/app/{appid}/")
        except (OSError, ValueError):
            pass
    # Short negative cache permits recovery without hammering offline services.
    try:
        atomic_write(cache_file, json.dumps(result).encode())
        if result["status"] != "online" or result.get("art_error") or (appid and not result["hero"]):
            stamp = time.time() - TTL + 300
            os.utime(cache_file, (stamp, stamp))
    except OSError:
        pass
    return result


def serialize(result):
    output = configparser.ConfigParser(interpolation=None)
    output["game"] = {k: str(v).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r") for k, v in result.items()}
    stream = io.StringIO()
    output.write(stream)
    return stream.getvalue()


def game_identity(config, command):
    digest = hashlib.sha256(command.encode()).hexdigest()
    return config.get("identities", digest, fallback=digest)


def configuration():
    config = configparser.ConfigParser(interpolation=None)
    try:
        config.read(Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "epinephrine/library.ini")
    except (OSError, configparser.Error):
        pass
    return config


def cache_root():
    return Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "epinephrine/library"


def cached_json(key, loader):
    path = cache_root() / (hashlib.sha256(key.encode()).hexdigest() + ".art.json")
    previous = None
    try:
        previous = json.loads(path.read_text())
        if time.time() - path.stat().st_mtime < TTL:
            return previous
    except (OSError, ValueError):
        pass
    try:
        result = loader()
        atomic_write(path, json.dumps(result).encode())
        if not result:
            stamp = time.time() - TTL + 300
            os.utime(path, (stamp, stamp))
        return result
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return previous or []


def search_games(query):
    config = configuration()
    key = config.get("services", "steamgriddb_key", fallback="").strip() or os.environ.get("STEAMGRIDDB_API_KEY", "")
    def search():
        results = []
        if key:
            try:
                data = fetch("https://www.steamgriddb.com/api/v2/search/autocomplete/" + urllib.parse.quote(query, safe=""), {"Authorization": "Bearer " + key})
                results.extend({"name": item["name"], "id": str(int(item["id"])), "provider": "SteamGridDB"} for item in data.get("data", [])[:12])
            except (OSError, ValueError, KeyError, TypeError):
                pass
        try:
            data = fetch("https://store.steampowered.com/api/storesearch/?" + urllib.parse.urlencode({"term": query, "l": "english", "cc": "us"}))
            results.extend({"name": item["name"], "id": str(int(item["id"])), "provider": "Steam"} for item in data.get("items", [])[:12])
        except (OSError, ValueError, KeyError, TypeError):
            pass
        return results
    return cached_json("search-v1" + query + key, search)


def gallery(name, command, kind):
    if kind not in ("cover", "hero"):
        raise ValueError("Invalid artwork kind")
    config = configuration()
    identity = game_identity(config, command)
    appid = config.get(identity, "appid", fallback="") or steam_id(command)
    sgdb = config.get(identity, "sgdb_id", fallback="")
    if not re.fullmatch(r"[0-9]{1,10}", appid):
        appid = ""
    if not sgdb.isdigit():
        sgdb = ""
    key = config.get("services", "steamgriddb_key", fallback="").strip() or os.environ.get("STEAMGRIDDB_API_KEY", "")
    def load():
        nonlocal appid, sgdb
        if not appid and not sgdb:
            matches = search_games(name)
            for provider in ("SteamGridDB", "Steam"):
                exact = [m for m in matches if m["provider"] == provider and normalized(m["name"]) == normalized(name)]
                if len(exact) == 1:
                    if provider == "Steam":
                        appid = exact[0]["id"]
                    else:
                        sgdb = exact[0]["id"]
        items = []
        if key and (sgdb or appid):
            try:
                target = "game/" + sgdb if sgdb else "steam/" + appid
                endpoint = "grids" if kind == "cover" else "heroes"
                params = "?types=static&nsfw=false&humor=false"
                if kind == "cover":
                    params += "&dimensions=600x900,342x482,660x930"
                data = fetch("https://www.steamgriddb.com/api/v2/" + endpoint + "/" + target + params, {"Authorization": "Bearer " + key})
                for item in data.get("data", [])[:24]:
                    items.append({"url": item["url"], "thumb": item.get("thumb", item["url"]), "credit": "SteamGridDB · " + item.get("author", {}).get("name", ""), "source": "https://www.steamgriddb.com/" + ("grid/" if kind == "cover" else "hero/") + str(int(item["id"]))})
            except (OSError, ValueError, KeyError, TypeError):
                pass
        if appid:
            filenames = ("library_600x900_2x.jpg", "library_600x900.jpg") if kind == "cover" else ("library_hero.jpg", "page_bg_generated_v6b.jpg")
            for filename in filenames:
                url = f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/{filename}"
                items.append({"url": url, "thumb": url, "credit": "Steam", "source": f"https://store.steampowered.com/app/{appid}/"})
        return items
    return cached_json("gallery-v2" + name + command + appid + sgdb + key + kind, load)


def download_image(url):
    path = cache_root() / (hashlib.sha256(url.encode()).hexdigest() + ".art.image")
    if not path.is_file():
        failed = path.with_suffix(".failed")
        if failed.is_file() and time.time() - failed.stat().st_mtime < 300:
            raise OSError("Image temporarily unavailable")
        try:
            atomic_write(path, fetch(url, image=True))
            failed.unlink(missing_ok=True)
        except (OSError, ValueError):
            atomic_write(failed, b"")
            raise
    return str(path)


def artwork_asset(name, command, kind):
    config = configuration()
    identity = game_identity(config, command)
    custom = config.get(identity, kind + "_path", fallback="")
    if custom and Path(custom).is_file():
        return {"path": custom, "credit": config.get(identity, kind + "_credit", fallback=""), "source": config.get(identity, kind + "_source", fallback="")}
    for item in gallery(name, command, kind):
        try:
            return {"path": download_image(item["url"]), "credit": item["credit"], "source": item["source"]}
        except (OSError, ValueError):
            continue
    return {"path": ""}


def sections(values):
    return "\n".join(serialize(value).replace("[game]", "[" + section + "]", 1) for section, value in values.items())


def prune_cache(max_bytes=128 * 1024 * 1024, max_age=30 * 86400):
    root = cache_root()
    marker = root / ".last-cleanup"
    now = time.time()
    if marker.exists() and now - marker.stat().st_mtime < 86400:
        return
    root.mkdir(parents=True, exist_ok=True)
    files = sorted((p for p in root.iterdir() if p.is_file() and p.name != marker.name), key=lambda p: p.stat().st_mtime, reverse=True)
    used = 0
    for path in files:
        info = path.stat()
        used += info.st_size
        if now - info.st_mtime > 60 and (used > max_bytes or now - info.st_mtime > max_age):
            path.unlink(missing_ok=True)
    marker.touch()


def main(args):
    if args[0] == "--cover":
        return serialize(artwork_asset(args[1], args[2], "cover"))
    if args[0] == "--gallery":
        return sections({"item" + str(i): item for i, item in enumerate(gallery(args[2], args[3], args[1]))})
    if args[0] == "--search":
        return sections({"match" + str(i): item for i, item in enumerate(search_games(args[1]))})
    if args[0] == "--image":
        return serialize({"path": download_image(args[1])})
    result = metadata(args[0], args[1])
    config = configuration()
    identity = game_identity(config, args[1])
    # Explicit choices override metadata caches immediately, including offline.
    if config.get(identity, "hero_path", fallback="") or config.get(identity, "sgdb_id", fallback="") or not result.get("hero"):
        art = artwork_asset(args[0], args[1], "hero")
        if art.get("path"):
            result.update(hero=art["path"], credit=art.get("credit", ""), art_url=art.get("source", ""))
    return serialize(result)


if __name__ == "__main__":
    try:
        try:
            prune_cache()
        except OSError:
            pass
        print(main(sys.argv[1:]))
    except (OSError, ValueError, configparser.Error, KeyError, TypeError, IndexError):
        print(serialize({"status": "unavailable", "path": ""}))
