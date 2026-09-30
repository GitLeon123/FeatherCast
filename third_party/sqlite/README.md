# SQLite

Vendored SQLite amalgamation, version 3.53.2 (public domain).

Local patch: `winShmMap` casts `nRegion` to `i64` before multiplying by
`szRegion`, avoiding an intermediate signed-int overflow reported by CodeQL
(`cpp/integer-multiplication-cast-to-long`). Preserve this patch or verify that
an upstream replacement includes the equivalent fix when updating SQLite.
