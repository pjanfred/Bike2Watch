#ifndef FTMS_CONTROL_H
#define FTMS_CONTROL_H

// Everything the web server needs to let the user set the connected
// bike's target resistance level from the dashboard's Resistance card.
// Implemented in main.cpp, which already owns the BLE client and the FTMS
// Control Point characteristic — same reasoning as ble_devices.h being
// implemented there instead of here.

// True once the FTMS Control Point characteristic has been found on the
// currently connected bike (see connectToFtms() in main.cpp) — i.e.
// requestSetResistance() has a real chance of doing something. Always
// false while no bike is connected, and reset the moment it disconnects.
bool ftmsControlPointAvailable();

// Range/step the currently connected bike reported via its optional
// "Supported Resistance Level Range" characteristic (0x2AD6), same
// unitless scale as the resistance level shown on the dashboard.
// `available` is false if the bike didn't expose that characteristic
// (common — it's optional in the spec); the dashboard falls back to a
// generic range in that case (see handleSettingsGet() in web_server.cpp).
struct ResistanceRange {
    bool  available = false;
    float min  = 0.0f;
    float max  = 0.0f;
    float step = 0.0f;
};
ResistanceRange getResistanceRange();

// Queues "Set Target Resistance Level" (FTMS Control Point opcode 0x04)
// for the connected bike, in the bike's own resistance unit/scale — pass
// the same number the dashboard displays, no pre-scaling needed. The
// actual BLE write happens asynchronously on the BLE client's own task
// (bleClientTask in main.cpp), same as every other NimBLE call in this
// codebase, so this never blocks the caller (the web server).
//
// Returns false immediately, without queuing anything, if no bike is
// connected or it never exposed a Control Point characteristic. True only
// means the request was queued, not that the bike accepted it — the write
// itself is fire-and-log, same as Request Control / Start-Resume in
// connectToFtms().
bool requestSetResistance(float level);

#endif // FTMS_CONTROL_H
