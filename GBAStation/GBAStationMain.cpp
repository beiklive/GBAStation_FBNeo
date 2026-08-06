/// @file GBAStationMain.cpp
/// @brief Entry point for GBAStation-integrated fbneo NRO
/// Sets up SDL/EGL/ImGui and runs the main loop

#include "GBAStationCore.h"
#include "GBAStationOverlay.h"
#include "GBAStationConfig.h"
#include "GBAStationAudio.h"
#include "GBAStationTranslationManager.h"

#include <SDL.h>
#include <memory>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "GBAStationUtils.h"
#include "GBAStationLogger.h"

#ifdef __SWITCH__
#include <switch.h>
#include <curl/curl.h>
#include "glad.h"
#include <EGL/egl.h>
#endif

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

//==============================================================================
// NX System Configuration (extern "C")
//==============================================================================

extern "C" {
u32 __NvOptimusEnablement = 1;
u32 __NvDeveloperOption = 1;
u32 __nx_applet_type = AppletType_Application;
u32 __nx_applet_exit_mode = 0; // 0 = standard exit (return to Homebrew ABI loader if NRO). 1 = forceful applet exit
size_t __nx_heap_size = 0;
}

//==============================================================================
// Globals
//==============================================================================

static SDL_Window *g_window = nullptr;
#ifndef __SWITCH__
static SDL_GLContext g_glContext = nullptr;
#endif
static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static EGLContext g_eglContext = EGL_NO_CONTEXT;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;

static std::unique_ptr<GBAStationCore> g_core;
static std::unique_ptr<GBAStationOverlay> g_overlay;

static bool g_running = true;
static bool g_exitToSystem = false;
static std::string g_returnNroPath = "sdmc:/switch/GBAStation.nro";
static std::string g_externalSessionToken;
static GBAStationAudio g_audio;
static SDL_AudioDeviceID g_audioDevice = 0;
static SDL_GameController *g_controllers[4] = {nullptr, nullptr, nullptr, nullptr};
static bool g_controllersDirty = true;
static std::unordered_map<std::string, std::string> g_configValues;

#ifdef __SWITCH__
static const ImWchar kGBAStationMaterialRanges[] = {
    0xE000, 0xF8FF,
    0,
};

// ABXY / L / R key-glyph block of the Nintendo shared font.  These codepoints
// overlap the Material Icons private-use area, and ImGui keeps the glyph of
// whichever font is packed first — so this range must be registered on the
// base (shared) fonts, which are always added before the Material font.
static const ImWchar kGBAStationNintendoKeyRanges[] = {
    0xE0E0, 0xE0E5,
    0,
};

static const ImWchar *GetGBAStationMenuGlyphRanges(ImGuiIO &io)
{
    static ImVector<ImWchar> ranges;
    if (!ranges.empty()) return ranges.Data;

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(kGBAStationNintendoKeyRanges);
    // Use the complete system Chinese range.  The common range leaves valid
    // core option labels rendered as question marks in the overlay.
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
    builder.AddText(u8"返回游戏 保存状态 读取状态 金手指 画面设置 功能设置 重置游戏 退出游戏 核心设置 按键映射 "
                    u8"视频 音频 输入 性能 系统 语言 区域 纹理 过滤 开启 关闭 自动 取消 确认 保存 读取 "
                    u8"模拟器 菜单 游戏 暂停 快进 截图 预览 存档 槽位 选项 分类 默认 当前");
    builder.BuildRanges(&ranges);
    return ranges.Data;
}

static ImFont *AddGBAStationSystemFont(ImGuiIO &io, float size)
{
    if (R_FAILED(plInitialize(PlServiceType_User)))
    {
        LOG_WARN("HOME", "Switch shared font service is unavailable");
        return nullptr;
    }

    // Match the GBAStation 3DS overlay: the extended Chinese face is the
    // primary font, with the regional and standard faces filling its gaps.
    const PlSharedFontType fontTypes[] = {
        PlSharedFontType_ExtChineseSimplified,
        PlSharedFontType_ChineseSimplified,
        PlSharedFontType_Standard,
    };
    ImFont *font = nullptr;
    int loadedFonts = 0;
    for (const PlSharedFontType type : fontTypes)
    {
        PlFontData sharedFont{};
        if (R_FAILED(plGetSharedFontByType(&sharedFont, type)) || !sharedFont.address || sharedFont.size == 0)
            continue;

        void *fontData = std::malloc(sharedFont.size);
        if (!fontData) continue;
        std::memcpy(fontData, sharedFont.address, sharedFont.size);

        ImFontConfig config;
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.MergeMode = font != nullptr;
        ImFont *added = io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(sharedFont.size),
                                                         size, &config, GetGBAStationMenuGlyphRanges(io));
        if (!added)
        {
            std::free(fontData);
            continue;
        }
        if (!font) font = added;
        ++loadedFonts;
    }
    // The Nintendo shared font carries the ABXY / L / R key glyphs used by the
    // 3DS-style menu chrome and its LR value selectors.
    PlFontData nintendoFont{};
    if (R_SUCCEEDED(plGetSharedFontByType(&nintendoFont, PlSharedFontType_NintendoExt)) &&
        nintendoFont.address && nintendoFont.size > 0)
    {
        void *fontData = std::malloc(nintendoFont.size);
        if (fontData)
        {
            std::memcpy(fontData, nintendoFont.address, nintendoFont.size);
            ImFontConfig config;
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.MergeMode = true;
            if (io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(nintendoFont.size),
                                               size, &config, GetGBAStationMenuGlyphRanges(io)))
            {
                ++loadedFonts;
            }
            else
            {
                std::free(fontData);
            }
        }
    }
    plExit();
    if (!font)
    {
        LOG_ERROR("HOME", "No usable Switch shared font was loaded");
        return nullptr;
    }
    io.FontDefault = font;

    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    io.Fonts->AddFontFromFileTTF("romfs:/fonts/MaterialIcons-Regular.ttf", size,
                                 &iconConfig, kGBAStationMaterialRanges);
    LOG_INFO("HOME", "Switch menu font atlas loaded from %d shared font source(s), Material Icons=%s",
             loadedFonts, io.Fonts->Fonts.Size > 0 ? "merged" : "unavailable");
    return font;
}

static void ApplyGBAStationMenuStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.075f, 0.090f, 0.97f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.070f, 0.105f, 0.120f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.050f, 0.420f, 0.390f, 1.00f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.070f, 0.520f, 0.480f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.050f, 0.420f, 0.390f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.070f, 0.520f, 0.480f, 1.00f);
}
#endif

#ifdef __SWITCH__
static u8 g_lastOperationMode = 255;

static void ApplySwitchPerformanceProfile()
{
    Result rcNormal = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220007);
    Result rcBoost = apmSetPerformanceConfiguration(ApmPerformanceMode_Boost, 0x92220008);
    if (R_FAILED(rcNormal) || R_FAILED(rcBoost))
    {
        LOG_WARN("HOME", "Switch performance profile failed (normal=0x%x boost=0x%x)", rcNormal, rcBoost);
    }
    else
    {
        LOG_INFO("HOME", "Applied Switch performance profile");
    }
}

static void PinCurrentThreadToCore(int core, const char *label)
{
    if (core < 0 || core > 2)
        return;

    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_FAILED(rc))
    {
        LOG_WARN("HOME", "Failed to pin %s thread to core %d (rc=0x%x)", label, core, rc);
    }
    else
    {
        LOG_INFO("HOME", "Pinned %s thread to core %d", label, core);
    }
}

static bool UpdateScreenMode()
{
    u8 operationMode = appletGetOperationMode();
    if (operationMode == g_lastOperationMode)
        return false;

    if (operationMode == AppletOperationMode_Handheld)
    {
        nwindowSetCrop(nwindowGetDefault(), 0, 360, 1280, 1080);
        LOG_INFO("DISPLAY", "Mode → Handheld (1280×720 crop)");
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().FontGlobalScale = 1.0f;
        }
    }
    else
    {
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
        LOG_INFO("DISPLAY", "Mode → Docked (1920×1080)");
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().FontGlobalScale = 1.5f;
        }
    }
    g_lastOperationMode = operationMode;
    return true;
}
#endif

static std::string Trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

static std::string DecodeConfigValue(std::string_view encoded)
{
    std::string value = Trim(encoded);
    if (value.size() > 2 && value[1] == '|' && value[0] == 's')
    {
        value.erase(0, 2);
        std::string decoded;
        decoded.reserve(value.size());
        bool escaped = false;
        for (char c : value)
        {
            if (escaped)
            {
                decoded.push_back(c);
                escaped = false;
            }
            else if (c == '\\')
                escaped = true;
            else
                decoded.push_back(c);
        }
        if (escaped)
            decoded.push_back('\\');
        return decoded;
    }
    return value;
}

static void LoadGBAStationConfig()
{
    g_configValues.clear();
    const char *paths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
    for (const char *path : paths)
    {
        std::ifstream in(path);
        if (!in)
            continue;
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t equal = line.find('=');
            if (equal == std::string::npos)
                continue;
            g_configValues[Trim(std::string_view(line).substr(0, equal))] =
                DecodeConfigValue(std::string_view(line).substr(equal + 1));
        }
        LOG_INFO("INPUT", "Loaded GBAStation config: %s", path);
        break;
    }
}

static bool TokenPressed(SDL_GameController *controller, std::string_view token)
{
    const std::string t = Trim(token);
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
    if (t == "PAD_LT" || t == "PAD_ZL")
        return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
    if (t == "PAD_RT" || t == "PAD_ZR")
        return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
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

static bool ComboPressed(SDL_GameController *controller, std::string_view combo)
{
    const std::string value = Trim(combo);
    if (value.empty() || value == "none")
        return false;
    std::size_t begin = 0;
    bool any = false;
    while (begin < value.size())
    {
        const std::size_t end = value.find('+', begin);
        const std::string_view token = std::string_view(value).substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin);
        if (!TokenPressed(controller, token))
            return false;
        any = true;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return any;
}

static bool BindingPressed(SDL_GameController *controller, const char *key, const char *fallback)
{
    const auto it = g_configValues.find(key);
    const std::string value = it == g_configValues.end() ? fallback : it->second;
    std::size_t begin = 0;
    while (begin <= value.size())
    {
        const std::size_t end = value.find('|', begin);
        const std::string_view combo = std::string_view(value).substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin);
        if (ComboPressed(controller, combo))
            return true;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}

static bool EndsWithNoCase(const std::string &text, const char *suffix)
{
    const std::size_t suffixLen = std::strlen(suffix);
    if (suffixLen > text.size())
        return false;
    return std::equal(suffix, suffix + suffixLen, text.end() - suffixLen,
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

//==============================================================================
// SDL/EGL Initialization
//==============================================================================

static void CloseControllers()
{
    for (SDL_GameController *&controller : g_controllers)
    {
        if (controller)
        {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
    }
}

static void RefreshControllers()
{
    CloseControllers();

    int controllerIndex = 0;
    int joystickCount = SDL_NumJoysticks();

    for (int i = 0; i < joystickCount && controllerIndex < 4; ++i)
    {
        if (!SDL_IsGameController(i))
            continue;

        SDL_GameController *controller = SDL_GameControllerOpen(i);
        if (!controller)
        {
            LOG_WARN("INPUT", "Failed to open controller %d: %s", i, SDL_GetError());
            continue;
        }

        g_controllers[controllerIndex++] = controller;
    }

    g_controllersDirty = false;
}

static void GetDisplayResolution(int &w, int &h)
{
#ifdef __SWITCH__
    u8 opMode = appletGetOperationMode();
    if (opMode == AppletOperationMode_Handheld)
    {
        w = 1280;
        h = 720;
    }
    else
    {
        w = 1920;
        h = 1080;
    }
#else
    if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    else
    {
        w = 1280;
        h = 720;
    }
#endif
}

bool InitWindow()
{
    LOG_INFO("HOME", "Starting initialization...");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
                 SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    {
        LOG_ERROR("HOME", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("HOME", "SDL initialized");

#ifdef __SWITCH__
    g_window = nullptr;
    LOG_INFO("HOME", "Switch: skipping SDL window (using native window)");

    UpdateScreenMode();
    int w, h;
    GetDisplayResolution(w, h);
    LOG_INFO("HOME", "Switch Resolution: %dx%d (logical)", w, h);

    // Initialize EGL
    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_eglDisplay == EGL_NO_DISPLAY)
    {
        LOG_ERROR("EGL", "eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(g_eglDisplay, &major, &minor))
    {
        LOG_ERROR("EGL", "eglInitialize failed");
        return false;
    }
    LOG_INFO("EGL", "EGL %d.%d initialized", major, minor);

    EGLConfig config;
    EGLint numConfigs;
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE};

    if (!eglChooseConfig(g_eglDisplay, configAttribs, &config, 1, &numConfigs))
    {
        LOG_ERROR("EGL", "eglChooseConfig failed");
        return false;
    }

    g_eglSurface = eglCreateWindowSurface(g_eglDisplay, config,
                                          nwindowGetDefault(), NULL);
    if (g_eglSurface == EGL_NO_SURFACE)
    {
        LOG_ERROR("EGL", "eglCreateWindowSurface failed");
        return false;
    }

    eglBindAPI(EGL_OPENGL_API);
    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};

    g_eglContext = eglCreateContext(g_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_eglContext == EGL_NO_CONTEXT)
    {
        LOG_ERROR("EGL", "eglCreateContext failed");
        return false;
    }

    if (!eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext))
    {
        LOG_ERROR("EGL", "eglMakeCurrent failed");
        return false;
    }

    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress))
    {
        LOG_ERROR("HOME", "gladLoadGLLoader failed");
        return false;
    }

    eglSwapInterval(g_eglDisplay, 0);
    LOG_INFO("EGL", "VSync disabled (eglSwapInterval=0) — manual pacing keyed to core fps");

    LOG_INFO("HOME", "OpenGL %s initialized", glGetString(GL_VERSION));

#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    g_window = SDL_CreateWindow("fbneo",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                GBAStationConfig::WINDOW_WIDTH, GBAStationConfig::WINDOW_HEIGHT,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

    if (!g_window)
    {
        LOG_ERROR("HOME", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    g_glContext = SDL_GL_CreateContext(g_window);
    if (!g_glContext)
    {
        LOG_ERROR("HOME", "SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(g_window, g_glContext);
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        LOG_ERROR("HOME", "gladLoadGLLoader failed");
        return false;
    }

    LOG_INFO("HOME", "OpenGL %s initialized", glGetString(GL_VERSION));
#endif

    if (GBAStationConfig::USE_SDLQUEUEAUDIO)
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = GBAStationAudio::SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = GBAStationAudio::CHANNELS;
        want.samples = 2048;
        want.callback = NULL;

        g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audioDevice == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_QueueAudio initialized. DeviceID: %d, Freq: %d", g_audioDevice, have.freq);
        }
    }
    else
    {
        if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 1024) < 0)
        {
            LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_mixer initialized");
        }
    }

    return true;
}

static void AudioSampleCallback(int16_t left, int16_t right)
{
    g_audio.PushSample(left, right);
}

static size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return g_audio.PushSamples(data, frames);
}

static void AudioFlushCallback()
{
    g_audio.Flush();
    LOG_INFO("AUDIO", "Audio flushed");
}

bool InitImGui()
{
    LOG_INFO("HOME", "InitImGui starting...");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    LOG_INFO("HOME", "ImGui context created");

#ifdef __SWITCH__
    ImGui_ImplSDL2_InitForOpenGL(g_window, nullptr);
    ImGui_ImplOpenGL3_Init("#version 430 core");
#else
    ImGui_ImplSDL2_InitForOpenGL(g_window, g_glContext);
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
    LOG_INFO("HOME", "ImGui backends initialized");

#ifdef __SWITCH__
    ImFontConfig fontCfg;
    fontCfg.SizePixels = GBAStationConfig::FONT_SIZE;
    if (AddGBAStationSystemFont(io, GBAStationConfig::FONT_SIZE))
    {
        LOG_INFO("HOME", "Loaded Switch simplified-Chinese system font and Material Icons");
    }
    else if (io.Fonts->AddFontFromFileTTF(GBAStationConfig::FONT_PATH, GBAStationConfig::FONT_SIZE,
                                          &fontCfg, io.Fonts->GetGlyphRangesChineseFull()))
    {
        LOG_WARN("HOME", "Switch shared font unavailable; using ROMFS Chinese fallback");
    }
    else if (!io.Fonts->AddFontDefault(&fontCfg))
    {
        LOG_ERROR("HOME", "Failed to load font from romfs and built-in ImGui fallback");
        return false;
    }
    else
    {
        LOG_WARN("HOME", "Failed to load %s, using built-in ImGui font", GBAStationConfig::FONT_PATH);
    }
    
    ApplyGBAStationMenuStyle();
#else
    if (!io.Fonts->AddFontFromFileTTF("assets/fonts/font.ttf", GBAStationConfig::FONT_SIZE))
    {
        LOG_ERROR("HOME", "Failed to load ImGui font from assets/fonts/font.ttf");
        return false;
    }
    // Load secondary font for RA alert descriptions
    io.Fonts->AddFontFromFileTTF("assets/fonts/description.ttf", GBAStationConfig::FONT_SIZE * 0.75f);
#endif

    LOG_INFO("HOME", "ImGui initialized");
    return true;
}

void CleanupWindow()
{
    CloseControllers();

    glFinish();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

#ifdef __SWITCH__
    if (g_eglContext != EGL_NO_CONTEXT)
    {
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_eglDisplay, g_eglContext);
    }
    if (g_eglSurface != EGL_NO_SURFACE)
    {
        eglDestroySurface(g_eglDisplay, g_eglSurface);
    }
    if (g_eglDisplay != EGL_NO_DISPLAY)
    {
        eglTerminate(g_eglDisplay);
    }

    eglReleaseThread();
#else
    if (g_glContext)
    {
        SDL_GL_DeleteContext(g_glContext);
    }
#endif

    if (g_window)
    {
        SDL_DestroyWindow(g_window);
    }

    SDL_Quit();
}

//==============================================================================
// Main Loop
//==============================================================================

void ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        ImGui_ImplSDL2_ProcessEvent(&event);

        if (event.type == SDL_QUIT)
        {
            LOG_INFO("HOME", "Received SDL_QUIT event");
            g_running = false;
        }

        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_CONTROLLERDEVICEREMOVED ||
            event.type == SDL_JOYDEVICEADDED ||
            event.type == SDL_JOYDEVICEREMOVED)
        {
            g_controllersDirty = true;
        }

#ifdef __SWITCH__
        if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
        {
            LOG_INFO("HOME", "Received Escape key event, requesting exit");
            g_running = false;
        }
#endif
    }
}

void HandleInput()
{
    SDL_GameController *controllers[4] = {nullptr, nullptr, nullptr, nullptr};
    int numControllers = 0;

    if (g_controllersDirty)
    {
        RefreshControllers();
    }

    for (int i = 0; i < 4; ++i)
    {
        if (g_controllers[i])
            controllers[numControllers++] = g_controllers[i];
    }


    if (g_overlay && numControllers > 0 && g_overlay->HandleInput(controllers[0]))
    {
        if (g_overlay->ShouldExitToSystem())
        {
            LOG_INFO("HOME", "ExitToSystem: terminating process");
            remove("imgui.ini");
            g_exitToSystem = true;
            g_running = false;
        }
        if (g_overlay->ShouldExit())
        {
            LOG_INFO("HOME", "ShouldExit detected! g_running will be false.");
#ifdef __SWITCH__
            const char *primaryNro = g_returnNroPath.empty() ? "sdmc:/switch/GBAStation.nro" : g_returnNroPath.c_str();
            const char *fallbackNro = "sdmc:/GBAStation/GBAStation.nro";
            const char *targetNro = nullptr;

            // Check if primaryNro exists, else check fallbackNro
            struct stat buffer;
            if (stat(primaryNro, &buffer) == 0)
            {
                targetNro = primaryNro;
            }
            else if (stat(fallbackNro, &buffer) == 0)
            {
                targetNro = fallbackNro;
            }

            if (targetNro != nullptr)
            {
                char args[512];
                if (!g_externalSessionToken.empty())
                    snprintf(args, sizeof(args), "%s --external-return %s", targetNro, g_externalSessionToken.c_str());
                else
                    snprintf(args, sizeof(args), "%s --resume", targetNro);

                envSetNextLoad(targetNro, args);
                LOG_INFO("HOME", "Chainloading back to %s with args: %s", targetNro, args);
            }
            else
            {
                LOG_WARN("HOME", "Chainload target not found! Exiting normally.");
            }

            // Clean up imgui.ini to avoid clutter/persistence issues
            remove("imgui.ini");
            LOG_INFO("HOME", "Deleted imgui.ini");
#endif
            g_running = false;
        }
        if (g_overlay->ShouldReset())
        {
            g_overlay->ClearReset();
            if (g_core)
            {
                g_core->Reset();
            }
        }
        return;
    }

    if (g_core)
    {
        g_core->ClearInputs();

        for (int p = 0; p < numControllers; p++)
        {
            SDL_GameController *controller = controllers[p];
            if (!controller) continue;

            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_A,
                                  BindingPressed(controller, "arcade.handle.a", "PAD_A"));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_B,
                                  BindingPressed(controller, "arcade.handle.b", "PAD_B"));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_X,
                                  BindingPressed(controller, "arcade.handle.x", "PAD_X"));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_Y,
                                  BindingPressed(controller, "arcade.handle.y", "PAD_Y"));

            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_START,
                                  BindingPressed(controller, "arcade.handle.start", "PAD_START"));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_SELECT,
                                  BindingPressed(controller, "arcade.handle.select", "PAD_BACK"));

            bool dpadUp = BindingPressed(controller, "arcade.handle.up", "PAD_UP");
            bool dpadDown = BindingPressed(controller, "arcade.handle.down", "PAD_DOWN");
            bool dpadLeft = BindingPressed(controller, "arcade.handle.left", "PAD_LEFT");
            bool dpadRight = BindingPressed(controller, "arcade.handle.right", "PAD_RIGHT");

            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_UP, dpadUp);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_DOWN, dpadDown);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_LEFT, dpadLeft);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_RIGHT, dpadRight);

            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L,
                                  BindingPressed(controller, "arcade.handle.l", "PAD_LB"));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R,
                                  BindingPressed(controller, "arcade.handle.r", "PAD_RB"));
            
            bool zl = BindingPressed(controller, "arcade.handle.l2", "PAD_LT");
            bool zr = BindingPressed(controller, "arcade.handle.r2", "PAD_RT");

            if (p == 0)
            {
                g_audio.SetFastForward(
                    BindingPressed(controller, "arcade.handle.fastforward", "PAD_LSB"));
            }

            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L2, zl);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R2, zr);

            // Left stick -> RetroPad Analog Left
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
            // Right stick -> RetroPad Analog Right
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
        }
    }
}

void Render()
{
    static int frameCount = 0;
    frameCount++;

    if (frameCount <= 3)
    {
        LOG_DEBUG("RENDER", "Frame %d: Render starting", frameCount);
    }

    ImGui_ImplOpenGL3_NewFrame();

#ifdef __SWITCH__
    UpdateScreenMode();

    ImGuiIO &io = ImGui::GetIO();
    int logW, logH;
    GetDisplayResolution(logW, logH);
    io.DisplaySize = ImVec2((float)logW, (float)logH);
    io.DeltaTime = 1.0f / 60.0f;
#else
    ImGui_ImplSDL2_NewFrame();
#endif
    ImGui::NewFrame();

    int w, h;
    GetDisplayResolution(w, h);
    ImVec2 displaySize((float)w, (float)h);

    if (g_core)
    {
        bool overlayVisible = g_overlay && g_overlay->IsVisible();

        if (!overlayVisible)
        {
            if (frameCount <= 3)
            {
                LOG_DEBUG("RENDER", "Frame %d: Calling RunFrame", frameCount);
            }
            g_core->RunFrame();
            if (frameCount <= 3)
            {
                LOG_DEBUG("RENDER", "Frame %d: RunFrame returned", frameCount);
            }
        }
        else
        {
            g_core->ApplyPendingOptions();
        }
    }

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (g_overlay)
    {
        unsigned int tex = g_core ? g_core->GetFrameTextureID() : 0;
        float ar = g_core ? g_core->GetAspectRatio() : 4.0f / 3.0f;
        int fw = g_core ? g_core->GetFrameWidth() : 640;
        int fh = g_core ? g_core->GetFrameHeight() : 480;
        int fboW = g_core ? g_core->GetFBOWidth() : 0;
        int fboH = g_core ? g_core->GetFBOHeight() : 0;

        g_overlay->Render(displaySize, tex, ar, fw, fh, fboW, fboH);

        // Sync shader selection from overlay to core
        if (g_core && g_overlay)
        {
            ShaderType desired = static_cast<ShaderType>(g_overlay->GetShaderSelection());
            if (desired != g_core->GetShader())
                g_core->SetShader(desired);
        }
    }
    
    if (g_core && g_core->GetOSDFrames() > 0)
    {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const float marginX = 24.0f;
        const float marginY = 16.0f;
        const float padX = 16.0f;
        const float padY = 8.0f;
        const float rounding = 14.0f;
        
        int frames = g_core->GetOSDFrames();
        float alpha = 1.0f;
        if (frames < 30) alpha = frames / 30.0f;
        
        std::string msg = g_core->GetOSDMessage();
        ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());
        
        float pillW = textSize.x + padX * 2;
        float pillH = textSize.y + padY * 2;
        float pillX = marginX;
        float pillY = marginY;
        
        ImU32 bgCol = IM_COL32(0, 0, 0, (int)(alpha * 153));
        fg->AddRectFilled(ImVec2(pillX, pillY), ImVec2(pillX + pillW, pillY + pillH), bgCol, rounding);
        
        ImU32 textCol = IM_COL32(255, 255, 255, (int)(alpha * 240));
        fg->AddText(ImVec2(pillX + padX, pillY + padY), textCol, msg.c_str());
        
        g_core->DecrementOSD();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifdef __SWITCH__
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
    SDL_GL_SwapWindow(g_window);
#endif
}

//==============================================================================
// Main
//==============================================================================

int main(int argc, char *argv[])
{
    Logger::Instance().ResetLogFile();

    g_running = true;
    g_controllersDirty = true;

    std::string romPath = GBAStationConfig::TEST_ROM;
    std::string displayTitle;
    bool romArgFound = false;

    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i])
            continue;
        const std::string argument = argv[i];
        if (argument == "--return" && i + 1 < argc && argv[i + 1])
        {
            g_returnNroPath = argv[++i];
            continue;
        }
        if (argument == "--gbastation-session" && i + 1 < argc && argv[i + 1])
        {
            g_externalSessionToken = argv[++i];
            continue;
        }
        if (argument.rfind("--", 0) == 0 || EndsWithNoCase(argument, ".nro"))
            continue;
        if (!romArgFound)
        {
            romPath = argument;
            romArgFound = true;
            continue;
        }
        if (displayTitle.empty())
            displayTitle = argument;
    }



#ifdef __SWITCH__
    LOG_INFO("HOME", "Calling appletLockExit...");
    appletLockExit();
    LOG_INFO("HOME", "Calling romfsInit...");
    Result romfsRc = romfsInit();
    if (R_FAILED(romfsRc))
    {
        LOG_WARN("HOME", "romfsInit failed: 0x%x", romfsRc);
    }
    else
    {
        LOG_INFO("HOME", "romfsInit succeeded");
    }

    if (R_SUCCEEDED(socketInitializeDefault()))
    {
        LOG_INFO("HOME", "socketInitializeDefault succeeded");
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    else
    {
        LOG_ERROR("HOME", "socketInitializeDefault failed");
    }

    LOG_INFO("HOME", "Calling nwindowSetDimensions...");
    nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
    LOG_INFO("HOME", "Switch pre-init complete (romfs, nwindow)");
#endif

    LOG_INFO("HOME", "fbneo starting...");

    GBAStationTranslationManager::Instance().Init();
    LoadGBAStationConfig();

    LOG_INFO("HOME", "Calling InitWindow...");
    if (!InitWindow())
    {
        LOG_ERROR("HOME", "Failed to initialize window");
        Logger::Instance().CloseLogFile();
        return 1;
    }
    LOG_INFO("HOME", "InitWindow succeeded");

#ifdef __SWITCH__
    ApplySwitchPerformanceProfile();
    PinCurrentThreadToCore(2, "main/render");
#endif

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef __SWITCH__
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
    SDL_GL_SwapWindow(g_window);
#endif

    LOG_INFO("HOME", "Calling InitImGui...");
    if (!InitImGui())
    {
        LOG_ERROR("HOME", "Failed to initialize ImGui");
        CleanupWindow();
        Logger::Instance().CloseLogFile();
        return 1;
    }
    LOG_INFO("HOME", "InitImGui succeeded");
#ifdef __SWITCH__
    g_lastOperationMode = 255;
#endif

    LOG_INFO("HOME", "Creating core...");
    g_core = std::make_unique<GBAStationCore>();

    LOG_INFO("HOME", "Creating overlay...");
    g_overlay = std::make_unique<GBAStationOverlay>();
    g_overlay->SetCore(g_core.get());

    g_core->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);

    if (!g_audio.Init(g_audioDevice))
    {
        LOG_WARN("HOME", "GBAStationAudio init failed");
    }

    LOG_INFO("HOME", "Core and overlay created");

    if (!romArgFound)
    {
        LOG_INFO("HOME", "No ROM argument provided. Using default: %s", romPath.c_str());
    }
    else
    {
        LOG_INFO("HOME", "ROM path provided via argv: %s", romPath.c_str());
    }

    {
        if (!displayTitle.empty()) {
            LOG_INFO("HOME", "Using cached game title: %s", displayTitle.c_str());
        } else {
            // Fallback: derive title from ROM filename
            size_t lastSlash = romPath.find_last_of("/\\");
            std::string filename = (lastSlash != std::string::npos) ? romPath.substr(lastSlash + 1) : romPath;
            displayTitle = GBAStationUtils::GetCleanTitle(filename);
            if (displayTitle.empty())
                displayTitle = filename;
            LOG_INFO("HOME", "Using filename-derived title: %s", displayTitle.c_str());
        }

        g_overlay->SetGameTitle(displayTitle);
    }

    LOG_INFO("HOME", "Loading ROM: %s", romPath.c_str());
    if (!g_core->LoadGame(romPath))
    {
        LOG_ERROR("HOME", "Failed to load ROM: %s", romPath.c_str());
    }
    else
    {
#ifdef __SWITCH__
        // Switch paces to the core's native fps (not a forced 60Hz), so the core
        // emits its sample rate over real time 1:1 — feed the resampler the raw
        // rate with no 60/fps stretch, otherwise audio pitch would be wrong.
        g_audio.SetCoreSampleRate(g_core->GetSampleRate());
        LOG_INFO("AUDIO", "Configured audio pipeline for %.0f Hz core output (native fps pacing)", g_core->GetSampleRate());
#else
        // Host uses vsync (forced ~60Hz), so stretch the audio to match.
        double adjustedSampleRate = g_core->GetSampleRate();
        if (g_core->GetFPS() > 0.0)
            adjustedSampleRate *= (60.0 / g_core->GetFPS());
        g_audio.SetCoreSampleRate(adjustedSampleRate);
        LOG_INFO("AUDIO", "Configured audio pipeline for %.0f Hz core output (stretched to 60Hz)", g_core->GetSampleRate());
#endif
        g_core->InitShaderPipeline();
    }

    Uint32 lastTime = SDL_GetTicks();

#ifdef __SWITCH__
    // Manual frame pacing keyed to the core's *actual* refresh rate. FBNeo arcade
    // games run at many rates (CPS ~59.6, others 55-58, some >60), so a hard 60Hz
    // vsync lock would run them fast/slow. vsync is off (eglSwapInterval=0); this
    // sleep is the sole governor. Audio is non-blocking, so it never compounds
    // with the pacing. The measurement includes the swap, so it self-corrects if
    // the swapchain ever blocks.
    static constexpr uint64_t TICKS_PER_SECOND = 19200000ULL;
    uint64_t frameStart = svcGetSystemTick();
#endif

    while (g_running)
    {
#ifdef __SWITCH__
        frameStart = svcGetSystemTick();

        if (!appletMainLoop())
        {
            LOG_INFO("HOME", "appletMainLoop returned false, exiting main loop");
            g_running = false;
            break;
        }
#endif

        float deltaTime = (SDL_GetTicks() - lastTime) / 1000.0f;
        lastTime = SDL_GetTicks();

        if (g_overlay)
        {
            g_overlay->Update(deltaTime);
        }

        ProcessEvents();
        HandleInput();
        Render();

#ifdef __SWITCH__
        // Pace to the core's real frame period, unless fast-forwarding (uncapped).
        if (!g_audio.IsFastForwarding())
        {
            double fps = g_core ? g_core->GetFPS() : 60.0;
            if (fps < 1.0)
                fps = 60.0;
            uint64_t frameTicks = (uint64_t)(TICKS_PER_SECOND / fps);
            uint64_t elapsedTicks = svcGetSystemTick() - frameStart;
            if (elapsedTicks < frameTicks)
            {
                uint64_t remainingTicks = frameTicks - elapsedTicks;
                int64_t sleepNs = (int64_t)((remainingTicks * 1000000000ULL) / TICKS_PER_SECOND);
                if (sleepNs > 0)
                    svcSleepThread(sleepNs);
            }
        }
#endif
    }

    LOG_INFO("HOME", "Starting cleanup...");
    g_overlay.reset();
    g_core.reset();



    g_audio.Shutdown();
    if (!GBAStationConfig::USE_SDLQUEUEAUDIO)
    {
        Mix_CloseAudio();
    }

    CleanupWindow();

#ifdef __SWITCH__
    romfsExit();
    curl_global_cleanup();
    socketExit();
    appletUnlockExit();
#endif

    LOG_INFO("HOME", "Clean exit");
    Logger::Instance().CloseLogFile();

    // For exit-to-system: exit(0) triggers libnx's __libnx_exit() which calls
    // __appExit() (tears down fsdev, fs, time, hid, applet, sm) and then
    // __nx_exit(0, envGetExitFuncPtr()).
    // Normally, Homebrew apps return to their loader (Sphaira/hbmenu) rather than exiting to OS.
    // By setting __nx_applet_exit_mode = 1, we bypass the loader and tell Switch OS to terminate the applet.
#ifdef __SWITCH__
    if (g_exitToSystem)
    {
        LOG_INFO("HOME", "g_exitToSystem is true, forcing applet termination via __nx_applet_exit_mode");
        __nx_applet_exit_mode = 1;
    }
#endif
    exit(0);
}
