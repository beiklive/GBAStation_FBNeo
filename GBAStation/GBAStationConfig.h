/// @file GBAStationConfig.h
/// @brief Minimal hardcoded configuration for GBAStation overlay (fbneo)
#pragma once

#include <string>

namespace GBAStationConfig {
    constexpr const char* TEST_ROM = "sdmc:/GBAStation/Arcade/roms/rom.zip";

    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";
    constexpr const char* SYSTEM_PATH = "sdmc:/GBAStation/bios/Arcade/";
    constexpr const char* SAVES_PATH = "sdmc:/GBAStation/saves/Arcade/";
    constexpr const char* STATES_PATH = "sdmc:/GBAStation/saves/Arcade/";

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    /// @brief Use callback/ring-buffer path (supports resampling)
    constexpr bool USE_SDLQUEUEAUDIO = false;
}

/// @brief UI action identifiers for the helpers bar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
