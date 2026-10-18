"""CLI for managing the board.

  refs.py search QUERY [--limit N]   search Wikimedia Commons, print image and page URLs
  refs.py add <biome> <url or local path> --caption TEXT [--source PAGE_URL]
  refs.py comments [--all]     print unread (or all) comments, grouped by target
  refs.py mark-read            mark every comment as read
  refs.py ratings              list images rated cool / ignore, grouped by biome
"""

import argparse
import json
import mimetypes
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

from server import IMAGE_DIRS, lock, load_data, save_data

IMAGE_DIR = IMAGE_DIRS[0]
USER_AGENT = "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124 Safari/537.36"


def search(args):
    params = urllib.parse.urlencode({
        "action": "query",
        "generator": "search",
        "gsrnamespace": 6,
        "gsrsearch": args.query,
        "gsrlimit": args.limit,
        "prop": "imageinfo",
        "iiprop": "url",
        "iiurlwidth": 1280,
        "format": "json",
    })
    request = urllib.request.Request("https://commons.wikimedia.org/w/api.php?" + params,
                                     headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=30) as response:
        pages = json.load(response).get("query", {}).get("pages", {})
    for page in sorted(pages.values(), key=lambda p: p["index"]):
        info = page["imageinfo"][0]
        print(page["title"])
        print("  image:", info.get("thumburl", info["url"]).split("?")[0])
        print("  page: ", info["descriptionurl"])


def fetch(url_or_path):
    local = Path(url_or_path)
    if local.is_file():
        return mimetypes.guess_type(local.name)[0] or "", local.read_bytes()
    request = urllib.request.Request(url_or_path, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.headers.get_content_type(), response.read()


def download(url, stem=None):
    """Saves the image under IMAGE_DIR as <stem><ext> (a fresh id by default); returns the file name."""
    content_type, payload = fetch(url)
    if not content_type.startswith("image/"):
        raise SystemExit(f"not an image ({content_type}): {url}")
    extension = mimetypes.guess_extension(content_type) or ".jpg"
    file_name = (stem or uuid.uuid4().hex[:12]) + extension
    IMAGE_DIR.mkdir(parents=True, exist_ok=True)
    (IMAGE_DIR / file_name).write_bytes(payload)
    return file_name


def add(args):
    file_name = download(args.url)
    image = {
        "id": file_name.split(".")[0],
        "biome": args.biome,
        "file": "images/" + file_name,
        "caption": args.caption,
        "source": args.source or args.url,
    }
    # Web images are not committed; fetch_images.py re-downloads them from this URL.
    if args.url.startswith("http"):
        image["url"] = args.url
    with lock():
        data = load_data()
        if args.biome not in {b["id"] for b in data["biomes"]}:
            raise SystemExit(f"unknown biome: {args.biome}")
        data["images"].append(image)
        save_data(data)
    print(image["id"])


def target_label(data, target):
    if target.startswith("biome:"):
        biome_id = target.split(":", 1)[1]
        biome = next((b for b in data["biomes"] if b["id"] == biome_id), None)
        return f"[biome] {biome['name'] if biome else biome_id}"
    image = next((i for i in data["images"] if i["id"] == target), None)
    if image is None:
        return f"[deleted image {target}]"
    return f"[{image['biome']}] {image['caption']} ({image['id']})"


def comments(args):
    data = load_data()
    selected = [c for c in data["comments"] if args.all or not c["read"]]
    by_target = {}
    for comment in selected:
        by_target.setdefault(comment["target"], []).append(comment)
    for target, group in by_target.items():
        print(target_label(data, target))
        for comment in group:
            print(f"  {comment['ts']}  {comment['text']}")
    if not selected:
        print("no comments")


def ratings(args):
    data = load_data()
    rated = [i for i in data["images"] if i.get("rating")]
    for biome in data["biomes"]:
        group = [i for i in rated if i["biome"] == biome["id"]]
        if not group:
            continue
        print(biome["name"])
        for rating in ("cool", "ignore"):
            for image in (i for i in group if i["rating"] == rating):
                print(f"  {rating:6}  {image['caption']} ({image['id']})")
    if not rated:
        print("no ratings")


def mark_read(args):
    with lock():
        data = load_data()
        for comment in data["comments"]:
            comment["read"] = True
        save_data(data)


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    search_parser = sub.add_parser("search")
    search_parser.add_argument("query")
    search_parser.add_argument("--limit", type=int, default=10)
    search_parser.set_defaults(func=search)

    add_parser = sub.add_parser("add")
    add_parser.add_argument("biome")
    add_parser.add_argument("url")
    add_parser.add_argument("--caption", required=True)
    add_parser.add_argument("--source")
    add_parser.set_defaults(func=add)

    comments_parser = sub.add_parser("comments")
    comments_parser.add_argument("--all", action="store_true")
    comments_parser.set_defaults(func=comments)

    sub.add_parser("mark-read").set_defaults(func=mark_read)
    sub.add_parser("ratings").set_defaults(func=ratings)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
