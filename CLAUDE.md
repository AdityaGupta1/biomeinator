## Knowledgebase

A project knowledgebase lives in `knowledge/`. **Before starting any new task,
always read `knowledge/index.md`** and open any entries that are relevant to
the task at hand.

Every knowledgebase file (including all `index.md` files) must have a
`_Last edited: YYYY-MM-DD_` line at the top. Update it whenever you edit the
file.

Entries may link to other entries using relative markdown links, e.g.
`[shaders → common_structs.md](../shaders/common_structs.md)`.

When writing an entry, focus on **rationale and categorisation** — why things are designed the way they are, what role a file or system plays, and what is non-obvious. Do not restate what is already clear from reading the source. Avoid listing every field or function; instead, explain what makes a group of fields or functions coherent, and only call out individual members when their behavior is surprising or easy to misuse.
