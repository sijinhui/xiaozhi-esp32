#ifndef CHAT_LCD_DISPLAY_H
#define CHAT_LCD_DISPLAY_H

#include "lcd_display.h"
#include "lvgl_display/lvgl_theme.h"

#include "application.h"
#include "board.h"
#include "material_symbols.h"

#include <esp_log.h>
#include <esp_random.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// SpiLcdDisplay with the chat/history/voice-bar UI developed for the Waveshare
// round AMOLED boards. With round_panel the elements follow the circle: status
// text along the top arc and chat lines as wide as each chord. On rectangular
// panels the same elements are re-laid-out with constant line widths and a
// smaller centred emoji. touch_controls adds the emoji-tap and volume-arc
// gestures and is only supported on round panels; boards without a touch panel
// pass false and only lose those two gestures (a screen-wide swipe still pages
// the history when the panel does have touch). rounder_align rounds
// invalidated areas to 2-px boundaries (needed by the CO5300 AMOLED driver,
// independent of the panel's shape).
class ChatLcdDisplay : public SpiLcdDisplay {
public:
    ChatLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                   bool mirror_y, bool swap_xy, bool round_panel, bool touch_controls,
                   bool rounder_align = false)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy),
          round_panel_(round_panel),
          touch_controls_(touch_controls && round_panel),
          rounder_align_(rounder_align) {
        // The volume arc and its geometry only make sense on a round panel. The
        // member is masked above so a build compiled with NDEBUG stays safe too.
        if (touch_controls && !round_panel) {
            ESP_LOGE(kLogTag, "touch_controls is only supported on round panels; disabled");
        }
        assert(!touch_controls || round_panel);
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        if (rounder_align_) {
            lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        }
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.1, 0);
#else
        ApplyChatStyles();
        SetupChatView();
        UpdateChatLayout(true);
#endif
        if (touch_controls_) {
            SetupTouchControls();
        }
    }

    // Touch actions go to the board, which owns the chat state and the codec.
    // Both run on the LVGL task.
    void OnEmojiTap(std::function<void()> callback) { on_emoji_tap_ = std::move(callback); }
    void OnVolumeChange(std::function<void(int)> callback) {
        on_volume_change_ = std::move(callback);
    }

    // Loudness 0..100 of the microphone (true) or the speaker (false)
    void SetAudioLevelSource(std::function<int(bool)> source) {
        audio_level_ = std::move(source);
    }

    // The base only shows an icon when muted; this one always shows the level
    virtual void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);
        DisplayLockGuard lock(this);
        RefreshVolumeIcon(Board::GetInstance().GetAudioCodec()->output_volume());
    }

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    // The emoji stays at the centre and the chat history fills the lower part,
    // each line as wide as the panel allows at its height. The history outlives
    // the conversation and is read a page at a time by swiping.
    virtual void SetChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        AddChatMessage(role != nullptr ? role : "", content != nullptr ? content : "");
    }

    virtual void RemoveChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        if (content == nullptr || entries_.empty() || entries_.back().text != content) {
            return;
        }
        entries_.pop_back();
        OnContentChanged(true);
    }

    // Called when a conversation ends. The history stays; only the turn ends, so the
    // next answer starts a new paragraph.
    virtual void ClearChatMessages() override {
        DisplayLockGuard lock(this);
        if (RemoveTransientSystemEntry()) {
            OnContentChanged(true);
        }
        turn_open_ = false;
    }

    virtual void SetHideSubtitle(bool hide) override {
        DisplayLockGuard lock(this);
        hide_subtitle_ = hide;
        if (hide) {
            HideText();
        }
    }

    virtual void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        SpiLcdDisplay::SetEmotion(emotion);
        // A bigger emoji pushes the lines down; only then re-lay the pages
        if (UpdateChatLayout(false)) {
            OnContentChanged(true);
        }
    }

    virtual void SetTheme(Theme* theme) override {
        DisplayLockGuard lock(this);
        SpiLcdDisplay::SetTheme(theme);
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        // A theme pack swaps the fonts, so every measured width and height changes
        UpdateChatLayout(true);
        OnContentChanged(true);
        ApplyVolumeColors();
    }
#endif

private:
    static constexpr const char* kLogTag = "ChatLcdDisplay";

    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    static constexpr int kEmojiTapMargin = 24;    // taps this close to the emoji still count
    static constexpr int kTapSlop = 16;           // a press that moves further is a swipe
    static constexpr int kVolumeArcSpan = 40;     // degrees above and below 3 o'clock
    static constexpr int kVolumeArcInset = 6;     // gap between the arc and the panel edge
    static constexpr int kVolumeArcWidth = 10;
    static constexpr int kVolumeZoneWidth = 72;   // touch band along the right edge
    static constexpr int kVolumeDragStart = 8;    // movement before a touch becomes a drag
    static constexpr int kVolumeStep = 5;         // fewer settings writes while dragging
    static constexpr uint32_t kVolumeApplyMs = 120;  // at most this often while dragging
    static constexpr uint32_t kVolumeHideMs = 1500;

    bool round_panel_;
    bool touch_controls_;
    bool rounder_align_;
    std::function<void()> on_emoji_tap_;
    std::function<void(int)> on_volume_change_;
    std::function<int(bool)> audio_level_;
    lv_point_t emoji_press_point_ = {0, 0};
    lv_obj_t* volume_arc_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    lv_obj_t* volume_zone_ = nullptr;
    lv_timer_t* volume_hide_timer_ = nullptr;
    lv_point_t volume_press_point_ = {0, 0};
    bool volume_dragging_ = false;
    int volume_ = 0;          // shown on the arc
    int volume_applied_ = 0;  // last handed to the board
    uint32_t volume_applied_tick_ = 0;

    // Tap the emoji to start or end a chat. Drag along the right edge to set the
    // volume; a tap there only shows the current level.
    void SetupTouchControls() {
        DisplayLockGuard lock(this);
        lv_obj_t* screen = lv_screen_active();
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        // Panels drawn over the emoji's tap margin would swallow the tap
        for (lv_obj_t* obj : {top_bar_, status_bar_, bottom_bar_, low_battery_popup_}) {
            if (obj != nullptr) {
                lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            }
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(emoji_box_, kEmojiTapMargin);
            lv_obj_add_event_cb(emoji_box_, OnEmojiEvent, LV_EVENT_PRESSED, this);
            lv_obj_add_event_cb(emoji_box_, OnEmojiEvent, LV_EVENT_CLICKED, this);
        }

        // Display-only arc hugging the right edge; the zone below takes the touches
        volume_arc_ = lv_arc_create(screen);
        int diameter = width_ - 2 * kVolumeArcInset;
        lv_obj_set_size(volume_arc_, diameter, diameter);
        lv_obj_center(volume_arc_);
        lv_obj_remove_flag(volume_arc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(volume_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(volume_arc_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(volume_arc_, 0, LV_PART_MAIN);
        lv_obj_set_style_arc_width(volume_arc_, kVolumeArcWidth, LV_PART_MAIN);
        lv_obj_set_style_arc_width(volume_arc_, kVolumeArcWidth, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(volume_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(volume_arc_, 0, LV_PART_KNOB);
        lv_arc_set_bg_angles(volume_arc_, 360 - kVolumeArcSpan, kVolumeArcSpan);
        lv_obj_add_flag(volume_arc_, LV_OBJ_FLAG_HIDDEN);

        volume_label_ = lv_label_create(screen);
        lv_obj_set_style_radius(volume_label_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(volume_label_, LV_OPA_70, 0);
        lv_obj_set_style_pad_hor(volume_label_, 10, 0);
        lv_obj_set_style_pad_ver(volume_label_, 4, 0);
        lv_obj_align(volume_label_, LV_ALIGN_RIGHT_MID,
                     -(kVolumeArcInset + kVolumeArcWidth + 14), 0);
        lv_obj_add_flag(volume_label_, LV_OBJ_FLAG_HIDDEN);

        int reach = static_cast<int>((width_ / 2) *
                                     sinf(kVolumeArcSpan * static_cast<float>(M_PI) / 180));
        volume_zone_ = lv_obj_create(screen);
        lv_obj_set_size(volume_zone_, kVolumeZoneWidth, 2 * reach);
        lv_obj_align(volume_zone_, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_opa(volume_zone_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(volume_zone_, 0, 0);
        lv_obj_set_style_radius(volume_zone_, 0, 0);
        lv_obj_set_style_pad_all(volume_zone_, 0, 0);
        lv_obj_remove_flag(volume_zone_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(volume_zone_, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_remove_flag(volume_zone_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(volume_zone_, OnVolumeTouch, LV_EVENT_ALL, this);

        volume_hide_timer_ = lv_timer_create(OnVolumeHideTimer, kVolumeHideMs, this);
        lv_timer_pause(volume_hide_timer_);

        ApplyVolumeColors();
    }

    // Colours follow the theme so the arc reads on both light and dark backgrounds
    void ApplyVolumeColors() {
        if (volume_arc_ == nullptr || current_theme_ == nullptr) {
            return;
        }
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        lv_obj_set_style_arc_color(volume_arc_, lvgl_theme->text_color(), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(volume_arc_, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_arc_color(volume_arc_, lvgl_theme->text_color(), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(volume_arc_, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(volume_label_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_text_color(volume_label_, lvgl_theme->text_color(), 0);
    }

    void RefreshVolumeIcon(int volume) {
        if (mute_label_ == nullptr) {
            return;
        }
        const char* icon = volume <= 0  ? MATERIAL_SYMBOLS_VOLUME_OFF
                           : volume < 34 ? MATERIAL_SYMBOLS_VOLUME_MUTE
                           : volume < 67 ? MATERIAL_SYMBOLS_VOLUME_DOWN
                                         : MATERIAL_SYMBOLS_VOLUME_UP;
        if (strcmp(lv_label_get_text(mute_label_), icon) != 0) {
            lv_label_set_text(mute_label_, icon);
        }
    }

    // The indicator grows from the bottom end of the arc towards the top
    void ShowVolume(int volume) {
        int start = kVolumeArcSpan - 2 * kVolumeArcSpan * volume / 100;
        lv_arc_set_angles(volume_arc_, (start + 360) % 360, kVolumeArcSpan);
        lv_label_set_text_fmt(volume_label_, "%d", volume);
        RefreshVolumeIcon(volume);
    }

    // Maps a point to the volume at the same angle on the arc: top end 100, bottom 0
    int VolumeAtPoint(const lv_point_t& p) const {
        float deg = atan2f(static_cast<float>(p.y - height_ / 2),
                           static_cast<float>(p.x - width_ / 2)) * 180 / static_cast<float>(M_PI);
        deg = std::clamp(deg, -static_cast<float>(kVolumeArcSpan),
                         static_cast<float>(kVolumeArcSpan));
        float level = (kVolumeArcSpan - deg) / (2 * kVolumeArcSpan) * 100 / kVolumeStep;
        return static_cast<int>(lroundf(level)) * kVolumeStep;
    }

    // A swipe that starts on the emoji still ends in a click, since nothing under
    // it scrolls; only a press that stayed put counts as a tap.
    static void OnEmojiEvent(lv_event_t* e) {
        auto self = static_cast<ChatLcdDisplay*>(lv_event_get_user_data(e));
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
            self->emoji_press_point_ = p;
            return;
        }
        if (std::abs(p.x - self->emoji_press_point_.x) > kTapSlop ||
            std::abs(p.y - self->emoji_press_point_.y) > kTapSlop) {
            return;
        }
        if (self->on_emoji_tap_) {
            self->on_emoji_tap_();
        }
    }

    static void OnVolumeTouch(lv_event_t* e) {
        auto self = static_cast<ChatLcdDisplay*>(lv_event_get_user_data(e));
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_PRESSED) {
            lv_indev_get_point(lv_indev_active(), &self->volume_press_point_);
            self->volume_dragging_ = false;
            self->volume_ = Board::GetInstance().GetAudioCodec()->output_volume();
            self->volume_applied_ = self->volume_;
            self->ShowVolume(self->volume_);
            lv_timer_pause(self->volume_hide_timer_);
            lv_obj_remove_flag(self->volume_arc_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(self->volume_label_, LV_OBJ_FLAG_HIDDEN);
        } else if (code == LV_EVENT_PRESSING) {
            lv_point_t p;
            lv_indev_get_point(lv_indev_active(), &p);
            if (!self->volume_dragging_) {
                if (std::abs(p.x - self->volume_press_point_.x) < kVolumeDragStart &&
                    std::abs(p.y - self->volume_press_point_.y) < kVolumeDragStart) {
                    return;
                }
                self->volume_dragging_ = true;
            }
            int volume = self->VolumeAtPoint(p);
            if (volume != self->volume_) {
                self->volume_ = volume;
                self->ShowVolume(volume);
            }
            // Each applied step saves settings and clicks, so a fast swipe is thinned
            if (lv_tick_elaps(self->volume_applied_tick_) >= kVolumeApplyMs) {
                self->ApplyVolume();
            }
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            self->ApplyVolume();
            lv_timer_reset(self->volume_hide_timer_);
            lv_timer_resume(self->volume_hide_timer_);
        }
    }

    void ApplyVolume() {
        if (volume_ == volume_applied_) {
            return;
        }
        volume_applied_ = volume_;
        volume_applied_tick_ = lv_tick_get();
        if (on_volume_change_) {
            on_volume_change_(volume_);
        }
    }

    static void OnVolumeHideTimer(lv_timer_t* timer) {
        auto self = static_cast<ChatLcdDisplay*>(lv_timer_get_user_data(timer));
        lv_obj_add_flag(self->volume_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(self->volume_label_, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(timer);
    }

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    static constexpr int kEdgeMargin = 12;       // status text stays this far inside the edge
    static constexpr int kTextMargin = 24;       // chat lines keep this far inside the edge
    static constexpr int kPunctSlack = 8;        // closing punctuation may overrun a line by this
    static constexpr int kMinLineWidth = 130;    // narrower slots on the bottom arc are unused
    static constexpr int kPillMargin = 12;       // extra room between the icon pill and the arc
    static constexpr int kRowGap = 8;            // between the icon pill and the status line
    static constexpr int kTextGap = 14;          // between the emoji and the first line
    static constexpr int kEmojiSlot = 128;       // size of the theme's emoji images
    static constexpr size_t kMaxEntries = 40;    // older messages are dropped
    static constexpr uint32_t kTypeTickMs = 40;  // typewriter step
    static constexpr uint32_t kHideAfterMs = 15000;  // idle and untouched this long: hide text

    // Rectangular panels: same elements, laid out for square corners. The emoji
    // stays centred but shrinks to a small slot so a short panel keeps a few
    // chat lines.
    static constexpr int kRectTopMargin = 8;
    static constexpr int kRectBottomMargin = 12;
    static constexpr int kRectTextMargin = 20;
    static constexpr int kRectTextGap = 12;
    static constexpr int kRectEmojiSlot = 64;    // the 64px emoji of a rect board's assets pack draw unscaled
    static constexpr int kRectBarGap = 14;
    static constexpr int kRectMinLines = 2;

    struct ChatEntry {
        char role;         // 'u'ser, 'a'ssistant or 's'ystem
        std::string text;  // full text
        size_t shown;      // bytes of text revealed so far
    };
    struct TextPos {
        size_t entry;
        size_t offset;
    };
    struct Line {
        size_t entry;
        size_t from;
        size_t to;
        size_t next;  // where the line after this one starts (BreakLine's next)
    };

    std::deque<ChatEntry> entries_;
    std::vector<TextPos> pages_;    // where each page starts; a page repeats the
                                    // previous page's last line, re-wrapped
    size_t page_ = 0;               // page on screen
    bool following_ = true;         // stay on the newest page
    bool text_visible_ = false;
    bool turn_open_ = false;        // the next answer sentence joins the last paragraph
    lv_obj_t* text_box_ = nullptr;
    std::vector<lv_obj_t*> line_labels_;
    std::vector<int> line_widths_;  // widest each line may be, top line first
    int text_top_ = 0;
    int line_height_ = 0;
    lv_timer_t* type_timer_ = nullptr;
    lv_timer_t* hide_timer_ = nullptr;
    uint32_t last_content_tick_ = 0;

    // Half-length of the chord at distance d from the centre, on the circle shrunk
    // by margin. Gives both the half width that fits at a given height and the
    // vertical reach of a box with a given half width.
    int CircleExtent(int d, int margin) const {
        int r = width_ / 2 - margin;
        d = std::abs(d);
        return d >= r ? 0 : static_cast<int>(sqrtf(static_cast<float>(r * r - d * d)));
    }

    static const lv_font_t* TextFont() {
        return lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
    }

    // One-time restyling of the objects created by LcdDisplay::SetupUI(). Fonts are
    // inherited from the screen, never cached here: a theme pack frees the old ones.
    void ApplyChatStyles() {
        // Status icons: a compact pill at the top instead of a full-width bar
        lv_obj_set_width(top_bar_, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(top_bar_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_left(top_bar_, 14, 0);
        lv_obj_set_style_pad_right(top_bar_, 14, 0);
        lv_obj_set_style_pad_column(top_bar_, 8, 0);
        lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        // Status and notification text share one line under the icons
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

        // The chat lines below replace the single-line subtitle
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

        if (round_panel_) {
            // Low battery popup: narrower and lifted so its corners stay on the panel
            int popup_w = width_ * 64 / 100;
            lv_obj_set_width(low_battery_popup_, popup_w);
            lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0,
                         -(height_ / 2 - CircleExtent(popup_w / 2, kEdgeMargin)));
        }
    }

    void SetupChatView() {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        // Taps and swipes pass through to the screen underneath
        text_box_ = lv_obj_create(screen);
        lv_obj_set_style_bg_opa(text_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box_, 0, 0);
        lv_obj_set_style_radius(text_box_, 0, 0);
        lv_obj_set_style_pad_all(text_box_, 0, 0);
        lv_obj_remove_flag(text_box_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(text_box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(text_box_, LV_OBJ_FLAG_HIDDEN);

        // Swipe up for newer pages, down for older ones, either way to bring the
        // text back once it has been hidden. Harmless when there is no touch.
        lv_obj_add_event_cb(screen, OnScreenGesture, LV_EVENT_GESTURE, this);

        type_timer_ = lv_timer_create(OnTypeTimer, kTypeTickMs, this);
        lv_timer_pause(type_timer_);
        hide_timer_ = lv_timer_create(OnHideTimer, 1000, this);

        SetupVoiceBars();
    }

    // The default emoji art is sized for large round panels; a short rectangular
    // panel shrinks it so the chat keeps a few lines. LvglRawImage reports no
    // dimensions, so take the decoded size from the LVGL object, not the header.
    void FitEmojiImage(int slot) {
        if (emoji_image_ == nullptr || lv_obj_has_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }
        int native = std::max({1, static_cast<int>(lv_image_get_src_width(emoji_image_)),
                               static_cast<int>(lv_image_get_src_height(emoji_image_))});
        lv_image_set_scale(emoji_image_, std::min<uint32_t>(256, slot * 256u / native));
    }

    // Places the pill, the status line and the chat lines from measured sizes.
    // Returns true when the line slots changed.
    bool UpdateChatLayout(bool log_layout) {
        if (top_bar_ == nullptr || status_bar_ == nullptr || emoji_box_ == nullptr ||
            text_box_ == nullptr) {
            return false;
        }
        const int cy = height_ / 2;
        lv_obj_update_layout(top_bar_);

        // Leave room for one more icon so the pill never nears the arc
        int pill_h = lv_obj_get_height(top_bar_);
        int pill_w = lv_obj_get_width(top_bar_) + (round_panel_ ? pill_h : 0);
        int pill_y = round_panel_ ? cy - CircleExtent(pill_w / 2, kPillMargin) : kRectTopMargin;
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, pill_y);

        if (!round_panel_) {
            FitEmojiImage(EmojiSlot());
        }

        // The status line is as wide as the circle allows at its top edge
        int status_y = pill_y + pill_h + (round_panel_ ? kRowGap : 2);
        int status_w = round_panel_ ? 2 * CircleExtent(cy - status_y, kEdgeMargin)
                                    : width_ - 2 * kEdgeMargin;
        lv_obj_set_width(status_bar_, status_w);
        lv_obj_set_width(status_label_, status_w);
        lv_obj_set_width(notification_label_, status_w);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, status_y);
        lv_obj_update_layout(status_bar_);
        int status_bottom = status_y + lv_obj_get_height(status_bar_);

        const lv_font_t* font = TextFont();
        int line_h = std::max<int>(1, font->line_height);
        // emoji_box_ is sized by the image's unscaled source, so on a rectangular
        // panel the art drawn on screen is at most the slot FitEmojiImage() set.
        lv_obj_update_layout(emoji_box_);
        int emoji_h = lv_obj_get_height(emoji_box_);
        int shown_emoji_h = round_panel_ ? emoji_h : std::min(emoji_h, EmojiSlot());
        // Round: a bigger emoji pushes the text down, with a 128px floor for the
        // icon font. Rect: a fixed slot, so the layout does not follow the emoji.
        int emoji_slot = round_panel_ ? std::max(emoji_h, kEmojiSlot) : EmojiSlot();
        int text_top = cy + emoji_slot / 2 + (round_panel_ ? kTextGap : kRectTextGap);
        std::vector<int> widths;
        if (round_panel_) {
            for (int bottom = text_top + line_h; bottom - cy < width_ / 2; bottom += line_h) {
                int w = 2 * CircleExtent(bottom - cy, kTextMargin);
                if (w < kMinLineWidth) {
                    break;
                }
                widths.push_back(w);
            }
        } else {
            for (int bottom = text_top + line_h; bottom <= height_ - kRectBottomMargin;
                 bottom += line_h) {
                widths.push_back(width_ - 2 * kRectTextMargin);
            }
            // Keep at least a couple of lines even with a tall font, moving the
            // first lines up over the emoji if need be, but never over the status line
            while (static_cast<int>(widths.size()) < kRectMinLines &&
                   text_top - line_h >= status_bottom) {
                text_top -= line_h;
                widths.insert(widths.begin(), width_ - 2 * kRectTextMargin);
            }
        }
        if (widths.empty()) {
            widths.push_back(round_panel_
                                 ? std::max(2 * CircleExtent(text_top - cy, kTextMargin),
                                            4 * line_h)
                                 : width_ - 2 * kRectTextMargin);
        }

        if (log_layout) {
            ESP_LOGI(kLogTag,
                     "%s layout: pill y=%d-%d, status y=%d-%d w=%d, emoji y=%d-%d, "
                     "chat y=%d-%d, %d lines %d..%dpx",
                     round_panel_ ? "Round" : "Rect", pill_y, pill_y + pill_h, status_y,
                     status_bottom, status_w,
                     cy - shown_emoji_h / 2, cy + shown_emoji_h / 2, text_top,
                     text_top + line_h * static_cast<int>(widths.size()),
                     static_cast<int>(widths.size()), widths.front(), widths.back());
        }
        if (text_top == text_top_ && line_h == line_height_ && widths == line_widths_) {
            return false;
        }
        text_top_ = text_top;
        line_height_ = line_h;
        line_widths_ = std::move(widths);

        // One label per line slot, with some slack so closing punctuation allowed
        // past a line's width is not cut.
        lv_obj_clean(text_box_);
        line_labels_.clear();
        int box_w = line_widths_.front() + 2 * kPunctSlack;
        lv_obj_set_size(text_box_, box_w, line_h * static_cast<int>(line_widths_.size()));
        lv_obj_align(text_box_, LV_ALIGN_TOP_MID, 0, text_top);
        for (size_t i = 0; i < line_widths_.size(); i++) {
            lv_obj_t* label = lv_label_create(text_box_);
            lv_obj_set_width(label, line_widths_[i] + 2 * kPunctSlack);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(label, "");
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, static_cast<int>(i) * line_h);
            line_labels_.push_back(label);
        }
        return true;
    }

    int EmojiSlot() const { return std::max(1, std::min(kRectEmojiSlot, height_ / 2)); }

    // Decodes the UTF-8 code point at s[i] and advances i; a bad byte counts as one.
    static uint32_t NextCodepoint(const std::string& s, size_t& i, size_t limit) {
        unsigned char c = s[i];
        size_t len = 1;
        if ((c >> 5) == 0x6) {
            len = 2;
        } else if ((c >> 4) == 0xE) {
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            len = 4;
        }
        if (i + len > limit) {
            len = 1;
        }
        uint32_t cp = len == 1 ? c : c & (0x7F >> len);
        for (size_t k = 1; k < len; k++) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        i += len;
        return cp;
    }

    // Closing punctuation stays at the end of the previous line instead of starting one
    static bool NoLineStart(uint32_t cp) {
        static const uint32_t kClosing[] = {
            ',',    '.',    '!',    '?',    ';',    ':',    ')',    ']',
            0x2026, 0x3001, 0x3002, 0x300B, 0x300D, 0x300F, 0x3011, 0xFF01,
            0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF5E,
        };
        return std::find(std::begin(kClosing), std::end(kClosing), cp) != std::end(kClosing);
    }

    // Greedy break of text[start, limit) into one line at most max_w wide. CJK text
    // breaks anywhere, Latin words at spaces. Sets where the line ends and where
    // the next one starts.
    static void BreakLine(const std::string& text, size_t start, size_t limit,
                          const lv_font_t* font, int max_w, size_t& end, size_t& next) {
        size_t i = start;
        size_t space_break = std::string::npos;
        int w = 0;
        end = limit;
        next = limit;
        while (i < limit) {
            size_t j = i;
            uint32_t cp = NextCodepoint(text, j, limit);
            if (cp == '\n') {
                end = i;
                next = j;
                break;
            }
            size_t k = j;
            uint32_t following = k < limit ? NextCodepoint(text, k, limit) : 0;
            int glyph_w = lv_font_get_glyph_width(font, cp, following);
            if (w + glyph_w > max_w && i > start && !NoLineStart(cp)) {
                bool mid_word = cp < 0x80 && cp != ' ' && space_break != std::string::npos;
                end = mid_word ? space_break : i;
                next = end;
                break;
            }
            w += glyph_w;
            i = j;
            if (cp == ' ') {
                space_break = i;
            }
        }
        while (end > start && text[end - 1] == ' ') {
            end--;
        }
        while (next < limit && text[next] == ' ') {
            next++;
        }
    }

    // Skips past entries with nothing (more) to show. Returns false at the end.
    bool SkipFinished(TextPos& pos) const {
        while (pos.entry < entries_.size() && pos.offset >= entries_[pos.entry].shown) {
            pos.entry++;
            pos.offset = 0;
        }
        return pos.entry < entries_.size();
    }

    // Fills the line slots from pos; each message starts on a new line. Returns
    // true when everything shown so far fits.
    bool LayoutPage(TextPos pos, std::vector<Line>& lines) const {
        lines.clear();
        const lv_font_t* font = TextFont();
        for (int w : line_widths_) {
            if (!SkipFinished(pos)) {
                return true;
            }
            const auto& entry = entries_[pos.entry];
            size_t end, next;
            BreakLine(entry.text, pos.offset, entry.shown, font, w, end, next);
            lines.push_back({pos.entry, pos.offset, end, next});
            pos.offset = next;
        }
        return !SkipFinished(pos);
    }

    // Splits the history into pages. Text only ever grows at the end, so unless
    // something was removed only the last page needs another look.
    void Repaginate(bool full) {
        if (full || pages_.empty()) {
            pages_.assign(1, TextPos{0, 0});
        }
        std::vector<Line> lines;
        while (!LayoutPage(pages_.back(), lines) && !lines.empty()) {
            TextPos next;
            if (lines.size() == 1) {
                // A single line slot cannot repeat for context: start the next page
                // at BreakLine's next (past the '\n' or stripped trailing spaces),
                // or a one-line page would loop forever or stall on an empty line.
                next = TextPos{lines.back().entry, lines.back().next};
            } else {
                // The next page repeats this page's last line so the reader keeps context
                next = TextPos{lines.back().entry, lines.back().from};
            }
            if (pages_.back().entry == next.entry && pages_.back().offset == next.offset) {
                break;
            }
            pages_.push_back(next);
        }
    }

    void RenderPage() {
        if (line_labels_.empty() || current_theme_ == nullptr) {
            return;
        }
        std::vector<Line> lines;
        if (!pages_.empty() && !entries_.empty()) {
            LayoutPage(pages_[page_], lines);
        }
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        for (size_t i = 0; i < line_labels_.size(); i++) {
            lv_obj_t* label = line_labels_[i];
            if (i >= lines.size()) {
                lv_label_set_text(label, "");
                continue;
            }
            const auto& entry = entries_[lines[i].entry];
            std::string text = entry.text.substr(lines[i].from, lines[i].to - lines[i].from);
            lv_label_set_text(label, text.c_str());
            // The answer reads at full strength, the user's words and system lines dimmer
            lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
            lv_obj_set_style_text_opa(label, entry.role == 'a' ? LV_OPA_COVER : LV_OPA_60, 0);
        }
    }

    static void SetTranslateY(void* obj, int32_t v) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), v, 0);
    }

    static void SetOpa(void* obj, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), v, 0);
    }

    // A newer page rises from below, an older one drops in from above
    void AnimatePageTurn(bool newer) {
        lv_anim_delete(text_box_, nullptr);
        int shift = (newer ? 1 : -1) * line_height_ * 2;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, text_box_);
        lv_anim_set_duration(&a, 250);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, SetTranslateY);
        lv_anim_set_values(&a, shift, 0);
        lv_anim_start(&a);
        lv_anim_set_exec_cb(&a, SetOpa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_start(&a);
    }

    void OnContentChanged(bool full) {
        Repaginate(full);
        size_t last = pages_.size() - 1;
        if (following_ || page_ > last) {
            bool turned = page_ < last && text_visible_;
            page_ = last;
            following_ = true;
            if (turned) {
                AnimatePageTurn(true);
            }
        }
        RenderPage();
        if (entries_.empty()) {
            HideText();
        } else {
            ShowText();
        }
    }

    void ShowText() {
        last_content_tick_ = lv_tick_get();
        if (hide_subtitle_ || text_box_ == nullptr || entries_.empty() || text_visible_) {
            return;
        }
        text_visible_ = true;
        lv_obj_remove_flag(text_box_, LV_OBJ_FLAG_HIDDEN);
    }

    void HideText() {
        if (!text_visible_) {
            return;
        }
        text_visible_ = false;
        lv_obj_add_flag(text_box_, LV_OBJ_FLAG_HIDDEN);
        // Next time it shows, start from the newest page
        if (!following_ && !pages_.empty()) {
            following_ = true;
            page_ = pages_.size() - 1;
            RenderPage();
        }
    }

    void AddChatMessage(const std::string& role, const std::string& content) {
        if (text_box_ == nullptr) {
            return;
        }
        bool full = RemoveTransientSystemEntry();
        if (role == "system") {
            // System lines (errors, activation codes, hints) are transient: each
            // replaces the last one and an empty one clears it
            if (!content.empty()) {
                entries_.push_back({'s', content, content.size()});
            }
        } else if (role == "user") {
            if (!content.empty()) {
                entries_.push_back({'u', content, content.size()});
                turn_open_ = false;
                following_ = true;  // a new question jumps back to the newest page
            }
        } else if (!content.empty()) {  // an empty answer is no reason to clear history
            if (turn_open_ && !entries_.empty() && entries_.back().role == 'a') {
                auto& text = entries_.back().text;
                // Keep a space between Latin sentences; CJK runs on
                if (!text.empty() && static_cast<unsigned char>(text.back()) < 0x80 &&
                    text.back() != ' ' && static_cast<unsigned char>(content[0]) < 0x80) {
                    text += ' ';
                }
                text += content;
            } else {
                entries_.push_back({'a', content, 0});
            }
            turn_open_ = true;
            lv_timer_resume(type_timer_);
        }
        // Drop the oldest only while following, so a reader's page does not move
        while (entries_.size() > kMaxEntries && following_) {
            entries_.pop_front();
            full = true;
        }
        OnContentChanged(full);
    }

    bool RemoveTransientSystemEntry() {
        if (entries_.empty() || entries_.back().role != 's') {
            return false;
        }
        entries_.pop_back();
        return true;
    }

    // Reveals answer text a few characters at a time, faster when it falls behind
    static void OnTypeTimer(lv_timer_t* timer) {
        auto self = static_cast<ChatLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self->entries_.empty() || self->entries_.back().role != 'a' ||
            self->entries_.back().shown >= self->entries_.back().text.size()) {
            lv_timer_pause(timer);
            return;
        }
        auto& entry = self->entries_.back();
        size_t backlog = entry.text.size() - entry.shown;
        size_t steps = 1 + backlog / 90;  // ~30 CJK characters behind: two per tick
        while (steps-- > 0 && entry.shown < entry.text.size()) {
            unsigned char c = entry.text[entry.shown];
            size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
            entry.shown = std::min(entry.shown + len, entry.text.size());
        }
        self->OnContentChanged(false);
    }

    // Hides the text once the device is idle and nobody has touched it for a while
    static void OnHideTimer(lv_timer_t* timer) {
        auto self = static_cast<ChatLcdDisplay*>(lv_timer_get_user_data(timer));
        if (!self->text_visible_ ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            self->last_content_tick_ = lv_tick_get();
            return;
        }
        if (lv_tick_elaps(self->last_content_tick_) > kHideAfterMs &&
            lv_display_get_inactive_time(self->display_) > kHideAfterMs) {
            self->HideText();
        }
    }

    static void OnScreenGesture(lv_event_t* e) {
        auto self = static_cast<ChatLcdDisplay*>(lv_event_get_user_data(e));
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if ((dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) || self->pages_.empty()) {
            return;
        }
        if (!self->text_visible_) {
            self->ShowText();
            return;
        }
        size_t last = self->pages_.size() - 1;
        if (dir == LV_DIR_TOP && self->page_ < last) {
            self->page_++;
            self->AnimatePageTurn(true);
        } else if (dir == LV_DIR_BOTTOM && self->page_ > 0) {
            self->page_--;
            self->AnimatePageTurn(false);
        } else {
            return;
        }
        self->following_ = self->page_ == last;
        self->RenderPage();
        self->last_content_tick_ = lv_tick_get();
    }

    // ---- Voice bars: either side of the emoji, following the microphone while
    // listening and the speaker while speaking

    static constexpr int kBarCount = 6;  // per side
    static constexpr int kBarInner = 84;  // first bar, from the centre (round panel)
    static constexpr int kBarPitch = 14;
    static constexpr int kBarWidth = 6;
    static constexpr int kBarMinHeight = 6;
    static constexpr int kBarMaxHeight = 56;
    static constexpr uint32_t kBarTickMs = 50;

    int bar_inner_ = kBarInner;
    int bar_pitch_ = kBarPitch;
    int bar_width_ = kBarWidth;
    int bar_min_height_ = kBarMinHeight;
    int bar_max_height_ = kBarMaxHeight;
    lv_obj_t* bars_[2 * kBarCount] = {};
    lv_timer_t* bar_timer_ = nullptr;
    int bar_level_ = 0;  // smoothed: rises at once, falls gently
    bool bars_visible_ = false;

    void SetupVoiceBars() {
        if (!round_panel_) {
            bar_inner_ = EmojiSlot() / 2 + kRectBarGap;
            bar_pitch_ = std::max(8, width_ / 32);
            bar_width_ = std::max(4, width_ / 64);
            bar_min_height_ = std::max(4, bar_width_);
            bar_max_height_ = std::min(kBarMaxHeight, height_ / 4);
        }
        lv_obj_t* screen = lv_screen_active();
        for (int side = 0; side < 2; side++) {
            for (int i = 0; i < kBarCount; i++) {
                lv_obj_t* bar = lv_obj_create(screen);
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_set_style_pad_all(bar, 0, 0);
                lv_obj_set_style_radius(bar, bar_width_ / 2, 0);
                lv_obj_set_style_bg_opa(bar, 230 - i * 30, 0);
                int x = (side == 0 ? -1 : 1) * (bar_inner_ + i * bar_pitch_);
                lv_obj_set_size(bar, bar_width_, bar_min_height_);
                lv_obj_align(bar, LV_ALIGN_CENTER, x, 0);
                lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
                bars_[side * kBarCount + i] = bar;
            }
        }
        bar_timer_ = lv_timer_create(OnBarTimer, kBarTickMs, this);
    }

    void SetBarsVisible(bool visible) {
        if (visible == bars_visible_) {
            return;
        }
        bars_visible_ = visible;
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        for (lv_obj_t* bar : bars_) {
            if (visible) {
                lv_obj_set_style_bg_color(bar, lvgl_theme->text_color(), 0);
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
            }
        }
        bar_level_ = 0;
    }

    static void OnBarTimer(lv_timer_t* timer) {
        auto self = static_cast<ChatLcdDisplay*>(lv_timer_get_user_data(timer));
        auto state = Application::GetInstance().GetDeviceState();
        bool listening = state == kDeviceStateListening;
        bool speaking = state == kDeviceStateSpeaking;
        self->SetBarsVisible((listening || speaking) && self->audio_level_ != nullptr);
        if (!self->bars_visible_) {
            return;
        }
        int level = self->audio_level_(listening);
        self->bar_level_ = std::max(level, self->bar_level_ * 4 / 5);
        // Taller in the middle of each group, with a little jitter so it looks alive
        static const int kShape[kBarCount] = {45, 75, 100, 70, 90, 50};
        for (int side = 0; side < 2; side++) {
            for (int i = 0; i < kBarCount; i++) {
                int jitter = 85 + static_cast<int>(esp_random() % 31);
                int h = self->bar_min_height_ +
                        (self->bar_max_height_ - self->bar_min_height_) * self->bar_level_ *
                            kShape[i] / 100 * jitter / 100 / 100;
                h = std::clamp(h, self->bar_min_height_, self->bar_max_height_);
                lv_obj_set_height(self->bars_[side * kBarCount + i], h);
            }
        }
    }
#endif
};

#endif  // CHAT_LCD_DISPLAY_H
