// player_settings.cpp -- see the header for what this panel is, and for the rows that are deliberately
// not in it.
//
// GPL-3.0-only.
//
// The shape is copied from PlayerMenuPanel on purpose (right-anchored column, GONE when closed, every row a
// plain Label, nothing focusable): that panel is known to work on this screen, and a second panel that
// looked different would be a second layout to get wrong. See player_menu.cpp for the reasoning behind the
// geometry, which is not repeated here.
//
// ASCII-only source file.

#include "player_settings.hpp"

#include <cstdio>

#include "hg_lang.h"

extern "C" {
#include "hg_ctl.h"
}

namespace hg {
namespace {

const float PANEL_WIDTH = 460.0f;
const float ROW_HEIGHT = 40.0f;
const float ROW_FONT = 18.0f;
const float TITLE_FONT = 22.0f;
const float GROUP_FONT = 15.0f;
const float HINT_FONT = 14.0f;

NVGcolor panelColor() { return nvgRGBA(20, 20, 24, 208); }   /* build109: more see-through, so the video reads behind the UI */
NVGcolor rowColor() { return nvgRGBA(0, 0, 0, 0); }
NVGcolor highlightColor() { return nvgRGBA(255, 255, 255, 30); }
NVGcolor dimColor() { return brls::Application::getTheme().getColor("brls/text_disabled"); }

} // namespace

PlayerSettingsPanel::PlayerSettingsPanel() {
    this->setAxis(brls::Axis::COLUMN);
    this->setPositionType(brls::PositionType::ABSOLUTE);
    this->setPositionTop(0.0f);
    this->setPositionRight(0.0f);
    this->setWidth(PANEL_WIDTH);
    this->setHeightPercentage(100.0f);
    this->setBackgroundColor(panelColor());
    this->setPadding(14.0f, 16.0f, 14.0f, 16.0f);
    this->setFocusable(false);
    this->setVisibility(brls::Visibility::GONE);

    auto* title = new brls::Label();
    title->setText(hg_tr(HG_T_SETTINGS));
    title->setFontSize(TITLE_FONT);
    title->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    title->setMarginBottom(4.0f);
    this->addView(title);

    /* THE GROUP HEADER. The reference layout groups these rows (通用 / 着色器 / 均衡器); this panel has one
     * group because only one of them has rows that can do anything. The header is drawn anyway, so the shape
     * of the panel is the reference's and a future row lands in a group instead of at the top. */
    auto* group = new brls::Label();
    group->setText(hg_tr(HG_T_PSET_GENERAL));
    group->setFontSize(GROUP_FONT);
    group->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    group->setTextColor(dimColor());
    group->setMarginBottom(4.0f);
    this->addView(group);

    static const int LABEL_KEYS[ROW_COUNT] = {HG_T_PSET_SLEEP, HG_T_PSET_BAR, HG_T_PSET_PLAYMODE};

    for (int i = 0; i < ROW_COUNT; i++) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(ROW_HEIGHT);
        row->setPadding(0.0f, 8.0f, 0.0f, 8.0f);
        row->setBackgroundColor(rowColor());
        row->setFocusable(false);

        auto* name = new brls::Label();
        name->setText(hg_tr(LABEL_KEYS[i]));
        name->setFontSize(ROW_FONT);
        name->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        name->setGrow(1.0f);
        row->addView(name);

        auto* value = new brls::Label();
        value->setText("");
        value->setFontSize(ROW_FONT);
        value->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        value->setTextColor(dimColor());
        row->addView(value);

        this->addView(row);
        this->rowBoxes[i] = row;
        this->valueLabels[i] = value;
    }

    /* THE HINT IS THE SAME STRING AS THE PICTURE MENU'S, on purpose: the two panels take the same keys, so
     * telling the user two different things about A and B would be worse than saying nothing. */
    auto* hint = new brls::Label();
    hint->setText(hg_tr(HG_T_MENU_HINT));
    hint->setFontSize(HINT_FONT);
    hint->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    hint->setTextColor(dimColor());
    hint->setMarginTop(6.0f);
    this->addView(hint);
}

brls::View* PlayerSettingsPanel::create() { return new PlayerSettingsPanel(); }

void PlayerSettingsPanel::setPanelVisible(bool show) {
    this->setVisibility(show ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
}

void PlayerSettingsPanel::setValues(int sleepIndex, int barFlag, int playMode) {
    char buf[32];

    if (sleepIndex != this->shownSleep) {
        this->shownSleep = sleepIndex;
        const int minutes = hg_ctl_sleep_minutes_at(sleepIndex);
        if (minutes <= 0) {
            snprintf(buf, sizeof buf, "%s", hg_tr(HG_T_VALUE_OFF));
        } else {
            snprintf(buf, sizeof buf, hg_tr(HG_T_PSET_MINUTES), minutes);
        }
        buf[sizeof buf - 1] = '\0';
        if (this->valueLabels[ROW_SLEEP]) this->valueLabels[ROW_SLEEP]->setText(buf);
    }

    if (barFlag != this->shownBar) {
        this->shownBar = barFlag;
        if (this->valueLabels[ROW_BAR]) {
            this->valueLabels[ROW_BAR]->setText(hg_tr(barFlag ? HG_T_VALUE_ON : HG_T_VALUE_OFF));
        }
    }

    if (playMode != this->shownPlay) {
        this->shownPlay = playMode;
        if (this->valueLabels[ROW_PLAY]) {
            this->valueLabels[ROW_PLAY]->setText(hg_tr(playMode ? HG_T_PSET_AUTONEXT : HG_T_PSET_STOP));
        }
    }
}

void PlayerSettingsPanel::setHighlight(int row) {
    if (row == this->shownRow) return;
    this->shownRow = row;
    for (int i = 0; i < ROW_COUNT; i++) {
        if (!this->rowBoxes[i]) continue;
        this->rowBoxes[i]->setBackgroundColor(i == row ? highlightColor() : rowColor());
    }
}

} // namespace hg
