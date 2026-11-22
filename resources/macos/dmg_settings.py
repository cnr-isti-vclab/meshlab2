# dmgbuild settings for MeshLab's disk image, used by scripts/package-macos-dmg.sh,
# which passes the bundle, its icon and the background with -D.
#
# dmgbuild writes the Finder layout (.DS_Store) itself rather than scripting Finder,
# so it works the same on a headless CI runner. The background is the classic
# MeshLab viewer gradient, black to (128,128,255), with a white slot behind each
# item so Finder's labels stay readable on it; dmgbuild picks up the @2x copy
# beside it for Retina screens.

files = [defines["app"]]
symlinks = {"Applications": "/Applications"}

icon = defines["icon"]
background = defines["background"]

# The background is 600 x 380; the icon centres sit in its two slots.
window_rect = ((200, 120), (600, 380))
icon_size = 128
text_size = 14
icon_locations = {
    "MeshLab.app": (150, 175),
    "Applications": (450, 175),
}
