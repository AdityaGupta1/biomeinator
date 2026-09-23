## Knowledgebase

A project knowledgebase lives in `knowledge/`. **Before starting any new task,
always read `knowledge/index.md`** and open any entries that are relevant to
the task at hand.

Every knowledgebase file (including all `index.md` files) must have a
`_Last edited: YYYY-MM-DD_` line at the top. Update it whenever you edit the
file.

Entries may link to other entries using relative markdown links, e.g.
`[shaders → common_structs.md](../shaders/common_structs.md)`.

When writing an entry, focus on **rationale and categorisation** — why things
are designed the way they are, what role a file or system plays, and what is
non-obvious. Do not restate what is already clear from reading the source.

**Do not:**
- List enum variants, struct fields, or function signatures that are plain data
  readable from the header (e.g. listing all block types or all biomes).
- Describe what a function does when the name already says it.
- Write tables that mirror data definitions in the code.

**Do:**
- Explain non-obvious design decisions (why two instances per chunk, why
  regions are never destroyed, why the surface multiplier is asymmetric).
- Call out gotchas, invariants, and ordering dependencies that would surprise a
  reader or cause bugs if violated.
- Describe the *role* of a group of things, not each member individually.
