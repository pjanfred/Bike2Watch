#ifndef VERSION_H
#define VERSION_H

// Firmware version shown on the web UI's Firmware Update page. Bump this
// alongside the git tag when cutting a release (see "Releasing new
// firmware versions" in README.md) — it's not derived from the tag
// automatically, since the tag only exists on the build machine's git
// checkout, not inside the compiled binary.
#define FIRMWARE_VERSION "0.2.1"

#endif // VERSION_H
