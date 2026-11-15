//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiRomMProgress.h
//
//  Modal progress dialog for RomM operations (game downloads, library imports and
//  connection tests). The work is run in a separate thread so the UI stays responsive.
//

#ifndef ES_APP_GUIS_GUI_ROMM_PROGRESS_H
#define ES_APP_GUIS_GUI_ROMM_PROGRESS_H

#include "GuiComponent.h"
#include "RomM.h"
#include "components/BackgroundComponent.h"
#include "components/ButtonComponent.h"
#include "components/ComponentGrid.h"
#include "components/TextComponent.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

class GuiRomMProgress : public GuiComponent
{
public:
    using WorkFunction = std::function<bool(RomM::Progress& progress, std::string& message)>;
    using DoneFunction = std::function<void(bool success, const std::string& message)>;

    // If closeOnSuccess is true then the dialog closes automatically when the work completes
    // successfully, otherwise the result message is shown until the user closes the dialog.
    GuiRomMProgress(const std::string& title,
                    WorkFunction work,
                    DoneFunction done,
                    bool closeOnSuccess);
    ~GuiRomMProgress();

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    void onSizeChanged() override;
    void finish();
    void close();

    Renderer* mRenderer;
    BackgroundComponent mBackground;
    ComponentGrid mGrid;

    std::shared_ptr<TextComponent> mTitle;
    std::shared_ptr<TextComponent> mStatus;
    std::shared_ptr<TextComponent> mDetail;
    std::shared_ptr<TextComponent> mProgressText;
    std::shared_ptr<ComponentGrid> mButtonGrid;
    std::shared_ptr<ButtonComponent> mButton;

    RomM::Progress mProgress;
    WorkFunction mWork;
    DoneFunction mDone;
    std::thread mThread;
    std::atomic<bool> mFinished;
    bool mSuccess;
    bool mCompleted;
    bool mCloseOnSuccess;
    std::string mMessage;
};

#endif // ES_APP_GUIS_GUI_ROMM_PROGRESS_H
