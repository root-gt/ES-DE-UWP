#!/usr/bin/env python3
"""Minimal mock of the RomM API endpoints used by the ES-DE RomM integration."""
import io
import json
import re
import struct
import sys
import zlib
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote

TOKEN = "testtoken"
LOG = []


def png(color):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(color) * 4 for _ in range(4))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def make_zip(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            z.writestr(name, data)
    return buf.getvalue()


PLATFORMS = [
    {"id": 1, "slug": "gba", "fs_slug": "gba", "name": "Game Boy Advance", "display_name": "Game Boy Advance"},
    {"id": 2, "slug": "snes", "fs_slug": "SNES-roms", "name": "Super Nintendo", "display_name": "Super Nintendo"},
    {"id": 3, "slug": "genesis-slash-megadrive", "fs_slug": "genesis", "name": "Sega Mega Drive/Genesis", "display_name": "Sega Mega Drive/Genesis"},
    {"id": 4, "slug": "unknown-thing", "fs_slug": "unknown-thing", "name": "Unknown Platform", "display_name": "Unknown Platform"},
]

ROMS = {}
CONTENT = {}


def add_rom(rid, platform, fs_name, content, name=None, multi=False, cover=True, summary=None):
    p = next(p for p in PLATFORMS if p["id"] == platform)
    ROMS[rid] = {
        "id": rid,
        "platform_id": platform,
        "platform_slug": p["slug"],
        "platform_fs_slug": p["fs_slug"],
        "fs_name": fs_name,
        "fs_name_no_ext": fs_name.rsplit(".", 1)[0] if "." in fs_name else fs_name,
        "fs_size_bytes": len(content),
        "name": name or fs_name.rsplit(".", 1)[0],
        "summary": summary,
        "has_multiple_files": multi,
        "metadatum": {"first_release_date": 1001894400000, "average_rating": 85.5,
                      "genres": ["Platform", "Action"], "companies": ["Nintendo", "Nintendo EAD"],
                      "player_count": "1-2"},
        "path_cover_large": f"roms/{p['fs_slug']}/{rid}/cover/big.png" if cover else None,
        "merged_screenshots": [f"/assets/romm/resources/roms/{rid}/screenshots/0.png"],
        "path_video": None,
        "path_manual": None,
        "ss_metadata": {"logo_path": f"roms/{rid}/marquee.png"},
        "files": [{"id": rid * 10, "file_name": fs_name, "file_size_bytes": len(content)}],
    }
    CONTENT[rid] = content


# GBA: 260 games to exercise pagination, plus a real-ish one with an & in the name.
add_rom(1, 1, "Kirby & The Amazing Mirror (USA).gba", b"GBAROM" * 200000,
        name="Kirby & The Amazing Mirror", summary="Kirby\r\nis back & pink.")
for i in range(2, 262):
    add_rom(i, 1, f"Homebrew Game {i:03d} (World).gba", f"rom {i}".encode() * 100, cover=(i % 50 == 0))

# SNES: archive that must be extracted to .sfc (RomMExtractedExtensions=snes=sfc).
sfc = bytes(range(256)) * 4096
add_rom(500, 2, "Super Metroid (USA).zip", make_zip([("readme.txt", b"hi"), ("Super Metroid (USA).sfc", sfc)]))
# SNES: plain .sfc file must not be treated as an archive.
add_rom(501, 2, "Plain Game (USA).sfc", b"PLAIN" * 1000)

# Genesis: multi-file ROM served as a zip.
add_rom(600, 3, "Multi Disc Game", make_zip([("disc1.bin", b"1" * 1000), ("disc2.bin", b"2" * 1000)]), multi=True)

# Big game served slowly to test the progress dialog and cancelling.
add_rom(800, 1, "Zzz Big Game (USA).gba", b"B" * (40 * 1024 * 1024))

# Unknown platform.
add_rom(700, 4, "Mystery.bin", b"???")

COVER = png((200, 50, 50))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("MOCK " + fmt % args + "\n")

    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        path = unquote(url.path)
        q = parse_qs(url.query)
        LOG.append(self.path)

        if path == "/api/heartbeat":
            return self.send(200, {"SYSTEM": {"VERSION": "4.4.0-mock"}})

        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            return self.send(401, {"detail": "Not authenticated"})

        if path == "/api/platforms":
            out = []
            for p in PLATFORMS:
                p = dict(p)
                p["rom_count"] = sum(1 for r in ROMS.values() if r["platform_id"] == p["id"])
                out.append(p)
            return self.send(200, out)

        if path == "/api/roms":
            pid = int(q["platform_ids"][0])
            limit = int(q.get("limit", ["50"])[0])
            offset = int(q.get("offset", ["0"])[0])
            items = sorted([r for r in ROMS.values() if r["platform_id"] == pid], key=lambda r: r["name"])
            page = items[offset:offset + limit]
            return self.send(200, {"items": page, "total": len(items), "limit": limit, "offset": offset})

        m = re.fullmatch(r"/api/roms/(\d+)", path)
        if m:
            rid = int(m.group(1))
            if rid not in ROMS:
                return self.send(404, {"detail": f"Rom with id {rid} not found"})
            return self.send(200, ROMS[rid])

        m = re.fullmatch(r"/api/roms/(\d+)/content/(.+)", path)
        if m:
            rid = int(m.group(1))
            if rid not in ROMS or ROMS[rid]["fs_name"] != m.group(2):
                return self.send(404, {"detail": "not found"})
            if rid == 800:
                import time
                data = CONTENT[rid]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    for i in range(0, len(data), 256 * 1024):
                        self.wfile.write(data[i:i + 256 * 1024])
                        time.sleep(0.1)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            return self.send(200, CONTENT[rid], "application/octet-stream")

        if path.startswith("/assets/romm/resources/") and path.endswith(".png"):
            return self.send(200, COVER, "image/png")

        return self.send(404, {"detail": "Not Found"})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
