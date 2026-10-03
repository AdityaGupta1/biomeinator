"""Reference image board: serves the page, images, and a small JSON API for comments.

Run with `python server.py` and open http://localhost:8502."""

import contextlib
import json
import os
import threading
import time
import uuid
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DATA_PATH = ROOT / "data.json"
PORT = 8502

LOCK_PATH = ROOT / "data.lock"
# Downloaded web images (gitignored, filled by fetch_images.py) and the committed AMNH photos.
IMAGE_DIRS = (ROOT / "static" / "images", ROOT / "static" / "photos")
THUMB_DIR = ROOT / "static" / "thumbs"
# Longest edge in pixels. "tiny" is a blurred placeholder shown until the grid thumbnail loads.
THUMB_SIZES = {"tiny": 32, "grid": 640}

RATINGS = (None, "ignore", "cool")

thumb_lock = threading.Lock()


@contextlib.contextmanager
def lock():
    # File lock rather than a threading lock so refs.py (a separate process) is serialized too.
    with LOCK_PATH.open("a+b") as lock_file:
        if os.name == "nt":
            import msvcrt
            lock_file.seek(0)
            # LK_LOCK retries for ~10 s before raising, plenty for a JSON rewrite.
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock_file, fcntl.LOCK_EX)
            yield


def load_data():
    with DATA_PATH.open() as f:
        return json.load(f)


def save_data(data):
    tmp = DATA_PATH.with_suffix(".tmp")
    with tmp.open("w") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, DATA_PATH)


def ensure_thumbnail(size_name, file_name):
    """Returns the cached thumbnail path, generating it on first request. None if the source is missing."""
    source = next((d / file_name for d in IMAGE_DIRS if (d / file_name).is_file()), None)
    if source is None:
        return None
    target = THUMB_DIR / size_name / (source.stem + ".jpg")
    with thumb_lock:
        if not target.is_file():
            # Imported lazily so refs.py, which imports this module, runs without Pillow.
            from PIL import Image, ImageOps
            with Image.open(source) as image:
                image = ImageOps.exif_transpose(image).convert("RGB")
                edge = THUMB_SIZES[size_name]
                image.thumbnail((edge, edge))
                target.parent.mkdir(parents=True, exist_ok=True)
                image.save(target, "JPEG", quality=75, progressive=True)
    return target


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT / "static"), **kwargs)

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        length = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(length) or b"{}")

    def do_GET(self):
        if self.path == "/api/data":
            with lock():
                self.send_json(load_data())
            return
        parts = self.path.split("/")
        if len(parts) == 4 and parts[1] == "thumbs" and parts[2] in THUMB_SIZES:
            target = ensure_thumbnail(parts[2], Path(parts[3]).name)
            if target is None:
                self.send_error(404)
                return
            body = target.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "max-age=86400")
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        try:
            body = self.read_json()
        except json.JSONDecodeError:
            self.send_json({"error": "bad json"}, 400)
            return

        if self.path == "/api/comments":
            text = str(body.get("text", "")).strip()
            target = str(body.get("target", ""))
            if not text or not target:
                self.send_json({"error": "text and target required"}, 400)
                return
            comment = {
                "id": uuid.uuid4().hex[:10],
                "target": target,
                "text": text,
                "ts": time.strftime("%Y-%m-%d %H:%M"),
                "read": False,
            }
            with lock():
                data = load_data()
                data["comments"].append(comment)
                save_data(data)
            self.send_json(comment)
            return

        if self.path == "/api/comments/delete":
            comment_id = str(body.get("id", ""))
            with lock():
                data = load_data()
                data["comments"] = [c for c in data["comments"] if c["id"] != comment_id]
                save_data(data)
            self.send_json({"ok": True})
            return

        if self.path == "/api/rating":
            image_id = str(body.get("id", ""))
            rating = body.get("rating")
            if rating not in RATINGS:
                self.send_json({"error": "bad rating"}, 400)
                return
            with lock():
                data = load_data()
                for image in data["images"]:
                    if image["id"] == image_id:
                        if rating is None:
                            image.pop("rating", None)
                        else:
                            image["rating"] = rating
                save_data(data)
            self.send_json({"ok": True})
            return

        self.send_json({"error": "not found"}, 404)

    def log_message(self, format, *args):
        pass


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
