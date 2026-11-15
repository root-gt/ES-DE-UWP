//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiRomM.h
//
//  RomM server settings, connection test and library import.
//  Accessed via Main menu > Utilities > RomM.
//

#ifndef ES_APP_GUIS_GUI_ROMM_H
#define ES_APP_GUIS_GUI_ROMM_H

#include <functional>

class FileData;

namespace GuiRomM
{
    // Opens the RomM settings menu. rescanFunction is called after a successful import and
    // should close the menus and rescan the ROM directory.
    void openSettings(std::function<void()> rescanFunction);

    // Returns true if the game is a RomM placeholder that will be downloaded when launched.
    bool needsDownload(FileData* game);

    // Downloads the game file for a placeholder and calls onSuccess afterwards.
    void downloadGame(FileData* game, std::function<void()> onSuccess);

} // namespace GuiRomM

#endif // ES_APP_GUIS_GUI_ROMM_H
