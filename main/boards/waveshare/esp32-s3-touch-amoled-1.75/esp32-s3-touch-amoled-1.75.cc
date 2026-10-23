#include "wifi_board.h"
#include "display/chat_lcd_display.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "codecs/metered_box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_random.h>
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
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#define TAG "WaveshareEsp32s3TouchAMOLED1inch75"

// A 60 ms click (Ogg Opus, 16 kHz mono) played for each volume step while dragging
static const uint8_t kVolumeTickOgg[] = {
    0x4f, 0x67, 0x67, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0xee,
    0x70, 0x18, 0x00, 0x00, 0x00, 0x00, 0xd1, 0xc5, 0xfa, 0x55, 0x01, 0x13, 0x4f, 0x70, 0x75, 0x73,
    0x48, 0x65, 0x61, 0x64, 0x01, 0x01, 0x78, 0x00, 0x80, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f,
    0x67, 0x67, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0xee, 0x70,
    0x18, 0x01, 0x00, 0x00, 0x00, 0x49, 0x50, 0xac, 0x66, 0x01, 0x3e, 0x4f, 0x70, 0x75, 0x73, 0x54,
    0x61, 0x67, 0x73, 0x0d, 0x00, 0x00, 0x00, 0x4c, 0x61, 0x76, 0x66, 0x36, 0x30, 0x2e, 0x31, 0x36,
    0x2e, 0x31, 0x30, 0x30, 0x01, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x65, 0x6e, 0x63, 0x6f,
    0x64, 0x65, 0x72, 0x3d, 0x4c, 0x61, 0x76, 0x63, 0x36, 0x30, 0x2e, 0x33, 0x31, 0x2e, 0x31, 0x30,
    0x32, 0x20, 0x6c, 0x69, 0x62, 0x6f, 0x70, 0x75, 0x73, 0x4f, 0x67, 0x67, 0x53, 0x00, 0x04, 0xb8,
    0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0xee, 0x70, 0x18, 0x02, 0x00, 0x00, 0x00, 0xe7,
    0x36, 0x9f, 0x24, 0x03, 0xff, 0x8b, 0x7c, 0xbb, 0x83, 0x7f, 0x86, 0x9f, 0xf8, 0xfe, 0x62, 0x5a,
    0xf2, 0x72, 0x18, 0xf9, 0x30, 0xbd, 0xec, 0x00, 0xad, 0x50, 0xfc, 0xca, 0xa3, 0x9c, 0x55, 0x09,
    0xdf, 0x63, 0xa9, 0xdf, 0xe2, 0xae, 0x5b, 0x78, 0x67, 0x0f, 0xd1, 0x53, 0x65, 0x6a, 0x51, 0x33,
    0x0f, 0x61, 0x1a, 0x7b, 0xa4, 0x64, 0x70, 0xe7, 0xc5, 0x3e, 0x77, 0x28, 0x24, 0x30, 0x00, 0x81,
    0xc6, 0xde, 0x4f, 0x8d, 0x1c, 0x5c, 0x25, 0x5a, 0x97, 0x9c, 0xff, 0xea, 0x95, 0xf0, 0x81, 0xb3,
    0x33, 0x3a, 0xc6, 0xd2, 0xc8, 0x21, 0xc0, 0x95, 0x80, 0x10, 0x29, 0x1e, 0x64, 0x51, 0xd3, 0xb0,
    0x64, 0xb2, 0x16, 0x01, 0x0d, 0x46, 0xdc, 0x05, 0xe9, 0x24, 0x95, 0xcf, 0x88, 0x08, 0x25, 0x0d,
    0xe7, 0xaf, 0x7f, 0x33, 0x24, 0xb7, 0xb6, 0x82, 0x63, 0x30, 0x05, 0xa7, 0xc7, 0x11, 0x47, 0x07,
    0x01, 0x7d, 0x56, 0x8d, 0xf5, 0xd5, 0x5f, 0xb4, 0x6e, 0xb7, 0x9f, 0xca, 0x97, 0x99, 0x05, 0x62,
    0x07, 0xc5, 0x6b, 0x17, 0xdd, 0x49, 0xb9, 0x7e, 0x36, 0x4a, 0x03, 0x87, 0x08, 0xbf, 0x7e, 0x13,
    0x79, 0x6d, 0xfc, 0xbb, 0x6f, 0xb9, 0x58, 0x0c, 0xed, 0xa0, 0xd4, 0x8f, 0x2c, 0xa7, 0xb2, 0x7f,
    0x3e, 0xeb, 0xa7, 0x2a, 0x45, 0xa8, 0x8b, 0x6b, 0x17, 0xfa, 0x3f, 0xbc, 0xf0, 0x61, 0xc0, 0xc6,
    0xc4, 0xc8, 0x73, 0x35, 0x51, 0xde, 0x4d, 0x62, 0x97, 0x43, 0xd4, 0x07, 0x34, 0x21, 0x2a, 0xd3,
    0x21, 0xc6, 0x9b, 0x67, 0xda, 0x72, 0x61, 0xc4, 0x11, 0xe7, 0x4a, 0x40, 0x0d, 0xfa, 0x82, 0x1c,
    0x32, 0xa8, 0xb1, 0x39, 0x88, 0xa0, 0x58, 0x25, 0x42, 0xa4, 0x7e, 0x2e, 0xc8, 0x0f, 0x98, 0xb7,
    0x8b, 0xa2, 0x5e, 0x1f, 0xa2, 0x84, 0x32, 0x74, 0xb7, 0x2f, 0xd9, 0x8a, 0xd0, 0x86, 0x70, 0x87,
    0x7c, 0x91, 0x23, 0x7f, 0x12, 0x34, 0xb4, 0xc1, 0xc2, 0x82, 0x8f, 0xcb, 0x3d, 0x77, 0x4c, 0x97,
    0x9f, 0xca, 0x4f, 0xf1, 0x31, 0x99, 0x14, 0xda, 0x36, 0xce, 0x9e, 0x75, 0x4b, 0x08, 0x7b, 0xf6,
    0xcf, 0x4d, 0x1c, 0xfb, 0xef, 0xf4, 0x9d, 0xb0, 0x79, 0x5f, 0xbd, 0xfe, 0xf1, 0x32, 0xc8, 0xae,
    0xdc, 0x99, 0x07, 0xdd, 0xd8, 0x13, 0xb8, 0x4f, 0xc3, 0x1c, 0xdf, 0xed, 0x4c, 0x01, 0x90, 0xa6,
    0x91, 0xb9, 0xda, 0xc6, 0x8a, 0xf6, 0x1e, 0xf9, 0xc8, 0xe9, 0x71, 0xfa, 0x4a, 0x70, 0x33, 0xbc,
    0x60, 0xf0, 0x37, 0xf4, 0x45, 0xc8, 0x51, 0x71, 0xad, 0xdb, 0x70, 0x76, 0x3f, 0x09, 0xdf, 0xfd,
    0x98, 0x37, 0x22, 0x4b, 0x21, 0x01, 0x2f, 0x76, 0x20, 0xd9, 0x01, 0xc0, 0x93, 0x8e, 0x68, 0x68,
    0x4f, 0x7b, 0x85, 0x7e, 0x41, 0xa8, 0xf4, 0x8f, 0x94, 0x08, 0x40, 0xd7, 0xd6, 0x6f, 0xbc, 0x94,
    0x1a, 0x5d, 0x76, 0x93, 0xac, 0x5a, 0xe6, 0x71, 0x0e, 0x15, 0x7d, 0xa0, 0x2a, 0xdc, 0xc5, 0x28,
    0x58, 0xbb, 0x83, 0x74, 0x02, 0x7f, 0xca, 0x2e, 0xe5, 0xac, 0x3e, 0x61, 0xd4, 0xfa, 0xce, 0x7d,
    0x1c, 0x52, 0x65, 0x05, 0x57, 0xb3, 0xcc, 0xa5, 0xd8, 0x81, 0x46, 0x70, 0x65, 0x9b, 0x03, 0xcf,
    0xaa, 0x39, 0xbc, 0x0c, 0x6d, 0x39, 0x0e, 0x8c, 0x28, 0x50, 0x93, 0xa2, 0x29, 0xca, 0xad, 0x62,
    0x60, 0xfe, 0xc4, 0x0b, 0x7a, 0xa0, 0xe7, 0xe8, 0x1b, 0x60, 0x68, 0xd8, 0x7d, 0x9e, 0x72, 0x50,
    0x82, 0x49, 0x38, 0x5c, 0x47, 0xdf, 0x89, 0x0d, 0xd8, 0x69, 0x2e, 0x2c, 0xc1, 0x0e, 0x58, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0xf6, 0xb8, 0x63, 0xca, 0x97, 0x71, 0xf8, 0x4a, 0xcf, 0xfb,
    0x03, 0x62, 0x32, 0xc8, 0x00, 0x3d, 0xd0, 0xbb, 0xa4, 0xa4, 0x60, 0x80, 0x07, 0x7d, 0x01, 0x29,
    0x06, 0xde, 0xff, 0x6e, 0x10, 0xc8, 0xe4, 0xe5, 0x59, 0xff, 0xfe, 0xff, 0xfe,
};

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
    ChatLcdDisplay* display_;
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
        display_ = new ChatLcdDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
            /*round_panel=*/true, /*touch_controls=*/true, /*rounder_align=*/true);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    // esp_lvgl_port aborts on any touch read error, but the CST9217 sometimes answers one
    // frame without its ACK byte (ESP_ERR_INVALID_RESPONSE), e.g. ~2 s after the reboot that
    // follows an OTA. Treat a failed read as "not touched" instead of restarting.
    static inline esp_err_t (*cst9217_read_data_)(esp_lcd_touch_handle_t) = nullptr;

    static esp_err_t ReadTouchData(esp_lcd_touch_handle_t tp) {
        esp_err_t err = cst9217_read_data_(tp);
        if (err != ESP_OK) {
            static int failures = 0;
            if (++failures <= 5 || failures % 100 == 0) {
                // Read the report register (0xD000) once more just to log what the controller
                // answers; byte 6 should be the 0xAB ACK
                uint8_t reg_low = 0x00;
                uint8_t report[10] = {};
                esp_lcd_panel_io_tx_param(tp->io, 0xD0, &reg_low, 1);
                esp_lcd_panel_io_rx_param(tp->io, -1, report, sizeof(report));
                ESP_LOGW(TAG, "Touch read failed (%d): %s, INT=%d", failures, esp_err_to_name(err),
                         gpio_get_level(tp->config.int_gpio_num));
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, report, sizeof(report), ESP_LOG_WARN);
            }
            portENTER_CRITICAL(&tp->data.lock);
            tp->data.points = 0;
            portEXIT_CRITICAL(&tp->data.lock);
        }
        return ESP_OK;
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
        cst9217_read_data_ = tp->read_data;
        tp->read_data = ReadTouchData;
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
        display_->SetAudioLevelSource([this](bool input) {
            auto codec = static_cast<MeteredBoxAudioCodec*>(GetAudioCodec());
            return input ? codec->input_level() : codec->output_level();
        });
        display_->OnVolumeChange([this](int volume) {
            Application::GetInstance().Schedule([this, volume]() {
                GetAudioCodec()->SetOutputVolume(volume);
                // A click at the new level; not over an answer, where a 16 kHz
                // sound would interrupt the speech stream's decoder
                auto& app = Application::GetInstance();
                if (volume > 0 && app.GetDeviceState() != kDeviceStateSpeaking) {
                    app.PlaySound(std::string_view(
                        reinterpret_cast<const char*>(kVolumeTickOgg), sizeof(kVolumeTickOgg)));
                }
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
        static MeteredBoxAudioCodec audio_codec(
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
