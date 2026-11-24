#pragma once

#include <cstdint>

namespace pf {

enum class Mode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };
char const* modeName(Mode m);
bool isFlyMode(Mode m);     // ship, UFO, wave, swing
bool isBoundedMode(Mode m); // modes that get a floor + ceiling (flying modes, ball, spider)

enum class ObjKind : uint8_t { Solid, Hazard, Orb, Pad, Portal, Speed };

} // namespace pf
