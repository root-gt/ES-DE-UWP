//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiRomMProgress.cpp
//
//  Modal progress dialog for RomM operations (game downloads, library imports and
//  connection tests). The work is run in a separate thread so the UI stays responsive.
//

#include "guis/GuiRomMProgress.h"

#include "Log.h"
#include "Window.h"
#include "components/MenuComponent.h"
#include "utils/LocalizationUtil.h"
#include "utils/StringUtil.h"

GuiRomMProgress::GuiRomMProgress(const std::string& title,
                                 WorkFunction work,
                                 DoneFunction done,
                                 bool closeOnSuccess)
    : mRenderer {Renderer::getInstance()}
    , mGrid {glm::ivec2 {1, 6}}
    , mWork {work}
    , mDone {done}
    , mFinished {false}
    , mSuccess {false}
    , mCompleted {false}
    , mCloseOnSuccess {closeOnSuccess}
{
    addChild(&mBackground);
    addChild(&mGrid);

    mTitle = std::make_shared<TextComponent>(
        Utils::String::toUpper(title),
        Font::get(FONT_SIZE_LARGE * Utils::Localization::sMenuTitleScaleFactor), mMenuColorTitle,
        ALIGN_CENTER);
    mGrid.setEntry(mTitle, glm::ivec2 {0, 0}, false, true);

    mStatus = std::make_shared<TextComponent>(_("CONNECTING..."), Font::get(FONT_SIZE_MEDIUM),
                                              mMenuColorPrimary, ALIGN_CENTER);
    mStatus->setRemoveLineBreaks(true);
    mGrid.setEntry(mStatus, glm::ivec2 {0, 1}, false, true, glm::ivec2 {1, 1},
                   GridFlags::BORDER_TOP);

    mDetail = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_SMALL), mMenuColorSecondary,
                                              ALIGN_CENTER);
    mDetail->setRemoveLineBreaks(true);
    mGrid.setEntry(mDetail, glm::ivec2 {0, 2}, false, true);

    mProgressText = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_MEDIUM),
                                                    mMenuColorPrimary, ALIGN_CENTER);
    mGrid.setEntry(mProgressText, glm::ivec2 {0, 3}, false, true);

    mGrid.setEntry(std::make_shared<GuiComponent>(), glm::ivec2 {0, 4}, false, false,
                   glm::ivec2 {1, 1}, GridFlags::BORDER_BOTTOM);

    std::vector<std::shared_ptr<ButtonComponent>> buttons;
    mButton = std::make_shared<ButtonComponent>(_("CANCEL"), _("cancel"), [this] {
        if (mCompleted)
            close();
        else
            mProgress.cancel = true;
    });
    buttons.push_back(mButton);
    mButtonGrid = MenuComponent::makeButtonGrid(buttons);
    mGrid.setEntry(mButtonGrid, glm::ivec2 {0, 5}, true, false);

    const float aspectValue {1.778f / mRenderer->getScreenAspectRatio()};
    const float width {glm::clamp(0.70f * aspectValue, 0.45f,
                                  (mRenderer->getIsVerticalOrientation() ? 0.95f : 0.85f)) *
                       mRenderer->getScreenWidth()};
    const float height {mTitle->getFont()->getLetterHeight() * 1.9725f +
                        Font::get(FONT_SIZE_MEDIUM)->getHeight() * 2.0f +
                        Font::get(FONT_SIZE_SMALL)->getHeight() * 1.5f +
                        Font::get(FONT_SIZE_MEDIUM)->getHeight() * 2.0f +
                        mButtonGrid->getSize().y + Font::get(FONT_SIZE_SMALL)->getHeight()};
    setSize(width, height);
    setPosition((mRenderer->getScreenWidth() - mSize.x) / 2.0f,
                (mRenderer->getScreenHeight() - mSize.y) / 2.0f);

    mThread = std::thread([this] {
        std::string message;
        bool success {false};
        try {
            success = mWork(mProgress, message);
        }
        catch (const std::exception& e) {
            message = e.what();
            success = false;
        }
        mMessage = message;
        mSuccess = success;
        mFinished = true;
    });
}

GuiRomMProgress::~GuiRomMProgress()
{
    mProgress.cancel = true;
    if (mThread.joinable())
        mThread.join();
}

void GuiRomMProgress::onSizeChanged()
{
    mBackground.fitTo(mSize);
    mGrid.setSize(mSize);

    mGrid.setRowHeightPerc(0, mTitle->getFont()->getLetterHeight() * 1.9725f / mSize.y, false);
    mGrid.setRowHeightPerc(1, mStatus->getFont()->getHeight() * 2.0f / mSize.y, false);
    mGrid.setRowHeightPerc(2, mDetail->getFont()->getHeight() * 1.5f / mSize.y, false);
    mGrid.setRowHeightPerc(3, mProgressText->getFont()->getHeight() * 2.0f / mSize.y, false);
    mGrid.setRowHeightPerc(5, mButtonGrid->getSize().y / mSize.y, false);
}

void GuiRomMProgress::update(int deltaTime)
{
    GuiComponent::update(deltaTime);

    if (mCompleted)
        return;

    if (mFinished) {
        finish();
        return;
    }

    const std::string status {mProgress.getStatus()};
    if (!status.empty())
        mStatus->setText(Utils::String::toUpper(status));
    mDetail->setText(mProgress.getDetail());

    std::string progressText;
    const long long bytesNow {mProgress.bytesNow};
    const long long bytesTotal {mProgress.bytesTotal};
    const int itemsNow {mProgress.itemsNow};
    const int itemsTotal {mProgress.itemsTotal};

    if (itemsTotal > 0) {
        progressText = std::to_string(itemsNow) + " / " + std::to_string(itemsTotal);
    }
    else if (bytesTotal > 0) {
        const int percentage {
            static_cast<int>(glm::clamp(static_cast<double>(bytesNow) / bytesTotal, 0.0, 1.0) *
                             100.0)};
        progressText = std::to_string(percentage) + "%  (" + RomM::formatBytes(bytesNow) +
                       " / " + RomM::formatBytes(bytesTotal) + ")";
    }
    else if (bytesNow > 0) {
        progressText = RomM::formatBytes(bytesNow);
    }
    if (mProgress.cancel)
        progressText = _("CANCELLING...");

    mProgressText->setText(progressText);
}

void GuiRomMProgress::finish()
{
    if (mThread.joinable())
        mThread.join();
    mCompleted = true;

    if (mSuccess && mCloseOnSuccess) {
        close();
        return;
    }

    mStatus->setText(mSuccess ? _("COMPLETED") : (mProgress.cancel ? _("CANCELLED") : _("FAILED")));
    mDetail->setText("");
    mProgressText->setText(mMessage);
    mButton->setText(_("OK"), _("close"));
    updateHelpPrompts();
}

void GuiRomMProgress::close()
{
    // Copy the callback and result as this object is deleted before the callback runs.
    DoneFunction done {mDone};
    const bool success {mSuccess};
    const std::string message {mMessage};
    delete this;
    if (done)
        done(success, message);
}

bool GuiRomMProgress::input(InputConfig* config, Input input)
{
    if (input.value != 0 && (config->isMappedTo("b", input) || config->isMappedTo("back", input))) {
        if (mCompleted)
            close();
        else
            mProgress.cancel = true;
        return true;
    }
    return GuiComponent::input(config, input);
}

std::vector<HelpPrompt> GuiRomMProgress::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts {mGrid.getHelpPrompts()};
    prompts.push_back(HelpPrompt("b", mCompleted ? _("close") : _("cancel")));
    return prompts;
}
