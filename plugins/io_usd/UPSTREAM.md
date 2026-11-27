# LightUSD

`io_usd` reads and writes USD through LightUSD (lighttransport, Apache-2.0, formerly TinyUSDZ),
in the pinned `external/lightusd` submodule. It uses the library's `next` core
(`lightusd::next`) and its render-scene converter (`tydra_next`); the older core and Tydra,
which LightUSD's README still describes, are deprecated upstream and not built.
`CMakeLists.txt` here turns off LightUSD's tools, tests, examples, C API, bundled
OBJ/FBX/VOX/volume importers, audio and EXR.

Currently pinned at **v1.0.0** (2026-11). Update it with:

```sh
git -C external/lightusd fetch --tags
git -C external/lightusd checkout <reviewed-tag>
```

then rebuild and run the USD tests in `tests/test_document.cpp` (`usd*`, and
`textFormatsIgnoreTheNumericLocale`), and check written files with OpenUSD as
`docs/design/proposals/usd_support.md` §4.4 describes. The writer relies on two LightUSD
details its comments name: shader outputs declared with `add_property_slot`, and
`PropMeta::kInterpolation` for primvar interpolation.
