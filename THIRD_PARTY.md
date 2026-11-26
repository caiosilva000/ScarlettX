# Third-party research components

## Victory shader translator

`third_party/victory` is a pinned Git submodule at
`63bcca43c6a00a758510398fa43096ce7e4f7df0`, sourced from
https://github.com/caiosilva000/VIctory.

Its `victory-shader` crate and the `scarlettx-victory-bridge` C ABI around it
are GPL-3.0-or-later. They are included for the authorized research branch.
Any distribution of this branch or a binary linked against the bridge must
comply with GPL-3.0-or-later; this file is not legal advice.

The bridge never stores title inputs. Its synthetic test creates its own DXBC
envelope and does not include assets, shaders, dumps, or other title-derived
data.

The bridge's synthetic compute fixture (`sx_victory_synthetic_compute`) is a
hand-assembled GFX10 program built from instruction encoders inside the bridge
crate. Its encoders and program shape follow `victory-shader`'s own synthetic
VALU tests (`crates/victory-shader/src/emit/valu_tests.rs`), which are
GPL-3.0-or-later; it contains nothing taken from a title.
