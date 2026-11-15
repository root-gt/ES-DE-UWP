//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiRomM.cpp
//
//  RomM server settings, connection test and library import.
//  Accessed via Main menu > Utilities > RomM.
//

#include "guis/GuiRomM.h"

#include "FileData.h"
#include "Log.h"
#include "RomM.h"
#include "Settings.h"
#include "SystemData.h"
#include "Window.h"
#include "components/SwitchComponent.h"
#include "components/TextComponent.h"
#include "guis/GuiMsgBox.h"
#include "guis/GuiRomMProgress.h"
#include "guis/GuiSettings.h"
#include "utils/LocalizationUtil.h"
#include "utils/StringUtil.h"

#include <memory>
#include <set>

namespace
{
    // Gives access to the protected menu color of the current color scheme.
    struct MenuColors : public GuiComponent {
        static unsigned int primary() { return mMenuColorPrimary; }
    };

    void addTextSetting(GuiSettings* s,
                        const std::string& label,
                        const std::string& setting,
                        bool isPassword = false)
    {
        auto text = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_MEDIUM),
                                                    MenuColors::primary(), ALIGN_RIGHT);
        if (isPassword) {
            std::string masked;
            if (Settings::getInstance()->getString(setting) != "") {
                masked = "********";
                text->setHiddenValue(Settings::getInstance()->getString(setting));
            }
            s->addEditableTextComponent(label, text, masked, "", true);
        }
        else {
            s->addEditableTextComponent(label, text, Settings::getInstance()->getString(setting));
        }
        text->setSize(0.0f, text->getFont()->getHeight());
        s->addSaveFunc([text, s, setting, isPassword] {
            const std::string value {isPassword ? text->getHiddenValue() : text->getValue()};
            if (value != Settings::getInstance()->getString(setting)) {
                Settings::getInstance()->setString(setting, Utils::String::trim(value));
                s->setNeedsSaving();
            }
        });
    }

    void addSwitchSetting(GuiSettings* s, const std::string& label, const std::string& setting)
    {
        auto toggle = std::make_shared<SwitchComponent>();
        toggle->setState(Settings::getInstance()->getBool(setting));
        s->addWithLabel(label, toggle);
        s->addSaveFunc([toggle, s, setting] {
            if (toggle->getState() != Settings::getInstance()->getBool(setting)) {
                Settings::getInstance()->setBool(setting, toggle->getState());
                s->setNeedsSaving();
            }
        });
    }

    void addActionRow(GuiSettings* s, const std::string& label, std::function<void()> action)
    {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                       MenuColors::primary()),
                       true);
        auto arrow = s->getMenu().makeArrow();
        row.addElement(arrow, false);
        row.makeAcceptInputHandler(action);
        s->addRow(row);
    }

    void showMessage(const std::string& message)
    {
        Window::getInstance()->pushGui(new GuiMsgBox(message));
    }

    bool checkConfigured()
    {
        if (Settings::getInstance()->getString("RomMServerURL").empty()) {
            showMessage(_("PLEASE ENTER THE ROMM SERVER URL FIRST"));
            return false;
        }
        return true;
    }

    void runImport(std::shared_ptr<std::vector<RomM::Platform>> platforms,
                   std::function<void()> rescanFunction)
    {
        // Write any pending gamelist.xml changes before the importer modifies the files.
        if (Settings::getInstance()->getString("SaveGamelistsMode") == "on exit") {
            for (auto system : SystemData::sSystemVector)
                system->writeMetaData();
        }

        auto stats = std::make_shared<RomM::ImportStats>();

        Window::getInstance()->pushGui(new GuiRomMProgress(
            _("IMPORTING FROM ROMM"),
            [platforms, stats](RomM::Progress& progress, std::string& message) {
                RomM::Client client;
                std::string error;
                const bool success {client.importPlatforms(*platforms, progress, *stats, error)};
                message = Utils::String::format(
                    _("%i NEW, %i UPDATED, %i UNCHANGED, %i FAILED, %i MEDIA FILES"),
                    stats->imported, stats->updated, stats->skipped, stats->failed,
                    stats->mediaFiles);
                if (!success)
                    message = error + "\n" + message;
                LOG(LogInfo) << "RomM: Import finished: " << message;
                return success;
            },
            [stats, rescanFunction](bool success, const std::string&) {
                // Rescan as soon as anything was written, even if the import was cancelled.
                if ((stats->imported > 0 || stats->updated > 0 || stats->mediaFiles > 0) &&
                    rescanFunction)
                    rescanFunction();
            },
            false));
    }

    void openPlatformSelection(std::shared_ptr<std::vector<RomM::Platform>> platforms,
                               std::function<void()> rescanFunction)
    {
        auto s = new GuiSettings(_("SELECT PLATFORMS"));

        const std::vector<std::string> selected {RomM::getSelectedPlatforms()};
        const std::set<std::string> selectedSet {selected.cbegin(), selected.cend()};

        auto switches =
            std::make_shared<std::vector<std::pair<std::string, std::shared_ptr<SwitchComponent>>>>();
        int unsupported {0};

        for (auto& platform : *platforms) {
            if (platform.esdeSystem.empty()) {
                LOG(LogInfo) << "RomM: Platform \"" << platform.name << "\" (slug \""
                             << platform.slug << "\", fs_slug \"" << platform.fsSlug
                             << "\") has no ES-DE system mapping";
                ++unsupported;
                continue;
            }
            if (platform.romCount == 0)
                continue;
            auto toggle = std::make_shared<SwitchComponent>();
            toggle->setState(selectedSet.empty() ||
                             selectedSet.find(platform.slug) != selectedSet.end());
            s->addWithLabel(Utils::String::toUpper(platform.name) + "  [" +
                                platform.esdeSystem + ", " + std::to_string(platform.romCount) +
                                "]",
                            toggle);
            switches->emplace_back(platform.slug, toggle);
        }

        if (switches->empty()) {
            delete s;
            showMessage(_("NO ROMM PLATFORMS WITH GAMES COULD BE MAPPED TO ES-DE SYSTEMS"));
            return;
        }

        auto storeSelection = [switches, s] {
            std::string value;
            for (auto& entry : *switches) {
                if (entry.second->getState())
                    value.append(value.empty() ? "" : ",").append(entry.first);
            }
            // Store "none" when everything is deselected so the default isn't applied again.
            if (value.empty())
                value = "none";
            if (value != Settings::getInstance()->getString("RomMImportPlatforms")) {
                Settings::getInstance()->setString("RomMImportPlatforms", value);
                s->setNeedsSaving();
            }
        };
        s->addSaveFunc(storeSelection);

        addActionRow(s, _("START IMPORT"), [s, platforms, switches, rescanFunction] {
            auto selection = std::make_shared<std::vector<RomM::Platform>>();
            for (auto& platform : *platforms) {
                for (auto& entry : *switches) {
                    if (entry.first == platform.slug && entry.second->getState())
                        selection->emplace_back(platform);
                }
            }
            if (selection->empty()) {
                showMessage(_("NO PLATFORMS SELECTED"));
                return;
            }
            // This lambda is owned by a row in s, so copy the captures before deleting s.
            std::function<void()> rescan {rescanFunction};
            delete s; // Saves the selection.
            runImport(selection, rescan);
        });

        if (unsupported > 0) {
            LOG(LogInfo) << "RomM: " << unsupported
                         << " platform(s) could not be mapped, add them to the "
                            "RomMPlatformMappings setting (romm-slug=es-de-system)";
        }

        Window::getInstance()->pushGui(s);
    }

    void startImport(std::function<void()> rescanFunction)
    {
        auto platforms = std::make_shared<std::vector<RomM::Platform>>();
        Window::getInstance()->pushGui(new GuiRomMProgress(
            _("ROMM PLATFORMS"),
            [platforms](RomM::Progress& progress, std::string& message) {
                progress.setStatus(_("FETCHING PLATFORMS"));
                RomM::Client client;
                return client.getPlatforms(*platforms, message);
            },
            [platforms, rescanFunction](bool success, const std::string& message) {
                if (success)
                    openPlatformSelection(platforms, rescanFunction);
            },
            true));
    }

} // namespace

namespace GuiRomM
{
    void openSettings(std::function<void()> rescanFunction)
    {
        auto s = new GuiSettings("ROMM");

        addSwitchSetting(s, _("ENABLE ROMM INTEGRATION"), "RomMEnabled");
        addTextSetting(s, _("SERVER URL"), "RomMServerURL");
        addTextSetting(s, _("API TOKEN"), "RomMAPIToken", true);
        addTextSetting(s, _("USERNAME"), "RomMUsername");
        addTextSetting(s, _("PASSWORD"), "RomMPassword", true);
        addSwitchSetting(s, _("DOWNLOAD GAMES ON LAUNCH"), "RomMDownloadOnLaunch");
        addSwitchSetting(s, _("IMPORT GAME MEDIA"), "RomMImportMedia");
        addSwitchSetting(s, _("IMPORT VIDEOS"), "RomMImportVideos");
        addSwitchSetting(s, _("DELETE ARCHIVES AFTER EXTRACTION"),
                         "RomMDeleteArchiveAfterExtraction");
        addTextSetting(s, _("EXTRACT ARCHIVES"), "RomMExtractedExtensions");
        addTextSetting(s, _("PLATFORM MAPPINGS"), "RomMPlatformMappings");

        addActionRow(s, _("TEST CONNECTION"), [s] {
            s->save(); // Apply the values currently entered.
            if (!checkConfigured())
                return;
            Window::getInstance()->pushGui(new GuiRomMProgress(
                _("ROMM CONNECTION TEST"),
                [](RomM::Progress& progress, std::string& message) {
                    progress.setStatus(_("CONNECTING..."));
                    RomM::Client client;
                    return client.testConnection(message);
                },
                nullptr, false));
        });

        addActionRow(s, _("IMPORT GAMES FROM ROMM"), [s, rescanFunction] {
            s->save();
            if (!checkConfigured())
                return;
            if (!Settings::getInstance()->getBool("RomMEnabled")) {
                showMessage(_("PLEASE ENABLE THE ROMM INTEGRATION FIRST"));
                return;
            }
            startImport([s, rescanFunction] {
                delete s;
                if (rescanFunction)
                    rescanFunction();
            });
        });

        Window::getInstance()->pushGui(s);
    }

    bool needsDownload(FileData* game)
    {
        if (game == nullptr || !RomM::downloadOnLaunchEnabled())
            return false;
        return RomM::readPlaceholder(game->getPath()).has_value();
    }

    void downloadGame(FileData* game, std::function<void()> onSuccess)
    {
        const std::string path {game->getPath()};
        const std::string system {game->getSourceFileData()->getSystem()->getName()};
        const std::string name {game->getName()};

        LOG(LogInfo) << "RomM: \"" << name << "\" is a placeholder, downloading it from RomM";

        Window::getInstance()->pushGui(new GuiRomMProgress(
            _("DOWNLOADING FROM ROMM"),
            [path, system, name](RomM::Progress& progress, std::string& message) {
                progress.setDetail(name);
                RomM::Client client;
                return client.downloadGame(path, system, progress, message);
            },
            [onSuccess](bool success, const std::string& message) {
                if (success && onSuccess)
                    onSuccess();
            },
            true));
    }

} // namespace GuiRomM
