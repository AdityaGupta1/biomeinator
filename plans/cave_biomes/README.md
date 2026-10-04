# Cave Biomes (AMNH)

Working folder for the mineral cave biome effort. Start with [plan.md](plan.md): scope,
per-biome direction, lighting rules, terrain architecture, rendering prerequisites and the
suggested implementation order.

| Folder | What it is |
|---|---|
| [refboard/](refboard/) | Reference image board: 248 images across 8 candidate biomes, with the review comments and cool/ignore ratings |
| [prototype/](prototype/) | Standalone CPU prototype of the terrain ideas plus a small path tracer (built on a GPU-less machine) |
| [prototype/results/](prototype/results/) | Prototype renders the design review was based on |

## Reference board

A local web page for browsing references and leaving comments per image or per biome, and
marking images as cool or ignore.

```
cd plans/cave_biomes/refboard
pip install -r requirements.txt
python fetch_images.py      # one-time: downloads web images into static/images (gitignored)
python server.py            # then open http://localhost:8502
```

- The user's AMNH photos are committed in `static/photos/`. Web images are not (some are
  copyrighted), so `fetch_images.py` re-downloads them from the URL recorded in `data.json`,
  resolving Wikimedia Commons entries from their file page. It skips files already present, so
  rerun it to retry failures. A few sources may have disappeared; their cards show a source
  link but no image.
- Thumbnails are generated on first view and cached in `static/thumbs/` (gitignored).
- `data.json` holds the biomes, captions, sources, comments and ratings, and is committed so
  review history travels with the branch.

CLI (`python refs.py <command>`):
- `comments [--all]` — unread (or all) comments grouped by image/biome; `mark-read` afterwards.
- `ratings` — images marked cool/ignore, per biome.
- `search QUERY` — search Wikimedia Commons; `add <biome> <url or path> --caption ... --source ...`
  downloads an image and records its URL for `fetch_images.py`.

## Prototype

Not engine code: a Python stand-in to judge shapes, palettes and lighting without a GPU. Uses
the engine's block textures from `assets/blocks/textures` plus procedural textures for new blocks.

```
cd plans/cave_biomes/prototype
pip install -r requirements.txt
python run.py gen marble           # generate + cross-sections into out/marble/
python run.py views marble         # search camera views, render a preview contact sheet
python run.py shot marble 3 --spp 256
```

Scenes: `marble`, `pegmatite`, `opal`, `opal_lamps`, `copper`, `mixed` (see `SCENES` in
`run.py`). Renders are noisy (no denoiser); on a GPU machine the real test is the engine.
The renderer mirrors the engine where it matters for the design: only exposed emitter faces
are light-sampled, glass occludes shadow rays, and emission masks are BSDF-hit only.
