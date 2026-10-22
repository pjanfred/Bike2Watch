#ifndef VERSION_H
#define VERSION_H

// Firmware version shown on the web UI's Firmware Update page. Bump this
// alongside the git tag when cutting a release (see "Releasing new
// firmware versions" in README.md) — it's not derived from the tag
// automatically, since the tag only exists on the build machine's git
// checkout, not inside the compiled binary.
//
// CI beta builds (.github/workflows/beta.yml) override it with
// -D FIRMWARE_VERSION=\"<version>-beta.<n>\" instead of editing this file.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.2.1"
#endif

#endif // VERSION_H
