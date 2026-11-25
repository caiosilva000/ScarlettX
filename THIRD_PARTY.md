# Third-party research components

## Victory shader translator

`third_party/victory-belobug-amd` is a pinned Git submodule at
`01aadd193d57e71fb4d57810eb2fdd1bc95c950b`, sourced from
https://github.com/caiosilva000/victory-belobug-amd.

Its `victory-shader` crate and the `scarlettx-victory-bridge` C ABI around it
are GPL-3.0-or-later. They are included for the authorized research branch.
Any distribution of this branch or a binary linked against the bridge must
comply with GPL-3.0-or-later; this file is not legal advice.

The bridge never stores title inputs. Its synthetic test creates its own DXBC
envelope and does not include assets, shaders, dumps, or other title-derived
data.
