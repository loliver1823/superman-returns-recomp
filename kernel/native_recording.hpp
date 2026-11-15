#pragma once
#include <cstdint>
struct PPCContext;

namespace sr::native::recording {
// Complete CPU controller for retail 82110C78. Default OFF. Its allocation,
// coherency, callback emission and append helpers retain their own contracts.
// The caller owns reachable nodes and helper outputs for the complete call.
bool kickGateOn();
bool endRecordingKick(PPCContext& ctx, uint8_t* base);
}
