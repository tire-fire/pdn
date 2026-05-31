#pragma once

// WIRE-FORMAT STABLE — do not reorder; new values must be appended at the end.
// This integer mapping is transmitted on serial as part of MAC_ADV (opcode 0x00).
// Reordering would silently break wire compatibility in multi-firmware scenarios.
// Today's deployment is single-firmware so the cost is currently theoretical;
// preserve this constraint anyway. If future contributors need to reorder for
// style, introduce a separate WireDeviceType enum with explicit value assignments
// and a translation function.
enum class DeviceType {
    UNKNOWN = 0,
    PDN = 1,
    FDN = 2,
};