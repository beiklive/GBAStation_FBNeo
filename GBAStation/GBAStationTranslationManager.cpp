/// @file GBAStationTranslationManager.cpp
/// @brief Implementation of i18n translation loading

#include "GBAStationTranslationManager.h"
#include <json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

GBAStationTranslationManager& GBAStationTranslationManager::Instance() {
    static GBAStationTranslationManager instance;
    return instance;
}

bool GBAStationTranslationManager::Init() {
    std::string language = "Chinese";

    // Prefer the launcher's UI.language (config.cfg, values zh-CN / en-US).
    // Fall back to general.jsonc's "language" for standalone/older launches.
    std::ifstream cfgFile("sdmc:/GBAStation/config/config.cfg");
    if (cfgFile.is_open()) {
        std::string line;
        while (std::getline(cfgFile, line)) {
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = line.substr(0, equals);
            if (key != "UI.language")
                continue;
            std::string value = line.substr(equals + 1);
            if (!value.empty() && value.front() == '"' && value.back() == '"' && value.size() >= 2)
                value = value.substr(1, value.size() - 2);
            if (value == "en-US" || value == "en")
                language = "English";
            else
                language = "Chinese";
            break;
        }
        cfgFile.close();
    }
    if (language == "Chinese") {
        std::ifstream cfgFile2("sdmc:/GBAStation/config/general.jsonc");
        if (cfgFile2.is_open()) {
            json cfg = json::parse(cfgFile2, nullptr, false, true);
            if (!cfg.is_discarded() && cfg.contains("language") && cfg["language"].is_string()) {
                language = cfg["language"].get<std::string>();
            }
            cfgFile2.close();
        }
    }

    if (m_currentLanguage == language && !m_translations.empty()) {
        return true;
    }

    m_translations.clear();
    m_currentLanguage = language;

    std::string filename = "";
    if (language == "English") filename = "en.json";
    else if (language == "Portuguese") filename = "pt.json";
    else if (language == "Espanol") filename = "es.json";
    else if (language == "Japanese") filename = "ja.json";
    else if (language == "French") filename = "fr.json";
    else if (language == "Chinese") filename = "zh.json";
    else filename = "en.json";

    std::string langPath = "romfs:/lang/" + filename;
    std::ifstream file(langPath);
    if (!file.is_open()) {
        return false;
    }

    json j = json::parse(file, nullptr, false);
    if (!j.is_discarded()) {
        for (auto& el : j.items()) {
            if (el.value().is_string()) {
                m_translations[el.key()] = el.value().get<std::string>();
            }
        }
    }

    return true;
}

std::string GBAStationTranslationManager::GetString(const std::string& key) const {
    auto it = m_translations.find(key);
    if (it != m_translations.end()) {
        return it->second;
    }
    return key;
}

std::string tr(const std::string& key) {
    return GBAStationTranslationManager::Instance().GetString(key);
}
