/// @file GBAStationOverlay.cpp
/// @brief Overlay UI for GBAStation-integrated fbneo (no disc support)

#define IMGUI_DEFINE_MATH_OPERATORS
#include "GBAStationOverlay.h"
#include "GBAStationAudio.h"
#include "GBAStationCore.h"
#include "GBAStationConfig.h"
#include "GBAStationTranslationManager.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include "GBAStationUtils.h"
#include <json.hpp>

#ifdef __SWITCH__
#include "glad.h"
#else
#include "glad.h"
#endif

#include <sys/stat.h>
#include <dirent.h>
#include <string>
#include <string_view>
#include <unordered_map>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "deps/stb/stb_image.h"
#define NANOSVG_IMPLEMENTATION
#include "deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "deps/nanosvg/nanosvgrast.h"

static std::string GetStatePath(GBAStationCore *core, int slot)
{
    if (!core) return "";
    std::string romPath = core->GetGamePath();
    std::string romName = romPath;
    size_t lastSlash = romName.find_last_of("/\\");
    if (lastSlash != std::string::npos) romName = romName.substr(lastSlash + 1);
    size_t lastDot = romName.find_last_of(".");
    if (lastDot != std::string::npos) romName = romName.substr(0, lastDot);
    struct stat st = {0};
    mkdir("sdmc:/GBAStation", 0777);
    mkdir("sdmc:/GBAStation/saves", 0777);
    mkdir("sdmc:/GBAStation/saves/Arcade", 0777);
    if (stat(GBAStationConfig::STATES_PATH, &st) == -1) mkdir(GBAStationConfig::STATES_PATH, 0777);
    std::string gameStateDir = std::string(GBAStationConfig::STATES_PATH) + romName + "/";
    if (stat(gameStateDir.c_str(), &st) == -1) mkdir(gameStateDir.c_str(), 0777);
    return gameStateDir + romName + ".state" + std::to_string(slot);
}

namespace UIStyle {
    inline void DrawTextWithShadow(ImDrawList *dl, ImVec2 pos, ImU32 color, const char *text, float shadowOffset = 1.5f) {
        dl->AddText(ImVec2(pos.x + shadowOffset, pos.y + shadowOffset), IM_COL32(0,0,0,50), text);
        dl->AddText(pos, color, text);
    }
    static void DrawSwitchButton(ImDrawList *dl, ImFont *font, float fontSize, ImVec2 center, float size, const char *symbol, float alpha, bool isDark) {
        ImU32 fillCol = IM_COL32(220, 220, 220, (int)(255 * alpha));
        ImU32 textCol = IM_COL32(40, 40, 40, (int)(255 * alpha));
        dl->AddCircleFilled(center, size * 0.5f, fillCol, 12);
        float symSize = fontSize * 0.75f;
        ImVec2 textSize = font->CalcTextSizeA(symSize, FLT_MAX, 0.0f, symbol);
        dl->AddText(font, symSize, center - (textSize * 0.5f), textCol, symbol);
    }
}

// Encode a codepoint as UTF-8 into a 4-byte buffer (icon glyphs).
void EncodeUtf8(char *out, int codepoint)
{
    if (codepoint <= 0x7F)
    {
        out[0] = (char)codepoint;
        out[1] = '\0';
    }
    else if (codepoint <= 0x7FF)
    {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        out[2] = '\0';
    }
    else
    {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        out[3] = '\0';
    }
}

static void CycleFBNeoOption(GBAStationCore *core, const char *key,
                             std::initializer_list<const char *> values, int direction)
{
    if (!core || values.size() == 0)
        return;
    const std::string current = core->GetCoreOption(key, *values.begin());
    int index = 0;
    int candidate = 0;
    for (const char *value : values) {
        if (current == value) { index = candidate; break; }
        ++candidate;
    }
    const int count = static_cast<int>(values.size());
    index = (index + direction + count) % count;
    auto value = values.begin();
    std::advance(value, index);
    core->SetCoreOption(key, *value);
}

namespace {

std::unordered_map<std::string, std::string> g_overlayBindings;

std::string TrimConfigValue(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string DecodeConfigValue(std::string_view encoded) {
    std::string value = TrimConfigValue(encoded);
    if (value.size() > 2 && value[1] == '|' && value[0] == 's') {
        std::string decoded;
        decoded.reserve(value.size() - 2);
        bool escaped = false;
        for (std::size_t i = 2; i < value.size(); ++i) {
            const char c = value[i];
            if (escaped) {
                decoded.push_back(c);
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else {
                decoded.push_back(c);
            }
        }
        if (escaped) decoded.push_back('\\');
        return decoded;
    }
    return value;
}

void LoadOverlayBindings() {
    g_overlayBindings.clear();
    const char *paths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
    for (const char *path : paths) {
        std::ifstream in(path);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t equal = line.find('=');
            if (equal == std::string::npos) continue;
            g_overlayBindings[TrimConfigValue(std::string_view(line).substr(0, equal))] =
                DecodeConfigValue(std::string_view(line).substr(equal + 1));
        }
        break;
    }
}

bool TokenPressed(SDL_GameController *controller, std::string_view token) {
    const std::string t = TrimConfigValue(token);
    if (t == "PAD_A") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B);
    if (t == "PAD_B") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
    if (t == "PAD_X") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y);
    if (t == "PAD_Y") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X);
    if (t == "PAD_UP") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
    if (t == "PAD_DOWN") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    if (t == "PAD_LEFT") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    if (t == "PAD_RIGHT") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    if (t == "PAD_LB") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    if (t == "PAD_RB") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    if (t == "PAD_LT" || t == "PAD_ZL") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
    if (t == "PAD_RT" || t == "PAD_ZR") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
    if (t == "PAD_START") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
    if (t == "PAD_BACK") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);
    if (t == "PAD_LSB" || t == "PAD_L3") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    if (t == "PAD_RSB" || t == "PAD_R3") return SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (t == "PAD_LEFTSTICKUP") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) < -16000;
    if (t == "PAD_LEFTSTICKDOWN") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) > 16000;
    if (t == "PAD_LEFTSTICKLEFT") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) < -16000;
    if (t == "PAD_LEFTSTICKRIGHT") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) > 16000;
    if (t == "PAD_RIGHTSTICKUP") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) < -16000;
    if (t == "PAD_RIGHTSTICKDOWN") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) > 16000;
    if (t == "PAD_RIGHTSTICKLEFT") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) < -16000;
    if (t == "PAD_RIGHTSTICKRIGHT") return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) > 16000;
    return false;
}

bool ComboPressed(SDL_GameController *controller, std::string_view combo) {
    const std::string value = TrimConfigValue(combo);
    if (value.empty() || value == "none") return false;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find('+', begin);
        const std::string_view token = std::string_view(value).substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin);
        if (!TokenPressed(controller, token)) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool BindingPressed(SDL_GameController *controller, const char *key, const char *fallback) {
    const auto it = g_overlayBindings.find(key);
    const std::string value = it == g_overlayBindings.end() ? fallback : it->second;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find('|', begin);
        const std::string_view combo = std::string_view(value).substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin);
        if (ComboPressed(controller, combo)) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

std::string ConfigOverrideValue(const char *key) {
    const auto it = g_overlayBindings.find(key);
    return it == g_overlayBindings.end() ? std::string{} : it->second;
}

} // namespace

GBAStationOverlay::GBAStationOverlay() {
    m_gameTitle = "Arcade";
    LoadOverlayBindings();
    LoadConfig();
    LoadGeneralConfig();
    LoadAccountData();
    LoadCoreSettings();
#ifdef __SWITCH__
    psmInitialize();
#endif
}

GBAStationOverlay::~GBAStationOverlay()
{
    if (m_triangleTexture != 0)
    {
        glDeleteTextures(1, &m_triangleTexture);
        m_triangleTexture = 0;
    }

    if (m_boltTexture != 0)
    {
        glDeleteTextures(1, &m_boltTexture);
        m_boltTexture = 0;
    }

    if (m_avatarTexture != 0)
    {
        glDeleteTextures(1, &m_avatarTexture);
        m_avatarTexture = 0;
    }
    if (m_focusTexture != 0)
    {
        glDeleteTextures(1, &m_focusTexture);
        m_focusTexture = 0;
    }

#ifdef __SWITCH__
    psmExit();
#endif
}

void GBAStationOverlay::LoadConfig() {
    const char *configPaths[] = {"sdmc:/GBAStation/config/display.jsonc","GBAStation/config/display.jsonc"};
    m_isDarkMode = true; m_showNickname = false;
    FILE *fp = nullptr;
    for (const char *path : configPaths) { fp = fopen(path, "rb"); if (fp) break; }
    if (fp) {
        fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            std::string content; content.resize(size); fread(&content[0], 1, size, fp);
            auto j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded()) {
                    if (j.contains("dark_mode") && j["dark_mode"].is_boolean()) m_isDarkMode = j["dark_mode"].get<bool>();
                    else if (j.contains("darkMode") && j["darkMode"].is_boolean()) m_isDarkMode = j["darkMode"].get<bool>();
                    if (j.contains("show_nickname") && j["show_nickname"].is_boolean()) m_showNickname = j["show_nickname"].get<bool>();
                    else if (j.contains("showNickname") && j["showNickname"].is_boolean()) m_showNickname = j["showNickname"].get<bool>();
                }
        }
        fclose(fp);
    }
}

void GBAStationOverlay::LoadGeneralConfig() {
    const char *configPaths[] = {"sdmc:/GBAStation/config/general.jsonc","GBAStation/config/general.jsonc"};
    m_hourFormat = "24h";
    FILE *fp = nullptr;
    for (const char *path : configPaths) { fp = fopen(path, "rb"); if (fp) break; }
    if (fp) {
        fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            std::string content; content.resize(size); fread(&content[0], 1, size, fp);
            auto j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded() && j.contains("hour_format") && j["hour_format"].is_string())
                    m_hourFormat = j["hour_format"].get<std::string>();
        }
        fclose(fp);
    }
}

void GBAStationOverlay::LoadAccountData() {
#ifdef __SWITCH__
    bool customAvatarLoaded = false;
    const char *avatarPaths[] = {"sdmc:/GBAStation/Arcade/assets/avatar.jpg"};
    for (const char *path : avatarPaths) {
        FILE *fp = fopen(path, "rb"); if (!fp) continue; fclose(fp);
        int width, height, channels;
        unsigned char *data = stbi_load(path, &width, &height, &channels, 4);
        if (data) {
            if (m_avatarTexture != 0) glDeleteTextures(1, &m_avatarTexture);
            glGenTextures(1, &m_avatarTexture); glBindTexture(GL_TEXTURE_2D, m_avatarTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            glBindTexture(GL_TEXTURE_2D, 0); stbi_image_free(data);
            m_nickname = "Player 1"; customAvatarLoaded = true; break;
        }
    }
    if (customAvatarLoaded) return;
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) return;
    AccountUid uid = {0}; bool found = false;
    if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid)) found = true;
    if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid)) found = true;
    if (!found) {
        s32 userCount = 0;
        if (R_SUCCEEDED(accountGetUserCount(&userCount)) && userCount > 0) {
            AccountUid uids[ACC_USER_LIST_SIZE]; s32 actualTotal = 0;
            if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actualTotal)) && actualTotal > 0) { uid = uids[0]; found = true; }
        }
    }
    if (found) {
        AccountProfile profile; AccountProfileBase profileBase;
        if (R_SUCCEEDED(accountGetProfile(&profile, uid))) {
            if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &profileBase))) m_nickname = std::string(profileBase.nickname);
            u32 imageSize = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imageSize)) && imageSize > 0) {
                unsigned char *jpegBuf = (unsigned char *)malloc(imageSize);
                if (jpegBuf) {
                    u32 actualSize = 0;
                    if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpegBuf, imageSize, &actualSize))) {
                        int width, height, channels;
                        unsigned char *rgba = stbi_load_from_memory(jpegBuf, actualSize, &width, &height, &channels, 4);
                        if (rgba) {
                            if (m_avatarTexture != 0) glDeleteTextures(1, &m_avatarTexture);
                            glGenTextures(1, &m_avatarTexture); glBindTexture(GL_TEXTURE_2D, m_avatarTexture);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                            glBindTexture(GL_TEXTURE_2D, 0); stbi_image_free(rgba);
                        }
                    }
                    free(jpegBuf);
                }
            }
            accountProfileClose(&profile);
        }
    }
    accountExit();
#else
    m_nickname = "Player 1";
#endif
}

void GBAStationOverlay::Update(float deltaTime) {
    if (m_currentMenu != OverlayMenu::None) {
        m_animTimer += deltaTime;
#ifdef __SWITCH__
        m_batteryTimer += deltaTime;
        if (m_batteryTimer >= 3.0f) {
            m_batteryTimer = 0.0f;
            psmGetBatteryChargePercentage(&m_batteryLevel);
            PsmChargerType chargerType; psmGetChargerType(&chargerType);
            m_isCharging = (chargerType != PsmChargerType_Unconnected);
        }
        float target = m_isCharging ? 1.0f : 0.0f;
        float diff = target - m_chargingStateProgress;
        if (std::abs(diff) > 0.001f) {
            m_chargingStateProgress += diff * deltaTime * 8.0f;
            m_chargingStateProgress = std::clamp(m_chargingStateProgress, 0.0f, 1.0f);
        }
#endif
    }
}

void GBAStationOverlay::Show() {
    if (m_currentMenu == OverlayMenu::None) {
        m_currentMenu = OverlayMenu::QuickMenu;
        m_animTimer = 0.0f; m_quickMenuSelection = 0; m_sidebarFocused = true;
        LoadOverlayBindings();
        LoadConfig(); LoadGeneralConfig();
    }
}

void GBAStationOverlay::Hide() {
    m_currentMenu = OverlayMenu::None;
    // Input releases while hidden are intentionally ignored. Clear the
    // latched navigation state here so a completed save/load cannot leave
    // the next menu session unresponsive.
    m_upHeld = false;
    m_downHeld = false;
    m_leftHeld = false;
    m_rightHeld = false;
    m_confirmHeld = false;
    m_backHeld = false;
    m_lastInputTime = 0;
}

void GBAStationOverlay::ActivateTab(int tab) {
    m_quickMenuSelection = std::clamp(tab, 0, 7);
    m_settingsSelection = 0;
    m_sidebarFocused = true;
    if (m_quickMenuSelection == 1) {
        m_isSaveMode = true;
        m_currentMenu = OverlayMenu::SaveStates;
    } else if (m_quickMenuSelection == 2) {
        m_isSaveMode = false;
        m_currentMenu = OverlayMenu::SaveStates;
    } else if (m_quickMenuSelection == 4 || m_quickMenuSelection == 5) {
        m_currentMenu = OverlayMenu::Settings;
    } else {
        m_currentMenu = OverlayMenu::QuickMenu;
    }
    m_animTimer = 0.4f;
}

void GBAStationOverlay::Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                         int frameWidth, int frameHeight, int fboWidth, int fboHeight) {
    ImDrawList *bgDrawList = ImGui::GetBackgroundDrawList();
    ImDrawList *fgDrawList = ImGui::GetForegroundDrawList();
    RenderGame(bgDrawList, displaySize, gameTexture, aspectRatio, frameWidth, frameHeight, fboWidth, fboHeight);
    if (m_currentMenu != OverlayMenu::None) {
        RenderOverlayBackground(fgDrawList, displaySize);
        RenderGBAStationMenu(fgDrawList, displaySize);
        RenderHelpersBar(fgDrawList, displaySize);
    }
    // RA alerts always render (even during gameplay, not just when menu is open)
    RenderRAAlerts(fgDrawList, displaySize, ImGui::GetIO().DeltaTime);
}

void GBAStationOverlay::RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime) {
    if (!m_core) return;
    auto& notifications = m_core->m_raNotifications;
    if (notifications.empty()) return;

    // Lazy-load RA icon from SVG if not loaded yet
    if (m_core->m_raIconTexture == 0) {
        // Load ra.svg as texture using nanosvg (available in this TU)
        const char* svgPath = "romfs:/assets/ra.svg";
        NSVGimage* image = nsvgParseFromFile(svgPath, "px", 96);
        if (image) {
            float sc = 64.0f / image->height;
            int w = (int)(image->width * sc), h = (int)(image->height * sc);
            NSVGrasterizer* rast = nsvgCreateRasterizer();
            if (rast) {
                unsigned char* img = (unsigned char*)malloc(w * h * 4);
                if (img) {
                    nsvgRasterize(rast, image, 0, 0, sc, img, w, h, w * 4);
                    unsigned int tex = 0;
                    glGenTextures(1, &tex);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    m_core->m_raIconTexture = tex;
                    free(img);
                }
                nsvgDeleteRasterizer(rast);
            }
            nsvgDelete(image);
        }
    }

    float scale = ImGui::GetIO().FontGlobalScale;
    ImFont *font = ImGui::GetFont();
    ImFont *descFont = font;
    if (ImGui::GetIO().Fonts->Fonts.Size > 1) {
        descFont = ImGui::GetIO().Fonts->Fonts[1];
    }
    float descFontSize = ImGui::GetFontSize() * 0.65f;
    float titleFontSize = ImGui::GetFontSize() * 0.85f;

    // Alert dimensions
    float alertW = 420.0f * scale; // wider
    float alertH = 100.0f * scale; // taller
    float padding = 12.0f * scale;
    float margin = 16.0f * scale;
    float spacing = 8.0f * scale;
    float cornerRadius = 14.0f * scale;
    float badgeSize = 76.0f * scale; // fits padding perfectly (100 - 24 = 76)
    float badgeRadius = 4.0f * scale; // less roundness per RA spec
    float badgeMargin = 12.0f * scale;

    RAAlertPosition pos = m_core->m_raAlertPosition;
    bool isTop = (pos == RAAlertPosition::TopLeft || pos == RAAlertPosition::TopRight);
    bool isRight = (pos == RAAlertPosition::TopRight || pos == RAAlertPosition::BottomRight);

    // Update timers and remove expired
    for (auto& n : notifications) {
        n.timer += deltaTime;
    }
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [](const RANotification& n) { return n.timer >= n.duration; }),
        notifications.end());

    // Render each notification
    for (size_t i = 0; i < notifications.size(); i++) {
        auto& n = notifications[i];

        // Lazy-resolve badge texture (may have been downloaded after notification was pushed)
        if (n.textureId == 0 && !n.badge_name.empty()) {
            if (n.badge_name == "ra_icon") {
                n.textureId = m_core->m_raIconTexture;
            } else {
                n.textureId = m_core->GetRABadgeTexture(n.badge_name);
            }
        }

        // Calculate slide animation
        float slideProgress;
        if (n.timer < n.slideIn) {
            float t = n.timer / n.slideIn;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else if (n.timer > n.duration - n.slideOut) {
            float t = (n.duration - n.timer) / n.slideOut;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else {
            slideProgress = 1.0f;
        }

        // Calculate position
        float stackOffset = (float)i * (alertH + spacing);
        float anchorX = isRight ? (displaySize.x - alertW - margin) : margin;
        float anchorY = isTop ? (margin + stackOffset) : (displaySize.y - margin - alertH - stackOffset);
        float slideOffsetY = isTop
            ? -(alertH + margin + stackOffset) * (1.0f - slideProgress)
            : (alertH + margin + stackOffset) * (1.0f - slideProgress);

        float drawY = anchorY + slideOffsetY;
        int alpha = (int)(230 * slideProgress);
        if (alpha <= 0) continue;

        ImVec2 rectMin(anchorX, drawY);
        ImVec2 rectMax(anchorX + alertW, drawY + alertH);

        // Background — glassmorphic rounded rectangle
        ImU32 bgColor = m_isDarkMode
            ? IM_COL32(35, 35, 40, alpha)
            : IM_COL32(245, 248, 252, alpha);
        ImU32 borderColor = m_isDarkMode
            ? IM_COL32(70, 70, 80, (int)(180 * slideProgress))
            : IM_COL32(200, 205, 215, (int)(200 * slideProgress));

        dl->AddRectFilled(rectMin, rectMax, bgColor, cornerRadius);
        dl->AddRect(rectMin, rectMax, borderColor, cornerRadius, 0, 1.5f * scale);

        // Badge image (left side)
        float textX = rectMin.x + padding;
        if (n.textureId != 0) {
            float badgeX = rectMin.x + badgeMargin;
            float badgeY = rectMin.y + (alertH - badgeSize) * 0.5f;

            float drawBadgeSize = badgeSize;
            float drawBadgeX = badgeX;
            float drawBadgeY = badgeY;

            // Make the general RA icon a bit smaller to fit visually better
            if (n.badge_name == "ra_icon") {
                drawBadgeSize = badgeSize * 0.70f;
                drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
                drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
            }

            ImVec2 bMin(drawBadgeX, drawBadgeY);
            ImVec2 bMax(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize);
            ImU32 imgCol = IM_COL32(255, 255, 255, alpha);
            dl->AddImageRounded((ImTextureID)(uintptr_t)n.textureId,
                bMin, bMax, ImVec2(0,0), ImVec2(1,1), imgCol, badgeRadius);
            
            textX = badgeX + badgeSize + badgeMargin;
        }

        // Description text
        ImU32 descColor = m_isDarkMode
            ? IM_COL32(185, 185, 195, alpha)
            : IM_COL32(80, 80, 95, alpha);
        float maxDescW = rectMax.x - textX - padding;

        ImU32 titleColor = m_isDarkMode
            ? IM_COL32(255, 255, 255, alpha)
            : IM_COL32(30, 30, 40, alpha);

        std::string desc = n.description;
        float maxDescH = descFontSize * 2.5f; // height for roughly 2 lines
        ImVec2 fullSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        // If content goes through 2 lines, slice and add '...'
        if (fullSize.y > maxDescH) {
            desc += "...";
            while (desc.length() > 4) {
                ImVec2 testSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
                if (testSize.y <= maxDescH) break;
                desc.erase(desc.length() - 4, 1);
            }
        }

        std::string titleStr = n.title;
        ImVec2 titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        if (titleSize.x > maxDescW) {
            titleStr += "...";
            while (titleStr.length() > 4) {
                ImVec2 testSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
                if (testSize.x <= maxDescW) break;
                titleStr.erase(titleStr.length() - 4, 1);
            }
            // Recalculate titleSize for accurate vertical centering
            titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        }

        ImVec2 descSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        float textSpacing = 4.0f * scale;
        float totalTextH = titleSize.y + textSpacing + descSize.y;
        float titleY = rectMin.y + (alertH - totalTextH) * 0.5f;
        float descY = titleY + titleSize.y + textSpacing;

        dl->AddText(font, titleFontSize,
            ImVec2(textX + 1.0f, titleY + 1.0f),
            IM_COL32(0, 0, 0, (int)(80 * slideProgress)),
            titleStr.c_str());
        dl->AddText(font, titleFontSize,
            ImVec2(textX, titleY), titleColor, titleStr.c_str());

        dl->AddText(descFont, descFontSize, ImVec2(textX, descY), descColor, desc.c_str(), nullptr, maxDescW);
    }
}
void GBAStationOverlay::RenderSocialArea(ImDrawList *dl, ImVec2 displaySize) {
    (void)dl;
    (void)displaySize;
}

void GBAStationOverlay::RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                             float aspectRatio, int width, int height, int fboWidth, int fboHeight) {
    if (texture == 0) return;
    int baseW = (width > 0) ? width : 320;
    int baseH = (height > 0) ? height : 240;
    float dstWidth = displaySize.x, dstHeight = displaySize.y, offsetX = 0, offsetY = 0;
    if (m_displayMode == GambatteDisplayMode::Integer) {
        int scale;
        if (m_displaySize == GambatteDisplaySize::Auto) {
            int scaleX = (int)displaySize.x / baseW, scaleY = (int)displaySize.y / baseH;
            scale = std::min(scaleX, scaleY); if (scale < 1) scale = 1;
        } else { scale = (int)m_displaySize - 3; if (scale < 1) scale = 1; }
        dstWidth = baseW * scale; dstHeight = baseH * scale;
        if (dstWidth > displaySize.x) dstWidth = displaySize.x;
        if (dstHeight > displaySize.y) dstHeight = displaySize.y;
    } else {
        switch (m_displaySize) {
        case GambatteDisplaySize::Stretch: dstWidth = displaySize.x; dstHeight = displaySize.y; break;
        case GambatteDisplaySize::_4_3: {
            float ar = 4.0f/3.0f, da = displaySize.x/displaySize.y;
            if (ar > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/ar; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*ar; } break;
        }
        case GambatteDisplaySize::_16_9: {
            float ar = 16.0f/9.0f, da = displaySize.x/displaySize.y;
            if (ar > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/ar; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*ar; } break;
        }
        default: {
            float da = displaySize.x/displaySize.y;
            if (aspectRatio > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/aspectRatio; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*aspectRatio; } break;
        }}
    }
    dstWidth = std::floor(dstWidth);
    dstHeight = std::floor(dstHeight);
    offsetX = std::floor((displaySize.x - dstWidth) / 2.0f);
    offsetY = std::floor((displaySize.y - dstHeight) / 2.0f);
    dl->AddRectFilled(ImVec2(0,0), displaySize, IM_COL32(0,0,0,255));
    float texW = (fboWidth > 0) ? (float)fboWidth : (float)baseW;
    float texH = (fboHeight > 0) ? (float)fboHeight : (float)baseH;
    float halfU = 0.5f / texW;
    float halfV = 0.5f / texH;
    float u_max = (width > 0) ? ((float)width / texW) - halfU : 1.0f;
    float v_max = (height > 0) ? ((float)height / texH) - halfV : 1.0f;
    dl->AddImage((ImTextureID)(intptr_t)texture, ImVec2(offsetX,offsetY), ImVec2(offsetX+dstWidth,offsetY+dstHeight), ImVec2(halfU,halfV), ImVec2(u_max,v_max));
}

void GBAStationOverlay::RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize) {
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    const int alpha = static_cast<int>(92 * ease);
    dl->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(4, 7, 13, alpha));
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(displaySize.x, 74.0f),
                      IM_COL32(3, 9, 16, static_cast<int>(116 * ease)));
    dl->AddRectFilled(ImVec2(0, displaySize.y - 50.0f), displaySize,
                      IM_COL32(6, 14, 23, static_cast<int>(116 * ease)));
}

void GBAStationOverlay::RenderTitleCard(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;

    std::string titleStr = "游戏菜单";
    if (m_currentMenu == OverlayMenu::SaveStates) {
        titleStr = m_isSaveMode ? "保存状态" : "读取状态";
    } else if (m_currentMenu == OverlayMenu::Settings) {
        titleStr = "画面设置";
    }

    float scale = ImGui::GetIO().FontGlobalScale;
    ImFont *font = ImGui::GetFont();
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    const float titleSize = ImGui::GetFontSize() * 0.95f;
    const float subSize = ImGui::GetFontSize() * 0.68f;
    dl->AddText(font, titleSize, ImVec2(56.0f * scale, 24.0f * scale),
                IM_COL32(238, 247, 255, static_cast<int>(255 * easeOut)),
                "GBAStation Arcade");
    ImVec2 sub = font->CalcTextSizeA(subSize, FLT_MAX, 0.0f, titleStr.c_str());
    dl->AddText(font, subSize, ImVec2(displaySize.x - 56.0f * scale - sub.x, 30.0f * scale),
                IM_COL32(154, 178, 197, static_cast<int>(255 * easeOut)),
                titleStr.c_str());
    dl->AddRectFilled(ImVec2(404.0f * scale, 73.0f * scale),
                      ImVec2(displaySize.x - 48.0f * scale, 74.0f * scale),
                      IM_COL32(8, 17, 27, static_cast<int>(255 * easeOut)));
}

/// @brief Render an animated menu container with rounded corners
static void RenderMenuContainer(ImDrawList *dl, ImVec2 displaySize, float menuWidth, int numItems, float itemHeight, float animTimer, bool isDark,
                                ImVec2 &menuPos, ImVec2 &menuSize, float &easeOut, float &cornerRadius) {
    float scale = ImGui::GetIO().FontGlobalScale;
    float t = std::min(animTimer / 0.4f, 1.0f);
    easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    menuSize = ImVec2(menuWidth, numItems * itemHeight);
    menuPos = ImVec2(48.0f * scale - 80.0f * scale * (1.0f - easeOut), 116.0f * scale);
    cornerRadius = 6.0f * scale;
    dl->AddRectFilled(ImVec2(48.0f * scale, 92.0f * scale),
                      ImVec2(384.0f * scale, displaySize.y - 70.0f * scale),
                      IM_COL32(12, 37, 40, static_cast<int>(244 * easeOut)), 6.0f * scale);
    dl->AddRectFilled(ImVec2(404.0f * scale, 110.0f * scale),
                      ImVec2(displaySize.x - 48.0f * scale, displaySize.y - 86.0f * scale),
                      IM_COL32(14, 23, 28, static_cast<int>(244 * easeOut)), 6.0f * scale);
    dl->AddRectFilled(ImVec2(404.0f * scale, 110.0f * scale),
                      ImVec2(408.0f * scale, displaySize.y - 86.0f * scale),
                      IM_COL32(15, 142, 122, static_cast<int>(185 * easeOut)), 2.0f * scale);
}

static void RenderMenuItem(ImDrawList *dl, ImVec2 menuPos, ImVec2 menuSize, int i, int numItems, float itemHeight,
                           bool isSelected, float cornerRadius, float easeOut, bool isDark, ImFont *font, float fontSize, const char *text) {
    float scale = ImGui::GetIO().FontGlobalScale;
    float itemY = menuPos.y + i * itemHeight;
    ImVec2 itemMin(menuPos.x, itemY), itemMax(menuPos.x + menuSize.x, itemY + itemHeight);
    if (isSelected) {
        ImU32 selCol = IM_COL32(15, 142, 122, static_cast<int>(255 * easeOut));
        dl->AddRectFilled(ImVec2(itemMin.x + 12.0f * scale, itemMin.y),
                          ImVec2(itemMax.x - 12.0f * scale, itemMax.y),
                          selCol, cornerRadius);
    }
    ImU32 textColor;
    textColor = isSelected ? IM_COL32(244,255,252,(int)(255*easeOut)) : IM_COL32(186,214,208,(int)(255*easeOut));
    ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    dl->AddText(font, fontSize, ImVec2(itemMin.x + 34.0f * scale, itemMin.y + (itemHeight - sz.y) / 2), textColor, text);
}

void GBAStationOverlay::EnsureFocusTexture() {
    if (m_focusTexture != 0) return;
#ifdef __SWITCH__
    const char *path = "romfs:/assets/ui/border_gradient.png";
#else
    const char *path = "GBAStation/assets/ui/border_gradient.png";
#endif
    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return;
    }
    glGenTextures(1, &m_focusTexture);
    glBindTexture(GL_TEXTURE_2D, m_focusTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);
}

void GBAStationOverlay::DrawFocusBorder(ImVec2 min, ImVec2 max, float thickness) {
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    const float x = min.x, y = min.y, w = max.x - min.x, h = max.y - min.y;
    if (m_focusTexture)
    {
        // Animated flowing gradient: advance a UV window around the border so
        // the highlight travels, matching the 3DS menu's FlowBorder.
        const float borderWidth = std::max(4.0f, thickness * 2.0f);
        const double milliseconds = static_cast<double>(SDL_GetTicks64());
        float uv = static_cast<float>(std::fmod(milliseconds / 3600.0, 1.0));
        const float topLength = w + borderWidth * 2.0f;
        const float sideLength = h;
        const float advance = 1.0f / 256.0f;
        const ImTextureID texture = (ImTextureID)(uintptr_t)m_focusTexture;
        float next = uv + topLength * advance;
        fg->AddImage(texture, ImVec2(x - borderWidth, y - borderWidth),
                     ImVec2(x + w + borderWidth, y),
                     ImVec2(uv, 0.0f), ImVec2(next, 1.0f));
        uv = next;
        next = uv + sideLength * advance;
        fg->AddImage(texture, ImVec2(x + w, y), ImVec2(x + w + borderWidth, y + h),
                     ImVec2(uv, 0.0f), ImVec2(next, 1.0f));
        uv = next;
        next = uv + topLength * advance;
        fg->AddImage(texture, ImVec2(x - borderWidth, y + h),
                     ImVec2(x + w + borderWidth, y + h + borderWidth),
                     ImVec2(next, 0.0f), ImVec2(uv, 1.0f));
        uv = next;
        next = uv + sideLength * advance;
        fg->AddImage(texture, ImVec2(x - borderWidth, y), ImVec2(x, y + h),
                     ImVec2(next, 0.0f), ImVec2(uv, 1.0f));
    }
    else
    {
        fg->AddRect(min, max, IM_COL32(79, 179, 255, 255), 0.0f, 0, 2.0f);
    }
}

void GBAStationOverlay::RenderGBAStationMenu(ImDrawList *dl, ImVec2 displaySize) {
    EnsureFocusTexture();
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float ease = 1.0f - std::pow(1.0f - std::min(m_animTimer / 0.4f, 1.0f), 3.0f);
    const float width = displaySize.x;
    const float height = displaySize.y;
    const ImVec2 min(0.0f, 0.0f);
    const ImVec2 max(min.x + width, min.y + height);
    const char *tabs[] = {"返回游戏", "保存状态", "读取状态", "金手指", "画面设置", "功能设置", "重置游戏", "退出游戏"};
    const int icons[] = {0xE5C4, 0xE161, 0xE2C6, 0xE3AE, 0xE333, 0xE8B8, 0xE5D5, 0xE879};
    const char *desc[] = {"继续当前游戏。", "创建即时存档。", "读取即时存档。", "管理游戏金手指。", "调整画面比例和缩放。", "调整可即时生效的核心选项。", "重新启动当前游戏。", "返回 GBAStation。"};
    const int active = m_quickMenuSelection;

    // 3DS palette
    const ImU32 white = IM_COL32(240, 247, 255, (int)(255.0f * ease));
    const ImU32 muted = IM_COL32(184, 204, 224, (int)(199.0f * ease));
    const ImU32 cyan = IM_COL32(112, 204, 255, (int)(255.0f * ease));
    const ImU32 focusBg = IM_COL32(0, 77, 128, (int)(133.0f * ease));
    const ImU32 contentFocusBg = IM_COL32(33, 107, 179, (int)(51.0f * ease));
    const ImU32 rowBg = IM_COL32(255, 255, 255, (int)(11.0f * ease));
    const ImU32 rowBorder = IM_COL32(255, 255, 255, (int)(26.0f * ease));
    const ImU32 focusBorder = IM_COL32(79, 179, 255, (int)(128.0f * ease));

    ImFont *font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();

    // Background: vertical gradient strips like the 3DS shell.
    for (int strip = 0; strip < 8; ++strip) {
        const float ft = (float)strip / 7.0f;
        const int r = (int)((20.0f - ft * 8.0f) * ease);
        const int g = (int)((25.0f - ft * 10.0f) * ease);
        const int b = (int)((33.0f - ft * 13.0f) * ease);
        dl->AddRectFilled(ImVec2(0.0f, strip * (height / 8.0f)),
                          ImVec2(width, (strip + 1) * (height / 8.0f)), IM_COL32(r, g, b, 240));
    }

    // Title
    dl->AddText(font, 26.0f * scale, ImVec2(64.0f * scale, 58.0f * scale), white, "游戏菜单");
    dl->AddRectFilled(ImVec2(56.0f * scale, 92.0f * scale),
                      ImVec2(width - 56.0f * scale, 93.0f * scale), IM_COL32(255, 255, 255, (int)(46.0f * ease)));

    // Sidebar
    const float sidebarX = 48.0f * scale;
    const float sidebarY = 116.0f * scale;
    const float sidebarW = 336.0f * scale;
    const float itemH = 58.0f * scale;
    const float step = 64.0f * scale;
    for (int i = 0; i < 8; ++i) {
        const float y = sidebarY + i * step;
        const bool selected = i == active;
        const bool tabFocused = selected && m_sidebarFocused;
        const ImVec2 itemMin(sidebarX, y), itemMax(sidebarX + sidebarW, y + itemH);
        if (selected) {
            dl->AddRectFilled(itemMin, itemMax, tabFocused ? focusBg : contentFocusBg);
            if (tabFocused) {
                DrawFocusBorder(itemMin, itemMax, 3.0f * scale);
            } else {
                dl->AddRect(itemMin, itemMax, focusBorder, 0.0f, 0, 1.0f * scale);
            }
        }
        char iconBuf[8];
        EncodeUtf8(iconBuf, icons[i]);
        const float textY = y + itemH * 0.5f + fontSize * 0.12f * scale;
        dl->AddText(font, 25.0f * scale, ImVec2(sidebarX + 34.0f * scale, y + itemH * 0.5f - 12.5f * scale),
                    selected ? white : muted, iconBuf);
        dl->AddText(font, 21.0f * scale, ImVec2(sidebarX + 64.0f * scale, textY),
                    selected ? white : muted, tabs[i]);
    }
    // Reset separator
    dl->AddRectFilled(ImVec2(sidebarX + 18.0f * scale, sidebarY + 6.0f * step - 9.0f * scale),
                      ImVec2(sidebarX + sidebarW - 18.0f * scale, sidebarY + 6.0f * step - 8.0f * scale),
                      IM_COL32(255, 255, 255, (int)(36.0f * ease)));
    // Divider
    dl->AddRectFilled(ImVec2(404.0f * scale, 110.0f * scale),
                      ImVec2(405.0f * scale, 610.0f * scale), IM_COL32(255, 255, 255, (int)(20.0f * ease)));

    // Content area
    const float contentX = 432.0f * scale;
    const float contentW = 790.0f * scale;
    const float contentRight = contentX + contentW;
    const float viewTop = 176.0f * scale;
    const float viewBottom = 664.0f * scale;
    const float rowH = 48.0f * scale;
    const float rowGap = 4.0f * scale;

    dl->AddText(font, 27.0f * scale, ImVec2(contentX, 150.0f * scale), white, tabs[active]);
    dl->AddRectFilled(ImVec2(contentX, 190.0f * scale),
                      ImVec2(contentX + contentW, 191.0f * scale), IM_COL32(0, 122, 204, (int)(71.0f * ease)));

    auto drawRow = [&](int row, bool focused, const char *iconUtf8, const std::string &label,
                       const std::string &value, bool selector) {
        const float y = viewTop + row * (rowH + rowGap);
        if (y + rowH < viewTop || y > viewBottom) {
            return;
        }
        const ImVec2 rowMin(contentX, y), rowMax(contentX + contentW, y + rowH);
        dl->AddRectFilled(rowMin, rowMax, focused ? focusBg : rowBg);
        if (focused) {
            DrawFocusBorder(rowMin, rowMax, 3.0f * scale);
        } else {
            dl->AddRect(rowMin, rowMax, rowBorder, 0.0f, 0, 1.0f * scale);
        }
        dl->AddText(font, 20.0f * scale, ImVec2(contentX + 24.0f * scale, y + rowH * 0.5f - 10.0f * scale),
                    selector ? cyan : (focused ? white : muted), iconUtf8);
        dl->AddText(font, 20.0f * scale, ImVec2(contentX + 46.0f * scale, y + rowH * 0.5f + 12.0f * scale),
                    focused ? white : muted, label.c_str());
        if (selector) {
            char iconL[8], iconR[8];
            EncodeUtf8(iconL, 0xE0E4);
            EncodeUtf8(iconR, 0xE0E5);
            const float centerY = y + rowH * 0.5f;
            dl->AddText(font, 26.0f * scale, ImVec2(contentX + contentW - 194.0f * scale, centerY - 13.0f * scale),
                        cyan, iconL);
            const float valueW = font->CalcTextSizeA(18.0f * scale, FLT_MAX, 0.0f, value.c_str()).x;
            dl->AddText(font, 18.0f * scale,
                        ImVec2(contentX + contentW - 110.0f * scale - valueW * 0.5f, centerY + 7.0f * scale),
                        cyan, value.c_str());
            dl->AddText(font, 26.0f * scale, ImVec2(contentX + contentW - 24.0f * scale, centerY - 13.0f * scale),
                        cyan, iconR);
        } else {
            const float valueW = font->CalcTextSizeA(18.0f * scale, FLT_MAX, 0.0f, value.c_str()).x;
            dl->AddText(font, 18.0f * scale, ImVec2(contentX + contentW - valueW - 18.0f * scale, y + rowH * 0.5f + 7.0f * scale),
                        cyan, value.c_str());
        }
    };

    const bool inContent = !m_sidebarFocused;
    if (m_currentMenu == OverlayMenu::SaveStates) {
        const int total = 10;
        const int visible = std::min(9, total);
        const int firstSlot = std::clamp(m_saveStateSlot - visible / 2, 0, std::max(0, total - visible));
        for (int row = 0; row < visible; ++row) {
            const int slot = firstSlot + row;
            struct stat st{};
            const bool exists = m_core && stat(GetStatePath(m_core, slot).c_str(), &st) == 0;
            char icon[8];
            EncodeUtf8(icon, 0xE161);
            drawRow(row, inContent && slot == m_saveStateSlot, icon,
                    "存档槽 " + std::to_string(slot + 1), exists ? "已有存档" : "空", false);
        }
    } else if (m_currentMenu == OverlayMenu::Settings) {
        if (active == 5) {
            const char *labels[] = {"跳帧模式", "固定跳帧", "CPU 速度", "低通滤波", "FM 插值", "采样率", "强制 60Hz", "32 位色深"};
            const char *keys[] = {"fbneo-frameskip-type", "fbneo-fixed-frameskip", "fbneo-cpu-speed-adjust", "fbneo-lowpass-filter", "fbneo-fm-interpolation", "fbneo-samplerate", "fbneo-force-60hz", "fbneo-allow-depth-32"};
            const int rowIcons[] = {0xE8E5, 0xE8E5, 0xE896, 0xE8B8, 0xE873, 0xE8F1, 0xE8E5, 0xE873};
            const int total = 8;
            const int visible = std::min(8, total);
            const int first = std::clamp(m_settingsSelection - visible / 2, 0, std::max(0, total - visible));
            for (int row = 0; row < visible; ++row) {
                const int option = first + row;
                std::string value = m_core ? m_core->GetCoreOption(keys[option], "默认") : "默认";
                // samplerate / allow-depth-32 are consumed at game load; the
                // core can't re-apply them live (unlike the frame/pacing ones).
                if (option == 5 || option == 7)
                    value += "（重启后生效）";
                char icon[8];
                EncodeUtf8(icon, rowIcons[option]);
                drawRow(row, inContent && option == m_settingsSelection, icon, labels[option], value,
                        option == 0 || option == 1 || option == 5);
            }
        } else {
            const std::string mode = m_displayMode == GambatteDisplayMode::Integer ? "整数缩放" : "比例显示";
            std::string size = m_displayMode == GambatteDisplayMode::Integer ? "自动" : "原始比例";
            if (m_displaySize == GambatteDisplaySize::_1x) size = "1x"; else if (m_displaySize == GambatteDisplaySize::_2x) size = "2x"; else if (m_displaySize == GambatteDisplaySize::Stretch) size = "拉伸"; else if (m_displaySize == GambatteDisplaySize::_4_3) size = "4:3"; else if (m_displaySize == GambatteDisplaySize::_16_9) size = "16:9";
            char icon[8];
            EncodeUtf8(icon, 0xE8F1);
            drawRow(0, inContent && m_settingsSelection == 0, icon, "显示模式", mode, true);
            EncodeUtf8(icon, 0xE3F4);
            drawRow(1, inContent && m_settingsSelection == 1, icon, "画面比例", size, true);
            EncodeUtf8(icon, 0xE3B6);
            drawRow(2, inContent && m_settingsSelection == 2, icon, "着色器", "切换", false);
        }
    } else {
        dl->AddText(font, 20.0f * scale, ImVec2(contentX, 310.0f * scale),
                    IM_COL32(204, 230, 250, (int)(219.0f * ease)), desc[active]);
    }
}

void GBAStationOverlay::RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    std::string items[] = {"保存状态", "读取状态", "画面设置", "退出游戏"};
    const char* descriptions[] = {
        "写入当前游戏状态到所选槽位。",
        "从所选槽位恢复游戏状态。",
        "调整画面比例、整数缩放和着色器。",
        "关闭街机核心并返回 GBAStation。",
    };
    const int N = 4; float itemH = 70.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 336.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) RenderMenuItem(dl, menuPos, menuSize, i, N, itemH, m_quickMenuSelection==i, cornerRadius, easeOut, m_isDarkMode, font, fs, items[i].c_str());
    dl->AddText(font, ImGui::GetFontSize() * 0.9f, ImVec2(440.0f * scale, 150.0f * scale),
                IM_COL32(238, 247, 255, static_cast<int>(255 * easeOut)),
                items[m_quickMenuSelection].c_str());
    dl->AddText(font, ImGui::GetFontSize() * 0.72f, ImVec2(440.0f * scale, 196.0f * scale),
                IM_COL32(196, 222, 216, static_cast<int>(255 * easeOut)),
                descriptions[m_quickMenuSelection]);
    dl->AddText(font, ImGui::GetFontSize() * 0.68f, ImVec2(440.0f * scale, 258.0f * scale),
                IM_COL32(160, 200, 190, static_cast<int>(220 * easeOut)),
                m_gameTitle.empty() ? "当前游戏: Arcade" : m_gameTitle.c_str());
}

void GBAStationOverlay::RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    const int N = 4; float itemH = 70.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 336.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    bool selectedExists = false;
    for (int i = 0; i < N; i++) {
        bool exists = false;
        if (m_core && m_core->IsGameLoaded()) { struct stat buffer; exists = (stat(GetStatePath(m_core, i).c_str(), &buffer) == 0); }
        char slotText[128];
        snprintf(slotText, sizeof(slotText), "槽位 %d  %s", i + 1, exists ? "已保存" : "空");
        RenderMenuItem(dl, menuPos, menuSize, i, N, itemH, m_saveStateSlot==i, cornerRadius, easeOut, m_isDarkMode, font, fs, slotText);
        if (m_saveStateSlot == i) selectedExists = exists;
    }
    const char* action = m_isSaveMode ? "保存到当前槽位" : "从当前槽位读取";
    const char* status = selectedExists ? "当前槽位已有存档" : "当前槽位为空";
    dl->AddText(font, ImGui::GetFontSize() * 0.9f, ImVec2(440.0f * scale, 150.0f * scale),
                IM_COL32(238, 247, 255, static_cast<int>(255 * easeOut)), action);
    dl->AddText(font, ImGui::GetFontSize() * 0.72f, ImVec2(440.0f * scale, 196.0f * scale),
                IM_COL32(154, 178, 197, static_cast<int>(255 * easeOut)), status);
}

void GBAStationOverlay::RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    const int N = 3; float itemH = 70.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 336.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) {
        bool isSelected = (m_settingsSelection == i);
        float itemY = menuPos.y + i * itemH;
        ImVec2 itemMin(menuPos.x, itemY), itemMax(menuPos.x + menuSize.x, itemY + itemH);
        if (isSelected) {
            dl->AddRectFilled(ImVec2(itemMin.x + 12.0f * scale, itemMin.y),
                              ImVec2(itemMax.x - 12.0f * scale, itemMax.y),
                              IM_COL32(15, 142, 122, static_cast<int>(255 * easeOut)),
                              cornerRadius);
        }
        std::string label, value;
        if (i == 0) { label = "显示模式"; value = (m_displayMode == GambatteDisplayMode::Integer) ? "整数缩放" : "自适应"; }
        else if (i == 1) {
            label = "画面比例";
            if (m_displayMode == GambatteDisplayMode::Integer) {
                switch (m_displaySize) { case GambatteDisplaySize::_1x: value="1x"; break; case GambatteDisplaySize::_2x: value="2x"; break; default: value="自动"; break; }
            } else {
                switch (m_displaySize) { case GambatteDisplaySize::Stretch: value="拉伸"; break; case GambatteDisplaySize::_4_3: value="4:3"; break; case GambatteDisplaySize::_16_9: value="16:9"; break; default: value="原始"; break; }
            }
        } else if (i == 2) {
            label = "着色器";
            const char *shaderNames[] = {"无", "xBRZ", "Eagle", "CRT Easy Mode"};
            value = shaderNames[m_shaderSelection % 4];
        }
        ImU32 textColor;
        textColor = isSelected ? IM_COL32(244,255,252,(int)(255*easeOut)) : IM_COL32(186,214,208,(int)(255*easeOut));
        float textX = itemMin.x + 34.0f * scale;
        ImVec2 labelSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
        dl->AddText(font, fs, ImVec2(textX, itemMin.y + (itemH - labelSize.y)/2), textColor, label.c_str());
        const float valueX = 440.0f * scale;
        const float valueY = 150.0f * scale + i * 62.0f * scale;
        dl->AddText(font, fs, ImVec2(valueX, valueY), isSelected ? IM_COL32(15,142,122,(int)(255*easeOut)) : IM_COL32(238,247,255,(int)(220*easeOut)), label.c_str());
        dl->AddText(font, fs, ImVec2(valueX + 230.0f * scale, valueY), IM_COL32(154,178,197,(int)(255*easeOut)), value.c_str());
        if (isSelected) {
            dl->AddRectFilled(ImVec2(valueX - 16.0f * scale, valueY + 4.0f * scale),
                              ImVec2(valueX - 10.0f * scale, valueY + 24.0f * scale),
                              IM_COL32(15,142,122,(int)(255*easeOut)), 2.0f * scale);
        }
    }
}

void GBAStationOverlay::RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize) {
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    float scale = ImGui::GetIO().FontGlobalScale;
    ImFont *font = ImGui::GetFont();

    // 3DS footer: B and A button hints pinned to the bottom right.
    const char *bLabel = (m_sidebarFocused || m_currentMenu == OverlayMenu::QuickMenu) ? "返回" : "返回列表";
    const char *aLabel = nullptr;
    if (m_currentMenu == OverlayMenu::SaveStates)
        aLabel = m_isSaveMode ? "保存" : "读取";
    else if (m_currentMenu == OverlayMenu::Settings)
        aLabel = "调整";
    else
        aLabel = "确定";

    const ImU32 hintColor = IM_COL32(184, 204, 224, (int)(199.0f * easeOut));
    char iconB[8], iconA[8];
    EncodeUtf8(iconB, 0xE0E1);
    EncodeUtf8(iconA, 0xE0E0);
    const float baseY = displaySize.y - 42.0f * scale;
    dl->AddText(font, 27.0f * scale, ImVec2(1020.0f * scale, baseY - 13.5f * scale), hintColor, iconB);
    dl->AddText(font, 19.0f * scale, ImVec2(1042.0f * scale, baseY + 9.0f * scale), hintColor, bLabel);
    dl->AddText(font, 27.0f * scale, ImVec2(1152.0f * scale, baseY - 13.5f * scale), hintColor, iconA);
    dl->AddText(font, 19.0f * scale, ImVec2(1174.0f * scale, baseY + 9.0f * scale), hintColor, aLabel);
}

bool GBAStationOverlay::HandleInput(SDL_GameController *controller) {
    if (!controller) return false;
    uint32_t now = SDL_GetTicks(); bool debounced = (now - m_lastInputTime) > DEBOUNCE_MS;
    bool guide = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_GUIDE);
    bool togglePressed = guide || BindingPressed(controller, "arcade.hotkey.menu.pad", "PAD_START+PAD_BACK");
    if (togglePressed && !m_toggleHeld && debounced) {
        m_toggleHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::None) Show();
        else if (m_currentMenu == OverlayMenu::SaveStates || m_currentMenu == OverlayMenu::Settings) { m_currentMenu = OverlayMenu::QuickMenu; m_animTimer = 0.4f; }
        else Hide();
        return true;
    }
    if (!togglePressed) m_toggleHeld = false;
    if (m_currentMenu == OverlayMenu::None) return false;

    // The menu always uses physical Switch navigation, independent of arcade
    // gameplay remaps in config.cfg. SDL B is Switch A; SDL A is Switch B.
    bool up = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    bool confirm = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B);
    bool back = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
    Sint16 axisY = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    Sint16 axisX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    if (axisY < -16000) up = true; if (axisY > 16000) down = true;
    if (axisX < -16000) left = true; if (axisX > 16000) right = true;

    // The overlay deliberately uses physical Switch navigation here.  Gameplay
    // remaps stay in the core; the sidebar switches pages immediately and
    // Right is the only way to move into a page's setting list.
    if (m_sidebarFocused) {
        if (up && !m_upHeld && debounced) { m_upHeld = true; m_lastInputTime = now; ActivateTab((m_quickMenuSelection + 7) % 8); GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus); }
        if (!up) m_upHeld = false;
        if (down && !m_downHeld && debounced) { m_downHeld = true; m_lastInputTime = now; ActivateTab((m_quickMenuSelection + 1) % 8); GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus); }
        if (!down) m_downHeld = false;
        if (right && !m_rightHeld && debounced) {
            m_rightHeld = true; m_lastInputTime = now;
            if (m_currentMenu == OverlayMenu::SaveStates || m_currentMenu == OverlayMenu::Settings) {
                m_sidebarFocused = false;
                GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
            }
        }
        if (!right) m_rightHeld = false;
        if (confirm && !m_confirmHeld && debounced) {
            m_confirmHeld = true; m_lastInputTime = now;
            if (m_quickMenuSelection == 0) { Hide(); GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm); }
            else if (m_quickMenuSelection == 6) { m_shouldReset = true; Hide(); GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm); }
            else if (m_quickMenuSelection == 7) { m_shouldExit = true; GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm); }
            else if (m_currentMenu == OverlayMenu::SaveStates || m_currentMenu == OverlayMenu::Settings) {
                m_sidebarFocused = false;
                GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
            }
        }
        if (!confirm) m_confirmHeld = false;
        if (back && !m_backHeld && debounced) { m_backHeld = true; m_lastInputTime = now; Hide(); GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Cancel); }
        if (!back) m_backHeld = false;
        return true;
    }

    if (up && !m_upHeld && debounced) {
        m_upHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) m_quickMenuSelection = (m_quickMenuSelection + 7) % 8;
        else if (m_currentMenu == OverlayMenu::SaveStates) m_saveStateSlot = (m_saveStateSlot + 9) % 10;
        else if (m_currentMenu == OverlayMenu::Settings) m_settingsSelection = (m_settingsSelection + (m_quickMenuSelection == 5 ? 7 : 2)) % (m_quickMenuSelection == 5 ? 8 : 3);
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
    }
    if (!up) m_upHeld = false;
    if (down && !m_downHeld && debounced) {
        m_downHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) m_quickMenuSelection = (m_quickMenuSelection + 1) % 8;
        else if (m_currentMenu == OverlayMenu::SaveStates) m_saveStateSlot = (m_saveStateSlot + 1) % 10;
        else if (m_currentMenu == OverlayMenu::Settings) m_settingsSelection = (m_settingsSelection + 1) % (m_quickMenuSelection == 5 ? 8 : 3);
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
    }
    if (!down) m_downHeld = false;

    if (left && !m_leftHeld && debounced) {
        m_leftHeld = true;
        m_lastInputTime = now;
        m_sidebarFocused = true;
        return true;
    }

    bool dirChanged = false; int dir = 0;
    if (left && !m_leftHeld && debounced) { m_leftHeld = true; dir = -1; dirChanged = true; m_lastInputTime = now; }
    if (!left) m_leftHeld = false;
    if (right && !m_rightHeld && debounced) { m_rightHeld = true; dir = 1; dirChanged = true; m_lastInputTime = now; }
    if (!right) m_rightHeld = false;

    if (dirChanged && m_currentMenu == OverlayMenu::Settings) {
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        if (m_quickMenuSelection == 5) {
            switch (m_settingsSelection) {
            case 0: CycleFBNeoOption(m_core, "fbneo-frameskip-type", {"disabled", "Fixed", "Auto", "Manual"}, dir); break;
            case 1: CycleFBNeoOption(m_core, "fbneo-fixed-frameskip", {"0", "1", "2", "3", "4", "5"}, dir); break;
            case 2: CycleFBNeoOption(m_core, "fbneo-cpu-speed-adjust", {"100%", "110%", "120%", "130%", "140%"}, dir); break;
            case 3: CycleFBNeoOption(m_core, "fbneo-lowpass-filter", {"disabled", "enabled"}, dir); break;
            case 4: CycleFBNeoOption(m_core, "fbneo-fm-interpolation", {"disabled", "4-point 3rd order"}, dir); break;
            case 5: CycleFBNeoOption(m_core, "fbneo-samplerate", {"44100", "48000"}, dir); break;
            case 6: CycleFBNeoOption(m_core, "fbneo-force-60hz", {"disabled", "enabled"}, dir); break;
            case 7: CycleFBNeoOption(m_core, "fbneo-allow-depth-32", {"disabled", "enabled"}, dir); break;
            }
        } else if (m_settingsSelection == 0) {
            m_displayMode = (m_displayMode == GambatteDisplayMode::Display) ? GambatteDisplayMode::Integer : GambatteDisplayMode::Display;
            m_displaySize = (m_displayMode == GambatteDisplayMode::Integer) ? GambatteDisplaySize::Auto : GambatteDisplaySize::_4_3;
            ApplyScalingSettings(true);
        } else if (m_settingsSelection == 1) {
            if (m_displayMode == GambatteDisplayMode::Integer) {
                int s = (int)m_displaySize + dir; if (s < 4) s = 6; if (s > 6) s = 4;
                m_displaySize = (GambatteDisplaySize)s;
            } else {
                int s = (int)m_displaySize + dir; if (s < 0) s = 3; if (s > 3) s = 0;
                m_displaySize = (GambatteDisplaySize)s;
            }
            ApplyScalingSettings(true);
        } else if (m_settingsSelection == 2) {
            m_shaderSelection = (m_shaderSelection + dir + 6) % 6;
            ApplyScalingSettings(true);
        }
    }

    if (confirm && !m_confirmHeld && debounced) {
        m_confirmHeld = true; m_lastInputTime = now;
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm);
        if (m_currentMenu == OverlayMenu::QuickMenu) {
            switch (m_quickMenuSelection) {
            case 0: Hide(); break;
            case 1: m_isSaveMode = true; m_currentMenu = OverlayMenu::SaveStates; break;
            case 2: m_isSaveMode = false; m_currentMenu = OverlayMenu::SaveStates; break;
            case 3:
            case 4:
            case 5: m_currentMenu = OverlayMenu::Settings; m_settingsSelection = 0; break;
            case 6: m_shouldReset = true; Hide(); break;
            case 7: m_shouldExit = true; break;
            }
        } else if (m_currentMenu == OverlayMenu::SaveStates) {
            if (m_core) {
                std::string sp = GetStatePath(m_core, m_saveStateSlot);
                if (m_isSaveMode) { m_core->SaveState(sp); m_currentMenu = OverlayMenu::QuickMenu; m_sidebarFocused = true; }
                else { m_core->LoadState(sp); Hide(); m_animTimer = 0.4f; return true; }
            } else m_currentMenu = OverlayMenu::QuickMenu;
        } else if (m_currentMenu == OverlayMenu::Settings) {
            if (m_quickMenuSelection == 5) {
                switch (m_settingsSelection) {
                case 0: CycleFBNeoOption(m_core, "fbneo-frameskip-type", {"disabled", "Fixed", "Auto", "Manual"}, 1); break;
                case 1: CycleFBNeoOption(m_core, "fbneo-fixed-frameskip", {"0", "1", "2", "3", "4", "5"}, 1); break;
                case 2: CycleFBNeoOption(m_core, "fbneo-cpu-speed-adjust", {"100%", "110%", "120%", "130%", "140%"}, 1); break;
                case 3: CycleFBNeoOption(m_core, "fbneo-lowpass-filter", {"disabled", "enabled"}, 1); break;
                case 4: CycleFBNeoOption(m_core, "fbneo-fm-interpolation", {"disabled", "4-point 3rd order"}, 1); break;
                case 5: CycleFBNeoOption(m_core, "fbneo-samplerate", {"44100", "48000"}, 1); break;
                case 6: CycleFBNeoOption(m_core, "fbneo-force-60hz", {"disabled", "enabled"}, 1); break;
                case 7: CycleFBNeoOption(m_core, "fbneo-allow-depth-32", {"disabled", "enabled"}, 1); break;
                }
            } else if (m_settingsSelection == 0) {
                m_displayMode = (m_displayMode == GambatteDisplayMode::Display) ? GambatteDisplayMode::Integer : GambatteDisplayMode::Display;
                m_displaySize = (m_displayMode == GambatteDisplayMode::Integer) ? GambatteDisplaySize::Auto : GambatteDisplaySize::_4_3;
                ApplyScalingSettings(true);
            } else if (m_settingsSelection == 1) {
                if (m_displayMode == GambatteDisplayMode::Integer) { int s = (int)m_displaySize; s = (s >= 6) ? 4 : s+1; m_displaySize = (GambatteDisplaySize)s; }
                else { int s = (int)m_displaySize; s = (s >= 3) ? 0 : s+1; m_displaySize = (GambatteDisplaySize)s; }
                ApplyScalingSettings(true);
            } else if (m_settingsSelection == 2) {
                m_shaderSelection = (m_shaderSelection + 1) % 6;
                ApplyScalingSettings(true);
            }
        }
    }
    if (!confirm) m_confirmHeld = false;

    if (back && !m_backHeld && debounced) {
        m_backHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) Hide();
        else m_sidebarFocused = true;
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Cancel);
    }
    if (!back) m_backHeld = false;
    return true;
}

void GBAStationOverlay::LoadCoreSettings() {
#ifdef __SWITCH__
    std::string configPath = "sdmc:/GBAStation/config/cores/fbneo.jsonc";
#else
    std::string configPath = "GBAStation/config/cores/fbneo.jsonc";
#endif
    std::ifstream file(configPath);
    if (file.is_open()) {
        auto j = nlohmann::json::parse(file, nullptr, false, true); file.close();
        if (!j.is_discarded()) {
            if (j.contains("display_mode") && j["display_mode"].is_string()) {
                m_displayMode = (j["display_mode"].get<std::string>() == "Integer") ? GambatteDisplayMode::Integer : GambatteDisplayMode::Display;
            } else m_displayMode = GambatteDisplayMode::Integer;
            if (j.contains("display_size") && j["display_size"].is_string()) {
                std::string v = j["display_size"].get<std::string>();
                if (v=="Stretch") m_displaySize = GambatteDisplaySize::Stretch;
                else if (v=="16:9") m_displaySize = GambatteDisplaySize::_16_9;
                else if (v=="Original") m_displaySize = GambatteDisplaySize::Original;
                else if (v=="1x") m_displaySize = GambatteDisplaySize::_1x;
                else if (v=="2x") m_displaySize = GambatteDisplaySize::_2x;
                else if (v=="Auto") m_displaySize = GambatteDisplaySize::Auto;
                else m_displaySize = GambatteDisplaySize::Auto;
            } else m_displaySize = GambatteDisplaySize::Auto;
            if (j.contains("shader_type") && j["shader_type"].is_string()) {
                std::string v = j["shader_type"].get<std::string>();
                if (v=="xBRZ") m_shaderSelection = 1;
                else if (v=="Eagle") m_shaderSelection = 2;
                else if (v=="CrtEasyMode") m_shaderSelection = 3;
                else m_shaderSelection = 0;
            } else m_shaderSelection = 0;
        } else { m_displayMode = GambatteDisplayMode::Integer; m_displaySize = GambatteDisplaySize::Auto; }
    } else { m_displayMode = GambatteDisplayMode::Integer; m_displaySize = GambatteDisplaySize::Auto; }
    const std::string modeOverride = ConfigOverrideValue("core.fbneo.display_mode");
    if (!modeOverride.empty())
        m_displayMode = modeOverride == "Integer" ? GambatteDisplayMode::Integer : GambatteDisplayMode::Display;
    const std::string sizeOverride = ConfigOverrideValue("core.fbneo.display_size");
    if (!sizeOverride.empty()) {
        if (sizeOverride=="Stretch") m_displaySize = GambatteDisplaySize::Stretch;
        else if (sizeOverride=="4:3") m_displaySize = GambatteDisplaySize::_4_3;
        else if (sizeOverride=="16:9") m_displaySize = GambatteDisplaySize::_16_9;
        else if (sizeOverride=="Original") m_displaySize = GambatteDisplaySize::Original;
        else if (sizeOverride=="1x") m_displaySize = GambatteDisplaySize::_1x;
        else if (sizeOverride=="2x") m_displaySize = GambatteDisplaySize::_2x;
        else if (sizeOverride=="Auto") m_displaySize = GambatteDisplaySize::Auto;
    }
    const std::string shaderOverride = ConfigOverrideValue("core.fbneo.shader_type");
    if (!shaderOverride.empty()) {
        if (shaderOverride=="xBRZ") m_shaderSelection = 1;
        else if (shaderOverride=="Eagle") m_shaderSelection = 2;
        else if (shaderOverride=="CrtEasyMode") m_shaderSelection = 3;
        else m_shaderSelection = 0;
    }
    ApplyScalingSettings(false);
}

void GBAStationOverlay::SaveCoreSettings() {
#ifdef __SWITCH__
    std::string configPath = "sdmc:/GBAStation/config/cores/fbneo.jsonc";
#else
    std::string configPath = "GBAStation/config/cores/fbneo.jsonc";
#endif
    nlohmann::json j = nlohmann::json::object();
    { std::ifstream in(configPath); if (in.is_open()) { auto p = nlohmann::json::parse(in, nullptr, false, true); in.close(); if (!p.is_discarded()) j = p; } }
    j["display_mode"] = (m_displayMode == GambatteDisplayMode::Integer) ? "Integer" : "Display";
    const char *sizeStr = "4:3";
    switch (m_displaySize) {
    case GambatteDisplaySize::Stretch: sizeStr="Stretch"; break; case GambatteDisplaySize::_4_3: sizeStr="4:3"; break;
    case GambatteDisplaySize::_16_9: sizeStr="16:9"; break; case GambatteDisplaySize::Original: sizeStr="Original"; break;
    case GambatteDisplaySize::_1x: sizeStr="1x"; break; case GambatteDisplaySize::_2x: sizeStr="2x"; break;
    case GambatteDisplaySize::Auto: sizeStr="Auto"; break; default: break;
    }
    j["display_size"] = sizeStr;
    const char *shaderStr = "None";
    switch (m_shaderSelection) {
    case 1: shaderStr = "xBRZ"; break;
    case 2: shaderStr = "Eagle"; break;
    case 3: shaderStr = "CrtEasyMode"; break;
    default: shaderStr = "None"; break;
    }
    j["shader_type"] = shaderStr;
    std::ofstream out(configPath); if (out.is_open()) { out << j.dump(4); out.close(); }
}

void GBAStationOverlay::ApplyScalingSettings(bool save) { if (save) SaveCoreSettings(); }

void GBAStationOverlay::LoadSVGIcon() {
    const char *svgContent = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 448 512"><path fill="#FFFFFF" d="M338.8-9.9c11.9 8.6 16.3 24.2 10.9 37.8L271.3 224 416 224c13.5 0 25.5 8.4 30.1 21.1s.7 26.9-9.6 35.5l-288 240c-11.3 9.4-27.4 9.9-39.3 1.3s-16.3-24.2-10.9-37.8L176.7 288 32 288c-13.5 0-25.5-8.4-30.1-21.1s-.7-26.9 9.6-35.5l288-240c11.3-9.4 27.4-9.9 39.3-1.3z"/></svg>)";
    char *input = strdup(svgContent); if (!input) return;
    NSVGimage *image = nsvgParse(input, "px", 96); free(input); if (!image) return;
    float sc = 64.0f / image->height; int w = (int)(image->width * sc), h = (int)(image->height * sc);
    m_boltWidth = w; m_boltHeight = h;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return; }
    unsigned char *img = (unsigned char *)malloc(w * h * 4);
    if (!img) { nsvgDeleteRasterizer(rast); nsvgDelete(image); return; }
    nsvgRasterize(rast, image, 0, 0, sc, img, w, h, w * 4);
    if (m_boltTexture != 0) glDeleteTextures(1, &m_boltTexture);
    glGenTextures(1, &m_boltTexture); glBindTexture(GL_TEXTURE_2D, m_boltTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(img); nsvgDeleteRasterizer(rast); nsvgDelete(image);
}

void GBAStationOverlay::RenderStatusBar(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f), alpha = ease;
    float scale = ImGui::GetIO().FontGlobalScale;
    float BAR_HEIGHT = 50.0f*scale, TOP_MARGIN = 32.0f*scale, SIDE_MARGIN = 32.0f*scale, ITEM_SPACING = 20.0f*scale;
    ImFont *font = ImGui::GetFont(); float fontSize = ImGui::GetFontSize();
    std::time_t now = std::time(nullptr); std::tm *lt = std::localtime(&now);
    char timeStr[16], periodStr[16]; bool is24h = (m_hourFormat == "24h");
    float timeW = 0, periodFontSize = fontSize * 0.55f, periodW = 0;
    if (is24h) { std::strftime(timeStr, sizeof(timeStr), "%H:%M", lt); timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x; periodStr[0]=0; }
    else { std::strftime(timeStr, sizeof(timeStr), "%I:%M", lt); std::strftime(periodStr, sizeof(periodStr), "%p", lt); timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x; periodW = font->CalcTextSizeA(periodFontSize, FLT_MAX, 0.0f, periodStr).x; }
    float totalWidth = timeW; if (!is24h) totalWidth += 4.0f + periodW;
    totalWidth += ITEM_SPACING + 34.0f*scale; float PADDING = 20.0f*scale; totalWidth += PADDING*2;
    float offsetY = (1.0f - ease) * -20.0f;
    float barX = displaySize.x - totalWidth - SIDE_MARGIN, barY = TOP_MARGIN + offsetY;
    ImU32 textColor = IM_COL32(200,200,200,(int)(255*alpha));
    float cursorX = barX + PADDING, centerY = barY + BAR_HEIGHT * 0.5f;
    dl->AddText(font, fontSize, ImVec2(cursorX, centerY - fontSize*0.5f), textColor, timeStr); cursorX += timeW;
    if (!is24h) { cursorX += 4.0f*scale; dl->AddText(font, periodFontSize, ImVec2(cursorX, centerY - fontSize*0.5f + (fontSize-periodFontSize)*0.9f), textColor, periodStr); cursorX += periodW; }
    cursorX += ITEM_SPACING;
    float bodyW = 32.0f*scale, bodyH = 20.0f*scale, tipW = 4.0f*scale, tipH = 10.0f*scale;
    ImVec2 batteryPos(cursorX, centerY - bodyH*0.5f);
    ImVec2 bodyMin = batteryPos, bodyMax = bodyMin + ImVec2(bodyW, bodyH);
    dl->AddRect(bodyMin, bodyMax, textColor, 3.0f, 0, 2.0f);
    ImVec2 tipMin(bodyMax.x, batteryPos.y + (bodyH-tipH)*0.5f), tipMax = tipMin + ImVec2(tipW, tipH);
    dl->AddRectFilled(tipMin, tipMax, textColor, 2.0f, ImDrawFlags_RoundCornersRight);
    float pct = std::clamp(m_batteryLevel / 100.0f, 0.0f, 1.0f);
    float pad = 4.0f*scale, fillMaxW = bodyW - pad*2, currentFillW = fillMaxW * pct;
    if (currentFillW < 2.0f*scale && pct > 0) currentFillW = 2.0f*scale;
    if (currentFillW > 0) dl->AddRectFilled(bodyMin + ImVec2(pad,pad), bodyMin + ImVec2(pad+currentFillW, bodyH-pad), textColor, 1.0f);
    if (m_isCharging) {
        if (m_boltTexture == 0) LoadSVGIcon();
        if (m_boltTexture != 0) {
            float iconH = 16.0f*scale, iconW = iconH * ((float)m_boltWidth / (float)m_boltHeight);
            ImVec2 iconPos(tipMax.x + 6.0f*scale, batteryPos.y + (bodyH - iconH)*0.5f);
            float fadeProgress = std::max(0.0f, (m_chargingStateProgress - 0.5f) * 2.0f);
            int alphaBolt = (int)(255 * fadeProgress * ease);
            if (alphaBolt > 0) dl->AddImage((ImTextureID)(intptr_t)m_boltTexture, iconPos, iconPos + ImVec2(iconW,iconH), ImVec2(0,0), ImVec2(1,1), IM_COL32(235,235,235,alphaBolt));
        }
    }
}
