"""Downloads the board's web images into static/images (gitignored); the AMNH photos in
static/photos are committed. Images come from each entry's recorded `url`; Wikimedia Commons
entries without one are resolved from their file page. Existing files are skipped, so rerunning
only fetches what is missing.

  python fetch_images.py
"""

import json
import time
import urllib.error
import urllib.parse
import urllib.request

from refs import IMAGE_DIR, USER_AGENT, download
from server import load_data, lock, save_data


def commons_url(page):
    """A scaled thumbnail URL for a Commons file page. Commons heavily rate-limits scripted
    downloads of originals, and asking for a width at or above the original's returns the
    original, so step down through its standard thumbnail widths until one is scaled."""
    title = "File:" + urllib.parse.unquote(page.split("/wiki/File:")[-1])
    url = None
    for width in (1280, 960, 500, 330):
        query = urllib.parse.urlencode({"action": "query", "titles": title, "prop": "imageinfo",
                                        "iiprop": "url", "iiurlwidth": width, "format": "json"})
        request = urllib.request.Request("https://commons.wikimedia.org/w/api.php?" + query,
                                         headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=30) as response:
            pages = json.load(response)["query"]["pages"].values()
        info = next(p["imageinfo"][0] for p in pages if "imageinfo" in p)
        url = info.get("thumburl", info["url"]).split("?")[0]
        if "/thumb/" in url:
            break
    return url


def main():
    data = load_data()
    changed = {}
    failed = []
    for image in data["images"]:
        if not image["file"].startswith("images/"):
            continue
        stem = image["id"]
        if any(IMAGE_DIR.glob(stem + ".*")):
            continue
        url = image.get("url")
        for attempt in range(3):
            try:
                if url is None and "commons.wikimedia.org/wiki/File:" in image["source"]:
                    url = commons_url(image["source"])
                if url is None:
                    raise ValueError("no download URL recorded")
                file_name = download(url, stem)
                changed[image["id"]] = "images/" + file_name
                print(f"ok     {image['id']}  {image['caption'][:70]}")
                break
            except (urllib.error.URLError, OSError, SystemExit, ValueError, StopIteration) as error:
                # Commons rate-limits bursts (HTTP 429); back off and retry.
                if attempt == 2 or isinstance(error, ValueError):
                    failed.append((image, error))
                    break
                time.sleep(20 * (attempt + 1))
    if changed:
        # The extension may differ from the one recorded when the image was first added.
        with lock():
            data = load_data()
            for image in data["images"]:
                if image["id"] in changed:
                    image["file"] = changed[image["id"]]
            save_data(data)
    for image, error in failed:
        print(f"FAILED {image['id']}  {error}  (source: {image['source']})")
    print(f"{len(changed)} downloaded, {len(failed)} failed")


if __name__ == "__main__":
    main()
