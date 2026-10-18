/*
 * PlayerMenuPanel implementation. See player_menu.hpp for the design decisions --
 * in particular why nothing here is focusable.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "player_menu.hpp"

#include <cstdio>

#include "hg_cfg.hpp"
#include "hg_log.hpp"

extern "C" {
#include "hg_lang.h"
}

namespace hg {
namespace {

/* ANCHORED TO THE RIGHT, FULL HEIGHT.
 *
 * wiliwili's own panel (resources/xml/fragment/player_setting.xml) is a 460-wide
 * column pinned to the right edge over a `brls/backdrop` scrim, with the rest of
 * the width left as a click-anywhere-to-cancel target. This panel keeps the
 * position and the width and drops the scrim: the scrim exists to catch touch
 * input, and there is no touch to catch here. Dropping it also keeps the picture
 * visible while the numbers are adjusted, which is the entire point -- you cannot
 * judge brightness against a black rectangle.
 *
 * 460 is wiliwili's number in its own logical space, and this app's default logical
 * space is borealis' 1280x720 (see the ui.scale note in hongguo.cfg), so the same
 * number means the same thing here. */
const float PANEL_WIDTH = 460.0f;

/* 8 rows at 40 pt plus the title and hint fit inside 720 logical pixels with room
 * to spare, so the panel needs no scrolling. That is worth stating because it is
 * what lets every row be a plain Label: a scroller would be focusable and would
 * take UP/DOWN, which is exactly the input this panel must not consume. */
const float ROW_HEIGHT = 40.0f;
const float ROW_FONT = 18.0f;
const float TITLE_FONT = 22.0f;
const float HINT_FONT = 14.0f;

NVGcolor panelColor() { return nvgRGBA(20, 20, 24, 208); }   /* build109: more see-through, so the video reads behind the UI */
NVGcolor normalRowColor() { return nvgRGBA(0, 0, 0, 0); }
NVGcolor valueColor() { return brls::Application::getTheme().getColor("brls/text_disabled"); }

} // namespace

PlayerMenuPanel::PlayerMenuPanel() {
  /* A COLUMN, PINNED TO THE RIGHT EDGE, FULL HEIGHT.
   *
   * positionType ABSOLUTE + positionRight 0 takes the panel out of the root Box's
   * flex flow, so it floats over the video instead of being laid out under the
   * overlay text. The root is a COLUMN Box whose first child takes the remaining
   * space, and a flow child here would push that around. */
  this->setAxis(brls::Axis::COLUMN);
  this->setPositionType(brls::PositionType::ABSOLUTE);
  this->setPositionTop(0.0f);
  this->setPositionRight(0.0f);
  this->setWidth(PANEL_WIDTH);
  this->setHeightPercentage(100.0f);
  this->setBackgroundColor(panelColor());
  this->setPadding(14.0f, 16.0f, 14.0f, 16.0f);

  /* Nothing in this panel may take focus. Setting it explicitly (rather than
   * relying on the default) makes the intent readable and survives any future
   * change to the framework's default. */
  this->setFocusable(false);

  /* HIDDEN FROM THE START, and GONE rather than INVISIBLE: GONE takes no space and
   * is skipped by the focus search entirely. The player screen must be byte-for-byte
   * what it was before this feature until X is actually pressed. */
  this->setVisibility(brls::Visibility::GONE);

  auto* title = new brls::Label();
  title->setText(hg_tr(HG_T_PICTURE));
  title->setFontSize(TITLE_FONT);
  title->setHorizontalAlign(brls::HorizontalAlign::LEFT);
  title->setMarginBottom(6.0f);
  this->addView(title);
  this->titleLabel = title;

  char valueBuf[32];
  HgPMenu scratch;
  scratch.open = 1;
  scratch.row = HG_PMENU_ROW_BRIGHTNESS;
  hg_pmenu_reset(&scratch);

  for (int i = 0; i < HG_PMENU_ROW_COUNT; i++) {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(ROW_HEIGHT);
    row->setPadding(0.0f, 8.0f, 0.0f, 8.0f);
    row->setBackgroundColor(normalRowColor());
    row->setFocusable(false);

    auto* name = new brls::Label();
    /* THE CHINESE LABEL, from the key the model hands back. The model keeps its own English strings for
     * hongguo.log; this is what the user reads. A key of -1 means "this row has no label", and then the
     * English one is the fallback rather than an empty row. */
    const int labelKey = hg_pmenu_row_label_key(i);
    const char* label = labelKey >= 0 ? hg_tr(labelKey) : hg_pmenu_row_label(i);
    name->setText(label ? label : "?");
    name->setFontSize(ROW_FONT);
    name->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    name->setGrow(1.0f);
    row->addView(name);

    auto* value = new brls::Label();
    value->setText(hg_pmenu_row_value(&scratch, i, valueBuf, (int)sizeof valueBuf));
    value->setFontSize(ROW_FONT);
    value->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    value->setTextColor(valueColor());
    row->addView(value);

    this->addView(row);

    this->rowBoxes[i] = row;
    this->nameLabels[i] = name;
    this->valueLabels[i] = value;
  }

  auto* hint = new brls::Label();
  /* THE PANEL HAS TO SAY HOW TO LEAVE IT.
   *
   * MEASURED ON HARDWARE: the first console run reported "I can see the picture quality option but I
   * do not know how to operate it, and pressing A does nothing". The keys worked -- A and B both
   * close the panel -- but the hint said only what the ROWS do, and the one thing a modal overlay
   * must always state is how to get out of it. A user who cannot find the exit reads the whole
   * feature as broken.
   *
   * So the hint names the exit first, then the navigation. It is also the line that carries the fact
   * that A is not "activate the row" here: every row changes with left/right, so there is nothing for
   * A to activate. */
  hint->setText(hg_tr(HG_T_MENU_HINT));
  hint->setFontSize(HINT_FONT);
  hint->setHorizontalAlign(brls::HorizontalAlign::LEFT);
  hint->setTextColor(valueColor());
  hint->setMarginTop(8.0f);
  this->addView(hint);
  this->hintLabel = hint;

  hg::logPrintf("PLAYERMENU: panel built (%d rows, w=%.0f, non-focusable)",
                HG_PMENU_ROW_COUNT, PANEL_WIDTH);
}

PlayerMenuPanel::~PlayerMenuPanel() = default;

void PlayerMenuPanel::setPanelVisible(bool visible) {
  if (visible) {
    /* The per-row highlight is stale after a hide, so force a repaint of it. */
    this->lastHighlighted = -1;
    this->setVisibility(brls::Visibility::VISIBLE);
  } else {
    this->setVisibility(brls::Visibility::GONE);
  }
  this->shown = visible;
}

/* The highlighted row gets a background; every other row gets a fully transparent
 * one. Two rows are touched per move -- the one losing the highlight and the one
 * gaining it -- so a held UP does not repaint eight rows per frame.
 *
 * The colour is brls/accent rather than brls/highlight/background: the panel's own
 * background is a dark grey, and the dark theme's highlight background is RGB(31,34,39)
 * -- one shade away from the panel, i.e. the highlight would be invisible. The accent
 * is the same token the progress bar already uses, so a selected row and the played
 * part of the bar are the same colour. */
void PlayerMenuPanel::applyHighlight(int row) {
  if (row == this->lastHighlighted) return;
  if (this->lastHighlighted >= 0 && this->lastHighlighted < HG_PMENU_ROW_COUNT) {
    this->rowBoxes[this->lastHighlighted]->setBackgroundColor(normalRowColor());
  }
  if (row >= 0 && row < HG_PMENU_ROW_COUNT) {
    this->rowBoxes[row]->setBackgroundColor(
        brls::Application::getTheme().getColor("brls/accent"));
  }
  this->lastHighlighted = row;
}

void PlayerMenuPanel::updateValues(const HgPMenu& menu, const char* const* qualityLabels,
                                   int qualityCount) {
  char buf[32];
  char qbuf[64];
  for (int i = 0; i < HG_PMENU_ROW_COUNT; i++) {
    /* The label comes from the table once, at construction; only the value can
     * change. Re-setting an unchanged string would allocate every frame for
     * nothing, so it is compared first. */
    const char* next = hg_pmenu_row_value(&menu, i, buf, (int)sizeof buf);

    /* WORDS COME FROM THE LANGUAGE TABLE. hg_pmenu_row_value builds the English form (which is what
     * hongguo.log shows); when the value is one of the words -- On/Off/Auto/Crop/Stretch/not available --
     * the key it reports is what the panel draws instead. */
    const int valueKey = hg_pmenu_row_value_key(&menu, i);
    if (valueKey >= 0) next = hg_tr(valueKey);

    /* THE QUALITY ROW GETS THE LADDER'S LABEL, NOT A POSITION. hg_pmenu can only say "2 of 5"
     * -- it does not know what the tiers are called, by design (see its header). "540p" is the
     * answer the user is looking for, so when the caller has the ladder, use it.
     *
     * AND IT CARRIES THE PENDING MARKER: the user asked for the two-step ("左右只移光标、A 才生效"), so a
     * cursor that has moved must LOOK different from the tier in force. Without the marker the row would
     * show the tier the user has not applied yet and read as if it were already playing. */
    if (i == HG_PMENU_ROW_QUALITY && qualityLabels && qualityCount > 0 &&
        menu.qualityIndex >= 0 && menu.qualityIndex < qualityCount && qualityLabels[menu.qualityIndex]) {
      snprintf(qbuf, sizeof qbuf, "%s%s", qualityLabels[menu.qualityIndex],
               menu.qualityApplied != menu.qualityIndex ? "  [A]" : "");
      qbuf[sizeof qbuf - 1] = '\0';
      next = qbuf;
    }

    if (this->valueLabels[i] && next && this->valueLabels[i]->getFullText() != next) {
      this->valueLabels[i]->setText(next);
    }
  }
  this->applyHighlight(menu.row);
}

} // namespace hg
