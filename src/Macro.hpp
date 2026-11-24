#pragma once

// Macro data model + GDR file I/O.
//
// Supported inputs (detected from the file contents, not the extension):
//   .gdr2      new binary GDR (maxnut/GDReplayFormat, "GDR" magic)
//   .gdr       old GDR as MessagePack (xdBot's default save format)
//   .gdr.json  old GDR as JSON (xdBot "save as JSON")
// The output is written in the same format as the input.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Tree.hpp"

namespace mr {

// One press or release at a physics tick (240 TPS, GJGameState::m_currentProgress).
struct InputEvent {
    uint32_t frame = 0;
    int button = 1; // 1 = jump, 2 = left, 3 = right (PlayerButton)
    bool player2 = false;
    bool down = false;
};

// A press and its matching release.
struct HoldSegment {
    uint32_t startFrame = 0;
    uint32_t endFrame = 0; // kOpenEnd = never released
    int button = 1;
    bool player2 = false;
};

constexpr uint32_t kOpenEnd = 0xFFFFFFFFu;

inline bool sameChannel(HoldSegment const& a, HoldSegment const& b) {
    return a.button == b.button && a.player2 == b.player2;
}

enum class MacroFormat { Gdr2, GdrMsgpack, GdrJson };

struct Macro {
    MacroFormat format = MacroFormat::GdrJson;
    std::filesystem::path path;
    std::vector<InputEvent> events;
    double framerate = 240.0;
    bool platformer = false;
    std::string botName;
    std::string botVersion;
    std::string levelName;
    int64_t levelID = 0;
    // xdBot's own macros count frames from level time (int(levelTime * 240) + 1),
    // not from m_currentProgress. They're converted to ticks during the first pass.
    bool xdBotFrames = false;
    int xdOffset = 0; // xdBot older than 2.3.6 stored frames one lower

    Value tree;                 // old GDR document (kept so unknown fields survive)
    std::vector<uint8_t> raw;   // original gdr2 bytes (header is reused on save)
};

bool loadMacro(std::filesystem::path const& path, Macro& out, std::string& err);
bool saveMacro(Macro const& src, std::vector<InputEvent> const& events, std::filesystem::path const& outPath,
               std::string const& botName, std::string& err);
std::string outputExtension(Macro const& m);

// Pairs presses with releases per (player, button) channel. Repeated presses
// while already held and releases while not held are dropped.
std::vector<HoldSegment> toSegments(std::vector<InputEvent> const& events);
// Back to a frame-sorted event list (within a channel: press, release, press...).
std::vector<InputEvent> toEvents(std::vector<HoldSegment> const& segs);
void sortSegments(std::vector<HoldSegment>& segs);

} // namespace mr
