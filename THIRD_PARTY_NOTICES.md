# Third-party notices

This document is an index. The authoritative licence text and copyright notices
remain in [COPYING](COPYING) and the relevant source files.

## kmscon

Faceplate is derived from kmscon and preserves its complete Git history.

- Project: <https://github.com/kmscon/kmscon>
- Baseline: `791e659f2420e00cda7c39b4eb53f3c2844b4885`
- Primary licence: MIT, subject to per-file notices

The kmscon authors listed in `COPYING` retain copyright in their contributions.
Their names may be used for attribution, not to imply endorsement.

## ccan hash table

The hash-table implementation under `src/shl/htable/` is derived from ccan and
is licensed under LGPL-2.1-or-later, as documented in `COPYING` and the source
files.

## GNU Unifont

When the Unifont backend is enabled, the bundled font is covered by the SIL
Open Font License 1.1. The copyright holders and full licence are recorded in
`COPYING`.

## Dependencies

Faceplate also uses separately distributed dependencies such as libtsm,
libudev, libxkbcommon, libdrm, Mesa/GBM/EGL/OpenGL ES, FreeType, Fontconfig,
Pango, libseat, and ncurses according to selected build options. Their licences
must be captured in the SBOM and packaging metadata for each release image.

## Release requirement

Release CI must produce a machine-readable dependency and licence inventory.
Human review remains mandatory for fonts, logos, themes, and other visual
assets because automated licence scanners are not sufficient for trademark and
asset-redistribution decisions.
