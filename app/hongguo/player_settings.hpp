// player_settings.hpp -- the player's own settings panel, opened by the control bar's 设置 chip.
//
// GPL-3.0-only.
//
// WHY A SECOND PANEL AND NOT ANOTHER ROW IN THE PICTURE MENU. They answer different questions and the
// reference layout keeps them apart: X is the PICTURE (brightness, aspect, quality -- things you adjust
// while watching and forget), and this is the PLAYER (a sleep timer, whether the progress bar stays on --
// things you set once). Mixing a sleep timer into a panel called 画面菜单 would be the same mistake as
// putting the volume there.
//
// WHAT IS DELIBERATELY NOT HERE. The reference's 通用 group has eight rows. Four of them cannot be done on
// this app, and docs/待做清单.md records each with its reason rather than shipping a row that does nothing:
//
//   着色器 / 均衡器        no shader or audio-filter chain on this render path (we are on mpv's software
//                          output; there is no GPU filter stage to hand a shader to)
//   高能进度条             the highlight-bar data belongs to bilibili's API and this service has none
//   上传历史播放记录        that is bilibili's cloud history; this app keeps a LOCAL one
//   播完退出全屏           this app is always fullscreen -- there is no windowed mode to leave
//   ─ and 播放方式 (what happens at the end) is waiting on F1: there is no episode switching yet, so
//     "auto-next" has nothing to advance to.
//
// ASCII-only source file.

#pragma once

#include <borealis.hpp>
#include <string>

namespace hg {

class PlayerSettingsPanel : public brls::Box {
  public:
    PlayerSettingsPanel();

    static brls::View* create();

    /* The rows, in order. The list is short and explicit because every row here is a feature that exists. */
    enum Row {
        ROW_SLEEP = 0,   /* 定时关闭 */
        ROW_BAR,         /* 固定显示进度条 */
        ROW_PLAY,        /* 播放方式：停在结尾 / 自动下一集 */
        ROW_COUNT
    };

    /* Show or hide. The panel is created hidden and is only ever driven by keys, like the picture menu. */
    void setPanelVisible(bool show);

    /* Re-draw the values. `sleepIndex` is an index into hg_ctl's ladder (0 = off) and `barFlag` is 0/1. */
    void setValues(int sleepIndex, int barFlag, int playMode);
    /* Move the highlight. */
    void setHighlight(int row);

  private:
    brls::Label* valueLabels[ROW_COUNT] = {nullptr, nullptr, nullptr};
    brls::Box* rowBoxes[ROW_COUNT] = {nullptr, nullptr, nullptr};
    int shownSleep = -2;
    int shownBar = -2;
    int shownPlay = -2;
    int shownRow = -1;
};

} // namespace hg
