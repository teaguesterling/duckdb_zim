# The `zim://` filesystem

The extension registers a read-only `zim://` filesystem, so **any** path-reading function
— DuckDB's own `read_text` / `read_blob`, or [`webbed`](https://github.com/teaguesterling/duckdb_webbed)'s
`read_html`, or anything else that goes through DuckDB's file layer — can address an entry
inside a ZIM, with no coupling and no GPL linkage back into those extensions.

```sql
SELECT * FROM read_text('zim://wikipedia.zim/A/Photosynthesis');   -- one entry as text
SELECT * FROM read_blob('zim://wikipedia.zim/I/logo.webp');        -- one entry as bytes
SELECT * FROM read_html('zim://wikipedia.zim/A/Photosynthesis');   -- requires: LOAD webbed;
SELECT * FROM read_text('zim://wikipedia.zim/A/C*');               -- glob over content paths
```

## Grammar

```
zim://<archive>.zim/<content-path>
```

Content-path-first. The archive component is a **local file** (libzim mmaps it directly,
so `zim://` can't currently nest another VFS such as S3 for the archive itself); everything
after the `.zim/` boundary is the entry path within the archive. Split archives
(`.zim[a-z][a-z]`) are recognized.

- A leading `C/` on the content path is tolerated and stripped.
- **Redirects are followed** — a redirect path serves its target's bytes, like a symlink.
- `*` / `?` / `[…]` globs are matched against entry paths.
- Full-URL **zimit / browsertrix** paths work, including trailing-slash directory pages
  (`zim://archive.zim/www.nhs.uk/medicines/insulin/`).

!!! note "Memory"
    A ZIM item is a decompressed blob, not a seekable byte range, so an entry is
    materialized into memory on open. Fine for articles; a single very large media item
    costs its size in RAM.

## Composition example

Glob the archive and pipe every article into `webbed`:

```sql
-- requires: LOAD webbed;
SELECT regexp_replace(filename, '^.*/', '') AS entry,
       html_extract_text(content::HTML, '//h1')[1] AS heading
FROM read_text('zim://wikipedia.zim/A/*')
ORDER BY entry;
```

This is the highest-leverage part of the extension: `zim` ships **zero** HTML knowledge,
yet an entire offline Wikipedia becomes XPath-queryable by composing the GPL container
reader with the MIT `webbed` extension over the `zim://` seam.
