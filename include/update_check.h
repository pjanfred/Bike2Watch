#ifndef UPDATE_CHECK_H
#define UPDATE_CHECK_H

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

// Header-only and Arduino-free by design (see test/test_native) — no WiFi/
// HTTPS stack needed to exercise the version-compare or JSON-field-
// extraction logic below. The Arduino/network side (fetching the GitHub
// API, downloading+flashing the matching asset) lives in web_server.cpp,
// following the same async-task shape as its existing wifiTestTask().

// Finds the first occurrence of `needle` (length `needleLen`) inside `buf`
// (length `bufLen`), neither of which is assumed to be null-terminated —
// the GitHub API response this is used on is read into a fixed-size
// buffer that may be cut off mid-document (see extractJsonStringField()
// below). Avoids memmem(), which isn't guaranteed available in every
// toolchain this header gets built with (native unit tests vs. the ESP32
// Arduino core).
inline const char* findInBuf(const char* buf, size_t bufLen,
                              const char* needle, size_t needleLen) {
    if (!buf || !needle || needleLen == 0 || needleLen > bufLen) return nullptr;
    for (size_t i = 0; i + needleLen <= bufLen; i++) {
        if (memcmp(buf + i, needle, needleLen) == 0) return buf + i;
    }
    return nullptr;
}

// Parses up to 3 dot-separated numeric components ("MAJOR.MINOR.PATCH")
// from `version`, skipping one optional leading 'v'/'V' (GitHub tag names
// look like "v1.2.0"; FIRMWARE_VERSION doesn't have the prefix). Missing
// trailing components default to 0 ("1.2" == "1.2.0"). A non-numeric
// component stops parsing there, defaulting everything after it to 0,
// rather than failing outright — isNewerVersion() below relies on that to
// treat unparseable input as "not newer" instead of crashing or guessing.
inline void parseVersionParts(const char* version, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!version) return;

    const char* p = version;
    if (*p == 'v' || *p == 'V') p++;

    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*p)) return;
        long value = 0;
        while (isdigit((unsigned char)*p)) {
            value = value * 10 + (*p - '0');
            p++;
        }
        out[i] = (int)value;
        if (*p != '.') return;
        p++;
    }
}

// Returns the SemVer pre-release part of `version` ("beta.2" for
// "v0.3.0-beta.2"), or nullptr for a plain release or unparseable input.
inline const char* versionPrerelease(const char* version) {
    if (!version) return nullptr;
    const char* p = version;
    if (*p == 'v' || *p == 'V') p++;
    if (!isdigit((unsigned char)*p)) return nullptr;
    while (isdigit((unsigned char)*p) || *p == '.') p++;
    if (*p != '-' || p[1] == '\0') return nullptr;
    return p + 1;
}

// Compares two pre-release strings per SemVer: dot-separated identifiers,
// numeric ones compared as numbers ("beta.2" < "beta.10"), numeric lower
// than alphanumeric, a shorter list lower when all earlier ones are equal.
// Returns <0, 0 or >0.
inline int comparePrerelease(const char* a, const char* b) {
    while (*a || *b) {
        if (!*a) return -1;
        if (!*b) return 1;
        bool aNum = isdigit((unsigned char)*a), bNum = isdigit((unsigned char)*b);
        const char* aEnd = a; while (*aEnd && *aEnd != '.') aEnd++;
        const char* bEnd = b; while (*bEnd && *bEnd != '.') bEnd++;
        for (const char* q = a; q < aEnd; q++) if (!isdigit((unsigned char)*q)) aNum = false;
        for (const char* q = b; q < bEnd; q++) if (!isdigit((unsigned char)*q)) bNum = false;
        if (aNum && bNum) {
            long av = 0, bv = 0;
            for (const char* q = a; q < aEnd; q++) av = av * 10 + (*q - '0');
            for (const char* q = b; q < bEnd; q++) bv = bv * 10 + (*q - '0');
            if (av != bv) return av < bv ? -1 : 1;
        } else if (aNum != bNum) {
            return aNum ? -1 : 1;
        } else {
            size_t aLen = aEnd - a, bLen = bEnd - b;
            int c = strncmp(a, b, aLen < bLen ? aLen : bLen);
            if (c != 0) return c < 0 ? -1 : 1;
            if (aLen != bLen) return aLen < bLen ? -1 : 1;
        }
        a = *aEnd ? aEnd + 1 : aEnd;
        b = *bEnd ? bEnd + 1 : bEnd;
    }
    return 0;
}

// True only if `latestTag` (GitHub's `tag_name`, e.g. "v1.2.0") is a
// strictly newer version than `currentVersion` (FIRMWARE_VERSION, e.g.
// "1.2.0", no "v"). False for equal, older, or unparseable input — an
// update flow that's unsure should never claim "update available".
// Pre-releases sort before their release ("1.2.0-beta.3" < "1.2.0") and
// among themselves per comparePrerelease().
inline bool isNewerVersion(const char* latestTag, const char* currentVersion) {
    int latest[3];
    int current[3];
    parseVersionParts(latestTag, latest);
    parseVersionParts(currentVersion, current);

    for (int i = 0; i < 3; i++) {
        if (latest[i] != current[i]) return latest[i] > current[i];
    }
    const char* latestPre = versionPrerelease(latestTag);
    const char* currentPre = versionPrerelease(currentVersion);
    if (!latestPre && !currentPre) return false;
    if (!latestPre) return true;      // release beats any pre-release
    if (!currentPre) return false;
    return comparePrerelease(latestPre, currentPre) > 0;
}

// Bounded, single-pass extraction of a top-level `"key":"value"` string
// field out of `json` (length `jsonLen`, not necessarily null-terminated,
// not necessarily complete JSON — see web_server.cpp's bounded ~8KB read
// of GitHub's /releases/latest response). Copies the value into `out`
// (size `outSize`), un-escaping \" and \\ only — the only escapes the
// fields this is used for (tag_name, browser_download_url, a release's
// body) are ever expected to need. If the value runs past the end of
// `json` or `out` before its closing quote (the bounded-read truncation
// case, most relevant to the possibly-huge "body" field), what was copied
// so far is still null-terminated and this returns true — a truncated
// value is not treated as a failure. Returns false only if `key` isn't
// found at all, or isn't followed by a `"..."` string value.
inline bool extractJsonStringField(const char* json, size_t jsonLen,
                                    const char* key, char* out, size_t outSize) {
    if (!json || !key || !out || outSize == 0) return false;
    out[0] = '\0';

    char needle[40];
    int needleLen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needleLen <= 0 || (size_t)needleLen >= sizeof(needle)) return false;

    const char* keyPos = findInBuf(json, jsonLen, needle, (size_t)needleLen);
    if (!keyPos) return false;

    const char* end = json + jsonLen;
    const char* p = keyPos + needleLen;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != ':') return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != '"') return false;
    p++;

    size_t outLen = 0;
    while (p < end && outLen + 1 < outSize) {
        char c = *p;
        if (c == '"') { p++; break; }
        if (c == '\\' && p + 1 < end && (p[1] == '"' || p[1] == '\\')) {
            out[outLen++] = p[1];
            p += 2;
            continue;
        }
        out[outLen++] = c;
        p++;
    }
    out[outLen] = '\0';
    return true;
}

// Scans `json`'s top-level "assets" array (length `jsonLen`) for the first
// asset object whose "name" field contains `nameContains` (e.g. BOARD_ENV,
// "esp32dev" or "esp32-s3-devkitc-1" — these are never substrings of each
// other, see platformio.ini, so a plain substring match is safe), and
// copies that asset's "browser_download_url" into `out` (size `outSize`).
// Returns false if no matching asset is found. Deliberately scoped to
// start searching only after the "assets" key (not a bare global "name"
// search) because a GitHub release object also has its own top-level
// "name" (the release title) *before* "assets" — matching that by
// accident would silently pick the wrong field.
inline bool findAssetDownloadUrl(const char* json, size_t jsonLen,
                                  const char* nameContains,
                                  char* out, size_t outSize) {
    if (!json || !nameContains || !out || outSize == 0) return false;
    out[0] = '\0';

    static const char kAssetsKey[] = "\"assets\"";
    static const char kNameKey[]   = "\"name\"";
    const size_t kNameKeyLen = sizeof(kNameKey) - 1;

    const char* end       = json + jsonLen;
    const char* assetsPos = findInBuf(json, jsonLen, kAssetsKey, sizeof(kAssetsKey) - 1);
    if (!assetsPos) return false;

    const char* cursor = assetsPos + (sizeof(kAssetsKey) - 1);
    while (cursor < end) {
        const char* namePos = findInBuf(cursor, (size_t)(end - cursor), kNameKey, kNameKeyLen);
        if (!namePos) return false;

        char name[128];
        if (!extractJsonStringField(namePos, (size_t)(end - namePos), "name", name, sizeof(name))) {
            return false;
        }

        // The next "name" occurrence (if any) belongs to the following
        // asset object — bound this asset's search window there, so
        // browser_download_url can never be matched from a different
        // asset than the "name" that was just checked.
        const char* nextNamePos = findInBuf(namePos + kNameKeyLen, (size_t)(end - (namePos + kNameKeyLen)),
                                             kNameKey, kNameKeyLen);
        const char* windowEnd = nextNamePos ? nextNamePos : end;

        if (strstr(name, nameContains)) {
            return extractJsonStringField(namePos, (size_t)(windowEnd - namePos),
                                           "browser_download_url", out, outSize);
        }

        cursor = namePos + kNameKeyLen;
    }
    return false;
}

#endif // UPDATE_CHECK_H
