#ifndef BIKE2WATCH_H
#define BIKE2WATCH_H

#include <cstdint>
#include <cstddef>
#include <cstring>

// Header-only and Arduino-free by design (see test/test_native) — no BLE
// stack or hardware needed to exercise the parsing/encoding logic below.

// One parsed FTMS "Indoor Bike Data" (0x2AD2) notification. Bikes split
// their data across multiple notifications rather than sending every
// field every time, so each hasX flag must be checked before reading the
// matching value — an absent field is left at its zero-initialized value,
// not a meaningful reading.
struct IndoorBikeData {
    bool hasSpeed;
    bool hasAvgSpeed;
    bool hasCadence;
    bool hasAvgCadence;
    bool hasTotalDistance;
    bool hasResistance;
    bool hasPower;
    bool hasAvgPower;

    uint16_t instantSpeedRaw;   // 0.01 km/h units — see speedKmh()
    uint16_t avgSpeedRaw;       // same units as instantSpeedRaw
    uint16_t instantCadenceRaw; // 0.5 rpm units — see cadenceRpm()
    uint16_t avgCadenceRaw;     // same units as instantCadenceRaw
    uint32_t totalDistance;     // metres
    int16_t  resistanceLevel;   // device-specific unit/scale
    int16_t  instantPower;      // watts
    int16_t  avgPower;          // watts

    float speedKmh()   const { return instantSpeedRaw / 100.0f; }
    float cadenceRpm() const { return instantCadenceRaw / 2.0f; }
};

// Running state for the CSC (Cycling Speed & Cadence) characteristic we
// expose to the Watch — accumulates whole wheel/crank revolutions plus
// the BLE-mandated 1/1024s event timestamps from the bike's continuous
// speed/cadence readings. Carries fractional revolutions across calls so
// slow speeds still accumulate correctly over many small updateCsc()
// calls instead of always rounding down to zero.
struct CscAccumulator {
    uint32_t cumulativeWheelRevs = 0;
    uint16_t lastWheelEventTime  = 0;   // 1/1024s ticks, wraps at 65536 — spec-mandated
    uint16_t cumulativeCrankRevs = 0;
    uint16_t lastCrankEventTime  = 0;   // 1/1024s ticks, wraps at 65536 — spec-mandated
    float fractionalWheelRevs = 0.0f;
    float fractionalCrankRevs = 0.0f;
    // The bike has no real wheel — this only has to match whatever wheel
    // size the paired Watch's sensor profile is configured with. See
    // Bike2WatchSettings::wheelCircumferenceMm in settings.h.
    float wheelCircumferenceM = 2.096f;
};

// Parses one FTMS "Indoor Bike Data" notification payload into `out`.
// Returns false (and leaves `out` unusable) if `data` is truncated
// relative to what its own flags field claims — a malformed or corrupted
// notification, not a normal "field absent" case (that's what the hasX
// flags are for).
inline bool parseIndoorBikeData(const uint8_t* data, size_t len, IndoorBikeData& out) {
    if (len < 2) return false;
    memset(&out, 0, sizeof(out));
    uint16_t flags = data[0] | (data[1] << 8);
    size_t pos = 2;

    // Inverted: flag SET means speed is *absent*. Every other flag below
    // is the normal "set means present" — this is the one exception the
    // FTMS spec makes, easy to misread if skimmed.
    out.hasSpeed = !(flags & 0x0001);
    if (out.hasSpeed) {
        if (pos + 2 > len) return false;
        out.instantSpeedRaw = data[pos] | (data[pos + 1] << 8);
        pos += 2;
    }
    out.hasAvgSpeed = flags & 0x0002;
    if (out.hasAvgSpeed) {
        if (pos + 2 > len) return false;
        out.avgSpeedRaw = data[pos] | (data[pos + 1] << 8);
        pos += 2;
    }
    out.hasCadence = flags & 0x0004;
    if (out.hasCadence) {
        if (pos + 2 > len) return false;
        out.instantCadenceRaw = data[pos] | (data[pos + 1] << 8);
        pos += 2;
    }
    out.hasAvgCadence = flags & 0x0008;
    if (out.hasAvgCadence) {
        if (pos + 2 > len) return false;
        out.avgCadenceRaw = data[pos] | (data[pos + 1] << 8);
        pos += 2;
    }
    out.hasTotalDistance = flags & 0x0010;
    if (out.hasTotalDistance) {
        if (pos + 3 > len) return false;   // 24-bit field, not 16 — no [pos+1]/[pos+2] typo
        out.totalDistance = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16);
        pos += 3;
    }
    out.hasResistance = flags & 0x0020;
    if (out.hasResistance) {
        if (pos + 2 > len) return false;
        out.resistanceLevel = (int16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
    }
    out.hasPower = flags & 0x0040;
    if (out.hasPower) {
        if (pos + 2 > len) return false;
        out.instantPower = (int16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
    }
    out.hasAvgPower = flags & 0x0080;
    if (out.hasAvgPower) {
        if (pos + 2 > len) return false;
        out.avgPower = (int16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
    }
    return true;
}

// Folds `deltaMs` worth of the bike's current speed/cadence into `csc`'s
// running revolution counts and event timestamps. Called once per
// notification cycle (see main.cpp's loop()) with whatever time actually
// elapsed since the last call, so it stays correct even if a cycle is
// skipped or delayed. A speed/cadence of exactly 0 intentionally leaves
// the accumulator untouched rather than dividing by it.
// Converts a revolution period in seconds to 1/1024 s ticks, clamped to the
// uint16_t range. Casting an out-of-range float to an integer is undefined
// behaviour, and a very slow wheel/crank (or a tiny rate) would otherwise
// produce periods above 64 s.
inline uint16_t cscTicksPerRev(float periodSec) {
    float ticks = periodSec * 1024.0f;
    if (!(ticks > 0.0f)) return 0;
    if (ticks >= 65535.0f) return 65535;
    return (uint16_t)ticks;
}

inline void updateCsc(CscAccumulator& csc, float speedKmh, float cadenceRpm, uint32_t deltaMs) {
    float deltaSec = deltaMs / 1000.0f;
    if (speedKmh > 0.0f) {
        float speedMs = speedKmh / 3.6f;
        float distanceM = speedMs * deltaSec;
        csc.fractionalWheelRevs += distanceM / csc.wheelCircumferenceM;
        float wheelRevsPerSec = speedMs / csc.wheelCircumferenceM;
        uint16_t ticksPerRev = cscTicksPerRev(1.0f / wheelRevsPerSec);
        while (csc.fractionalWheelRevs >= 1.0f) {
            csc.cumulativeWheelRevs++;
            csc.fractionalWheelRevs -= 1.0f;
            csc.lastWheelEventTime += ticksPerRev;
        }
    }
    if (cadenceRpm > 0.0f) {
        float revsPerSec = cadenceRpm / 60.0f;
        csc.fractionalCrankRevs += revsPerSec * deltaSec;
        uint16_t ticksPerRev = cscTicksPerRev(60.0f / cadenceRpm);
        while (csc.fractionalCrankRevs >= 1.0f) {
            csc.cumulativeCrankRevs++;
            csc.fractionalCrankRevs -= 1.0f;
            csc.lastCrankEventTime += ticksPerRev;
        }
    }
}

// Encodes the CSC Measurement characteristic (0x2A5B) payload from `csc`'s
// current totals. `includeWheel`/`includeCrank` mirror whichever data the
// bike actually has available right now (see main.cpp) — the CSC spec
// allows either field to be omitted per notification, signaled by the
// leading flags byte this writes. `buf` must be at least 11 bytes
// (1 flags + 6 wheel + 4 crank, the worst case with both included).
inline size_t buildCscMeasurement(uint8_t* buf, const CscAccumulator& csc,
                                   bool includeWheel, bool includeCrank) {
    size_t pos = 0;
    uint8_t flags = 0;
    if (includeWheel) flags |= 0x01;
    if (includeCrank) flags |= 0x02;
    buf[pos++] = flags;
    if (includeWheel) {
        buf[pos++] =  csc.cumulativeWheelRevs        & 0xFF;
        buf[pos++] = (csc.cumulativeWheelRevs >> 8)  & 0xFF;
        buf[pos++] = (csc.cumulativeWheelRevs >> 16) & 0xFF;
        buf[pos++] = (csc.cumulativeWheelRevs >> 24) & 0xFF;
        buf[pos++] =  csc.lastWheelEventTime        & 0xFF;
        buf[pos++] = (csc.lastWheelEventTime >> 8)  & 0xFF;
    }
    if (includeCrank) {
        buf[pos++] =  csc.cumulativeCrankRevs        & 0xFF;
        buf[pos++] = (csc.cumulativeCrankRevs >> 8)  & 0xFF;
        buf[pos++] =  csc.lastCrankEventTime         & 0xFF;
        buf[pos++] = (csc.lastCrankEventTime >> 8)   & 0xFF;
    }
    return pos;
}

// Encodes the Cycling Power Measurement characteristic (0x2A63) payload —
// just a zero flags field followed by the instantaneous power, the only
// field this bridge ever populates. `buf` must be at least 4 bytes.
inline size_t buildCpMeasurement(uint8_t* buf, int16_t powerWatts) {
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = (uint8_t)( powerWatts       & 0xFF);
    buf[3] = (uint8_t)((powerWatts >> 8) & 0xFF);
    return 4;
}

#endif // BIKE2WATCH_H
