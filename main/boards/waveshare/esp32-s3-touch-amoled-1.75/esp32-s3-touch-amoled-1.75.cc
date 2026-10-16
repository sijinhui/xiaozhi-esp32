#include "wifi_board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <material_symbols.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#define TAG "WaveshareEsp32s3TouchAMOLED1inch75"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

// 在waveshare_amoled_1_75类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
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

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.1, 0);
#else
        ApplyRoundStyles();
        SetupChatView();
        UpdateRoundLayout(true);
#endif
        SetupTouchControls();
    }

    // Touch actions go to the board, which owns the chat state and the codec.
    // Both run on the LVGL task.
    void OnEmojiTap(std::function<void()> callback) { on_emoji_tap_ = std::move(callback); }
    void OnVolumeChange(std::function<void(int)> callback) {
        on_volume_change_ = std::move(callback);
    }

    // The base only shows an icon when muted; this one always shows the level
    virtual void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);
        DisplayLockGuard lock(this);
        RefreshVolumeIcon(Board::GetInstance().GetAudioCodec()->output_volume());
    }

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Round 466x466 panel. With nothing to read, a large emoji sits at the centre.
    // Once there is text, the emoji shrinks to the bottom arc and a scrollable chat
    // history fills the middle. The history outlives the conversation and can be
    // reopened by swiping up or down on the face.
    virtual void SetChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        AddChatMessage(role != nullptr ? role : "", content != nullptr ? content : "");
    }

    virtual void RemoveChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        if (content == nullptr || entries_.empty() || entries_.back().text != content) {
            return;
        }
        DeleteLastEntry();
    }

    // Called when a conversation ends. The history stays; only the turn ends, so the
    // next answer starts a new paragraph.
    virtual void ClearChatMessages() override {
        DisplayLockGuard lock(this);
        RemoveTransientSystemEntry();
        turn_open_ = false;
    }

    virtual void SetHideSubtitle(bool hide) override {
        DisplayLockGuard lock(this);
        hide_subtitle_ = hide;
        if (hide) {
            ShowFace();
        }
    }

    virtual void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        SpiLcdDisplay::SetEmotion(emotion);
        // Emoji images are laid out unscaled; re-apply the current mode's transform
        SetEmojiProgress(chat_visible_ ? 1000 : 0);
    }

    virtual void SetTheme(Theme* theme) override {
        DisplayLockGuard lock(this);
        SpiLcdDisplay::SetTheme(theme);
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        // A theme pack swaps the fonts, so every measured height changes
        UpdateRoundLayout(true);
        ApplyChatColors();
        ApplyVolumeColors();
    }
#endif

private:
    static constexpr int kEmojiTapMargin = 24;    // taps this close to the emoji still count
    static constexpr int kTapSlop = 16;           // a press that moves further is a swipe
    static constexpr int kVolumeArcSpan = 40;     // degrees above and below 3 o'clock
    static constexpr int kVolumeArcInset = 6;     // gap between the arc and the panel edge
    static constexpr int kVolumeArcWidth = 10;
    static constexpr int kVolumeZoneWidth = 72;   // touch band along the right edge
    static constexpr int kVolumeDragStart = 8;    // movement before a touch becomes a drag
    static constexpr int kVolumeStep = 5;         // fewer settings writes while dragging
    static constexpr uint32_t kVolumeHideMs = 1500;

    std::function<void()> on_emoji_tap_;
    std::function<void(int)> on_volume_change_;
    lv_point_t emoji_press_point_ = {0, 0};
    lv_obj_t* volume_arc_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    lv_obj_t* volume_zone_ = nullptr;
    lv_timer_t* volume_hide_timer_ = nullptr;
    lv_point_t volume_press_point_ = {0, 0};
    bool volume_dragging_ = false;
    int volume_ = 0;

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

        int reach = static_cast<int>((width_ / 2) * sinf(kVolumeArcSpan * static_cast<float>(M_PI) / 180));
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
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
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
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_PRESSED) {
            lv_indev_get_point(lv_indev_active(), &self->volume_press_point_);
            self->volume_dragging_ = false;
            self->volume_ = Board::GetInstance().GetAudioCodec()->output_volume();
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
                if (self->on_volume_change_) {
                    self->on_volume_change_(volume);
                }
            }
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            lv_timer_reset(self->volume_hide_timer_);
            lv_timer_resume(self->volume_hide_timer_);
        }
    }

    static void OnVolumeHideTimer(lv_timer_t* timer) {
        auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        lv_obj_add_flag(self->volume_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(self->volume_label_, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(timer);
    }

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    static constexpr int kEdgeMargin = 12;      // text stays this far inside the panel edge
    static constexpr int kPillMargin = 20;      // extra room between the icon pill and the arc
    static constexpr int kRowGap = 8;           // between the icon pill and the status line
    static constexpr int kChatGap = 6;          // between the status line and the history
    static constexpr int kEntryGap = 10;        // between two messages
    static constexpr int kEmojiSlot = 128;      // size of the theme's emoji images
    static constexpr int kSmallEmoji = 64;      // emoji size while the history shows
    static constexpr int kSmallEmojiMargin = 14;  // small emoji to the bottom of the panel
    static constexpr size_t kMaxEntries = 40;   // older messages are dropped
    static constexpr uint32_t kTypeTickMs = 40; // typewriter step
    static constexpr uint32_t kFaceAfterMs = 15000;  // idle and untouched this long: face

    struct ChatEntry {
        lv_obj_t* label;
        char role;         // 'u'ser, 'a'ssistant or 's'ystem
        std::string text;  // full text; the label may still be revealing it
        size_t shown;      // bytes of text on the label
    };

    std::deque<ChatEntry> entries_;
    lv_obj_t* chat_ = nullptr;
    lv_timer_t* type_timer_ = nullptr;
    lv_timer_t* face_timer_ = nullptr;
    bool chat_visible_ = false;
    bool follow_ = true;          // keep the newest line in view
    bool chat_pressed_ = false;   // a finger is on the history
    bool turn_open_ = false;      // the next answer sentence joins the last paragraph
    uint32_t last_content_tick_ = 0;
    int small_emoji_offset_ = 0;  // centre of the small emoji, below the panel centre

    // Half-length of the chord at distance d from the centre, on the circle shrunk
    // by margin. Gives both the half width that fits at a given height and the
    // vertical reach of a box with a given half width.
    int CircleExtent(int d, int margin) const {
        int r = width_ / 2 - margin;
        d = std::abs(d);
        return d >= r ? 0 : static_cast<int>(sqrtf(static_cast<float>(r * r - d * d)));
    }

    // One-time restyling of the objects created by LcdDisplay::SetupUI(). Fonts are
    // inherited from the screen, never cached here: a theme pack frees the old ones.
    void ApplyRoundStyles() {
        // Status icons: a compact pill on the top arc instead of a full-width bar
        lv_obj_set_width(top_bar_, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(top_bar_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_left(top_bar_, 14, 0);
        lv_obj_set_style_pad_right(top_bar_, 14, 0);
        lv_obj_set_style_pad_column(top_bar_, 8, 0);
        lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        // Status and notification text share one line under the icons
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

        // The chat history replaces the single-line subtitle
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

        // The emoji scales around its own centre
        lv_obj_set_style_transform_pivot_x(emoji_box_, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(emoji_box_, lv_pct(50), 0);

        // Low battery popup: narrower and lifted so its corners stay on the panel
        int popup_w = width_ * 64 / 100;
        lv_obj_set_width(low_battery_popup_, popup_w);
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0,
                     -(height_ / 2 - CircleExtent(popup_w / 2, kEdgeMargin)));
    }

    void SetupChatView() {
        lv_obj_t* screen = lv_screen_active();
        chat_ = lv_obj_create(screen);
        lv_obj_set_style_bg_opa(chat_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chat_, 0, 0);
        lv_obj_set_style_radius(chat_, 0, 0);
        lv_obj_set_style_pad_all(chat_, 0, 0);
        lv_obj_set_style_pad_row(chat_, kEntryGap, 0);
        lv_obj_set_flex_flow(chat_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(chat_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(chat_, LV_DIR_VER);
        lv_obj_add_event_cb(chat_, OnChatEvent, LV_EVENT_ALL, this);
        lv_obj_add_flag(chat_, LV_OBJ_FLAG_HIDDEN);

        // A swipe on the face brings the history back
        lv_obj_add_event_cb(screen, OnScreenGesture, LV_EVENT_GESTURE, this);

        type_timer_ = lv_timer_create(OnTypeTimer, kTypeTickMs, this);
        lv_timer_pause(type_timer_);
        face_timer_ = lv_timer_create(OnFaceTimer, 1000, this);
        ApplyChatColors();
    }

    void ApplyChatColors() {
        if (current_theme_ == nullptr) {
            return;
        }
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        for (auto& entry : entries_) {
            StyleEntry(entry, lvgl_theme);
        }
    }

    // The answer reads at full strength, the user's words and system lines dimmer
    void StyleEntry(const ChatEntry& entry, LvglTheme* lvgl_theme) {
        lv_obj_set_style_text_color(entry.label, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_opa(entry.label, entry.role == 'a' ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_set_style_text_align(entry.label,
                                    entry.role == 'u'   ? LV_TEXT_ALIGN_RIGHT
                                    : entry.role == 's' ? LV_TEXT_ALIGN_CENTER
                                                        : LV_TEXT_ALIGN_LEFT,
                                    0);
    }

    // Places the pill, the status line, the history and the small emoji from
    // measured sizes.
    void UpdateRoundLayout(bool log_layout) {
        if (top_bar_ == nullptr || status_bar_ == nullptr || emoji_box_ == nullptr ||
            chat_ == nullptr) {
            return;
        }
        const int cy = height_ / 2;
        lv_obj_update_layout(top_bar_);

        // Leave room for one more icon so the pill never nears the arc
        int pill_h = lv_obj_get_height(top_bar_);
        int pill_w = lv_obj_get_width(top_bar_) + pill_h;
        int pill_y = cy - CircleExtent(pill_w / 2, kPillMargin);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, pill_y);

        // The status line is as wide as the circle allows at its top edge
        int status_y = pill_y + pill_h + kRowGap;
        int status_w = 2 * CircleExtent(cy - status_y, kEdgeMargin);
        lv_obj_set_width(status_bar_, status_w);
        lv_obj_set_width(status_label_, status_w);
        lv_obj_set_width(notification_label_, status_w);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, status_y);
        lv_obj_update_layout(status_bar_);
        int status_h = lv_obj_get_height(status_bar_);

        // The small emoji sits on the bottom arc, the history between it and the
        // status line, as wide as fits at its farther edge and clear of the volume band
        int small_top = height_ - kSmallEmojiMargin - kSmallEmoji;
        small_emoji_offset_ = small_top + kSmallEmoji / 2 - cy;
        int chat_top = status_y + status_h + kChatGap;
        int chat_bottom = small_top - kChatGap;
        int far = std::max(cy - chat_top, chat_bottom - cy);
        int chat_w = std::min(2 * CircleExtent(far, kEdgeMargin), width_ - 2 * kVolumeZoneWidth);
        lv_obj_set_size(chat_, chat_w, chat_bottom - chat_top);
        lv_obj_align(chat_, LV_ALIGN_TOP_MID, 0, chat_top);
        SetEmojiProgress(chat_visible_ ? 1000 : 0);

        if (log_layout) {
            ESP_LOGI(TAG, "Round layout: pill y=%d-%d, status y=%d-%d w=%d, chat y=%d-%d w=%d, "
                     "small emoji y=%d-%d",
                     pill_y, pill_y + pill_h, status_y, status_y + status_h, status_w,
                     chat_top, chat_bottom, chat_w, small_top, small_top + kSmallEmoji);
        }
    }

    // 0: large emoji at the centre, 1000: small emoji on the bottom arc
    void SetEmojiProgress(int32_t v) {
        if (emoji_box_ == nullptr) {
            return;
        }
        lv_obj_update_layout(emoji_box_);
        int size = std::max<int>(lv_obj_get_height(emoji_box_), kEmojiSlot);
        int small_scale = 256 * kSmallEmoji / size;
        lv_obj_set_style_transform_scale(emoji_box_, 256 - (256 - small_scale) * v / 1000, 0);
        lv_obj_set_style_translate_y(emoji_box_, small_emoji_offset_ * v / 1000, 0);
    }

    static void EmojiAnim(lv_anim_t* a, int32_t v) {
        static_cast<CustomLcdDisplay*>(lv_anim_get_user_data(a))->SetEmojiProgress(v);
    }

    void AnimateEmoji(bool to_small) {
        lv_anim_delete(emoji_box_, nullptr);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, emoji_box_);
        lv_anim_set_user_data(&a, this);
        lv_anim_set_custom_exec_cb(&a, EmojiAnim);
        lv_anim_set_values(&a, to_small ? 0 : 1000, to_small ? 1000 : 0);
        lv_anim_set_duration(&a, 250);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    void ShowChat() {
        if (hide_subtitle_ || chat_ == nullptr || entries_.empty()) {
            return;
        }
        last_content_tick_ = lv_tick_get();
        if (chat_visible_) {
            return;
        }
        chat_visible_ = true;
        lv_obj_remove_flag(chat_, LV_OBJ_FLAG_HIDDEN);
        follow_ = true;
        ScrollToEnd();
        AnimateEmoji(true);
    }

    void ShowFace() {
        if (!chat_visible_) {
            return;
        }
        chat_visible_ = false;
        lv_obj_add_flag(chat_, LV_OBJ_FLAG_HIDDEN);
        AnimateEmoji(false);
    }

    void ScrollToEnd() {
        if (!follow_ || chat_pressed_ || !chat_visible_) {
            return;
        }
        lv_obj_update_layout(chat_);
        lv_obj_scroll_to_y(chat_, LV_COORD_MAX, LV_ANIM_OFF);
    }

    void AddChatMessage(const std::string& role, const std::string& content) {
        if (chat_ == nullptr) {
            return;
        }
        if (role == "system") {
            // System lines (errors, activation codes, hints) are transient: each
            // replaces the last one and an empty one clears it
            RemoveTransientSystemEntry();
            if (!content.empty()) {
                AppendEntry('s', content, true);
            }
        } else if (role == "user") {
            RemoveTransientSystemEntry();
            if (content.empty()) {
                return;
            }
            AppendEntry('u', content, true);
            turn_open_ = false;
            follow_ = true;
        } else {
            if (content.empty()) {
                return;  // the answer is never cleared, it stays in the history
            }
            RemoveTransientSystemEntry();
            if (turn_open_ && !entries_.empty() && entries_.back().role == 'a') {
                auto& text = entries_.back().text;
                // Keep a space between Latin sentences; CJK runs on
                if (!text.empty() && static_cast<unsigned char>(text.back()) < 0x80 &&
                    text.back() != ' ' && static_cast<unsigned char>(content[0]) < 0x80) {
                    text += ' ';
                }
                text += content;
            } else {
                AppendEntry('a', content, false);
            }
            turn_open_ = true;
            lv_timer_resume(type_timer_);
        }
        if (entries_.empty()) {
            ShowFace();
            return;
        }
        ShowChat();
        ScrollToEnd();
    }

    void AppendEntry(char role, const std::string& text, bool show_all) {
        lv_obj_t* label = lv_label_create(chat_);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(label, show_all ? text.c_str() : "");
        entries_.push_back({label, role, text, show_all ? text.size() : 0});
        StyleEntry(entries_.back(), static_cast<LvglTheme*>(current_theme_));
        // Drop the oldest only while following, so a reader's page does not jump
        while (entries_.size() > kMaxEntries && follow_) {
            lv_obj_delete(entries_.front().label);
            entries_.pop_front();
        }
    }

    void DeleteLastEntry() {
        lv_obj_delete(entries_.back().label);
        entries_.pop_back();
        if (entries_.empty()) {
            ShowFace();
        }
    }

    void RemoveTransientSystemEntry() {
        if (!entries_.empty() && entries_.back().role == 's') {
            DeleteLastEntry();
        }
    }

    // Reveals answer text a few characters at a time, faster when it falls behind
    static void OnTypeTimer(lv_timer_t* timer) {
        auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self->entries_.empty() || self->entries_.back().role != 'a') {
            lv_timer_pause(timer);
            return;
        }
        auto& entry = self->entries_.back();
        if (entry.shown >= entry.text.size()) {
            lv_timer_pause(timer);
            return;
        }
        size_t backlog = entry.text.size() - entry.shown;
        size_t steps = 1 + backlog / 90;  // ~30 CJK characters behind: two per tick
        while (steps-- > 0 && entry.shown < entry.text.size()) {
            unsigned char c = entry.text[entry.shown];
            size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
            entry.shown = std::min(entry.shown + len, entry.text.size());
        }
        lv_label_set_text(entry.label, entry.text.substr(0, entry.shown).c_str());
        self->last_content_tick_ = lv_tick_get();
        self->ScrollToEnd();
    }

    // Back to the face once the device is idle and nobody has touched it for a while
    static void OnFaceTimer(lv_timer_t* timer) {
        auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (!self->chat_visible_ || self->chat_pressed_ ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            self->last_content_tick_ = lv_tick_get();
            return;
        }
        if (lv_tick_elaps(self->last_content_tick_) > kFaceAfterMs &&
            lv_display_get_inactive_time(self->display_) > kFaceAfterMs) {
            self->ShowFace();
        }
    }

    static void OnChatEvent(lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        switch (lv_event_get_code(e)) {
            case LV_EVENT_PRESSED:
                self->chat_pressed_ = true;
                break;
            case LV_EVENT_RELEASED:
            case LV_EVENT_PRESS_LOST:
                self->chat_pressed_ = false;
                self->follow_ = lv_obj_get_scroll_bottom(self->chat_) <= 8;
                break;
            case LV_EVENT_SCROLL_END:
                // Scrolled back to the end: follow new text again
                self->follow_ = lv_obj_get_scroll_bottom(self->chat_) <= 8;
                break;
            default:
                break;
        }
    }

    static void OnScreenGesture(lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (!self->chat_visible_ && (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM)) {
            self->ShowChat();
        }
    }
#endif
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED1inch75 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // Shared by the BOOT button and a tap on the emoji
    void HandleChatClick() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() { HandleChatClick(); });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lv_indev_t* indev = lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");

        // The LVGL task delivers these; hand the work to the main task, which owns
        // the chat state, the codec and the power save timer.
        lvgl_port_lock(0);
        lv_indev_add_event_cb(indev, [](lv_event_t* e) {
            auto board = static_cast<WaveshareEsp32s3TouchAMOLED1inch75*>(lv_event_get_user_data(e));
            Application::GetInstance().Schedule([board]() { board->power_save_timer_->WakeUp(); });
        }, LV_EVENT_PRESSED, this);
        lvgl_port_unlock();
        display_->OnEmojiTap([this]() {
            Application::GetInstance().Schedule([this]() { HandleChatClick(); });
        });
        display_->OnVolumeChange([this](int volume) {
            Application::GetInstance().Schedule([this, volume]() {
                GetAudioCodec()->SetOutputVolume(volume);
            });
        });
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED1inch75() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch75);
