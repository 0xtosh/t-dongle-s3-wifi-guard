#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <FastLED.h>
#include <lvgl.h>
#include <stdarg.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"

#ifndef BOOT_PIN
#define BOOT_PIN 0
#endif

#define LED_DI_PIN 40
#define LED_CI_PIN 39

#define LCD_HOST SPI2_HOST
#define PIN_NUM_MOSI 3
#define PIN_NUM_CLK 5
#define PIN_NUM_CS 4
#define PIN_NUM_DC 2
#define PIN_NUM_RST 1
#define PIN_NUM_BCKL 38

#define SD_MMC_D0_PIN 14
#define SD_MMC_D1_PIN 17
#define SD_MMC_D2_PIN 21
#define SD_MMC_D3_PIN 18
#define SD_MMC_CLK_PIN 12
#define SD_MMC_CMD_PIN 16

#define LCD_PIXEL_WIDTH 160
#define LCD_PIXEL_HEIGHT 80
#define LEDC_BACKLIGHT_FREQ 1000
#define LEDC_BACKLIGHT_BIT_WIDTH 8
#define LEDC_BACKLIGHT_CHANNEL 3

#define CHANNEL_MIN 1
#define CHANNEL_MAX 13
#define CHANNEL_HOP_MS 400
#define ALERT_TIMEOUT_MS 8000
#define ALERT_EXTRA_HOLD_MS 1000
#define BURST_WINDOW_MS 1500
#define ALERT_THRESHOLD 3
#define LED_BLINK_MS 180
#define UI_UPDATE_MS 125
#define WIFI_INIT_DELAY_MS 2500
#define MARQUEE_STEP_MS 35
#define MARQUEE_STEP_PX 1
#define MARQUEE_GAP_PX 24
#define LOG_PACKET_WINDOW_MS 3000
#define LOG_PACKETS_PER_WINDOW 3

enum AlertKind : uint8_t {
    ALERT_NONE = 0,
    ALERT_AP_TARGETED,
    ALERT_CLIENT_TARGETED,
    ALERT_UNKNOWN
};

struct DetectionEvent {
    uint8_t source[6];
    uint8_t destination[6];
    uint8_t bssid[6];
    uint16_t reason;
    uint8_t channel;
    uint8_t burst_count;
    AlertKind kind;
    bool destination_is_broadcast;
    uint32_t last_seen_ms;
    bool valid;
};

struct MarqueeLine {
    lv_obj_t *viewport;
    lv_obj_t *label;
    char *cache;
    size_t cache_size;
    lv_coord_t viewport_width;
    lv_coord_t text_width;
    lv_coord_t origin_y;
    int32_t offset_x;
    uint32_t last_step_ms;
};

static esp_lcd_panel_handle_t panel_handle = nullptr;
static esp_lcd_panel_io_handle_t io_handle = nullptr;
static spi_device_handle_t spi_handle = nullptr;
static esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = PIN_NUM_RST,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    .color_space = ESP_LCD_COLOR_SPACE_BGR,
#else
    .color_space = LCD_RGB_ELEMENT_ORDER_BGR,
    .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
#endif
    .bits_per_pixel = 16,
};
static spi_bus_config_t spi_config = ST7735_PANEL_BUS_SPI_CONFIG(
        PIN_NUM_CLK, PIN_NUM_MOSI, LCD_PIXEL_WIDTH * LCD_PIXEL_HEIGHT * sizeof(uint16_t));
static esp_lcd_panel_io_spi_config_t io_config = ST7735_PANEL_IO_SPI_CONFIG(
        PIN_NUM_CS, PIN_NUM_DC, nullptr, nullptr);

static CRGB leds[1];
static portMUX_TYPE detection_mux = portMUX_INITIALIZER_UNLOCKED;
static DetectionEvent shared_detection = {};
static DetectionEvent latest_detection = {};

static bool monitoring_enabled = true;
static bool monitoring_ready = false;
static bool monitoring_failed = false;
static uint8_t current_channel = CHANNEL_MIN;
static uint32_t last_channel_hop_ms = 0;
static uint32_t boot_debounce_ms = 0;
static uint32_t last_led_update_ms = 0;
static uint32_t last_ui_update_ms = 0;
static uint32_t wifi_init_due_ms = WIFI_INIT_DELAY_MS;
static bool wifi_init_done = false;
static bool sd_logging_ready = false;
static char log_file_path[64] = "";
static DetectionEvent log_window_detection = {};
static uint32_t log_window_start_ms = 0;
static uint8_t log_window_entries = 0;
static uint32_t last_logged_detection_seen_ms = 0;

static lv_obj_t *top_bar = nullptr;
static lv_obj_t *status_label = nullptr;
static lv_obj_t *channel_label = nullptr;
static lv_obj_t *headline_banner = nullptr;
static lv_obj_t *headline_label = nullptr;
static lv_obj_t *content_area = nullptr;
static lv_obj_t *details_label = nullptr;
static lv_obj_t *footer_label = nullptr;

static char status_text_cache[24] = "";
static char channel_text_cache[16] = "";
static char headline_text_cache[32] = "";
static char summary_text_cache[196] = "";
static char details_text_cache[196] = "";
static char footer_text_cache[96] = "";

static MarqueeLine summary_line = {};
static MarqueeLine details_line = {};
static MarqueeLine footer_line = {};

static inline bool mac_equals(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, 6) == 0;
}

static inline bool detection_identity_equals(const DetectionEvent &left, const DetectionEvent &right)
{
    if (!left.valid || !right.valid) {
        return false;
    }

    return left.kind == right.kind &&
           left.reason == right.reason &&
           left.channel == right.channel &&
           left.destination_is_broadcast == right.destination_is_broadcast &&
           mac_equals(left.source, right.source) &&
           mac_equals(left.destination, right.destination) &&
           mac_equals(left.bssid, right.bssid);
}

static inline bool mac_is_broadcast(const uint8_t *mac)
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return mac_equals(mac, broadcast);
}

static void format_mac(char *buffer, size_t buffer_size, const uint8_t *mac)
{
    snprintf(buffer, buffer_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool is_alert_active(const DetectionEvent &detection, uint32_t now_ms)
{
    return detection.valid &&
           detection.burst_count >= ALERT_THRESHOLD &&
           (now_ms - detection.last_seen_ms) <= (ALERT_TIMEOUT_MS + ALERT_EXTRA_HOLD_MS);
}

static const uint8_t *targeted_mac_for_event(const DetectionEvent &detection)
{
    if (detection.kind == ALERT_AP_TARGETED) {
        return detection.destination;
    }
    if (detection.kind == ALERT_CLIENT_TARGETED) {
        return detection.destination_is_broadcast ? detection.bssid : detection.destination;
    }
    return detection.destination_is_broadcast ? detection.bssid : detection.destination;
}

static void append_log_line(const char *fmt, ...)
{
    if (!sd_logging_ready || log_file_path[0] == '\0') {
        return;
    }

    File file = SD_MMC.open(log_file_path, FILE_APPEND);
    if (!file) {
        Serial.println("Failed to open log file for append");
        sd_logging_ready = false;
        return;
    }

    char message[224];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    file.print(message);
    file.close();
}

static void init_sd_logging()
{
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN, SD_MMC_D1_PIN, SD_MMC_D2_PIN, SD_MMC_D3_PIN);
    if (!SD_MMC.begin()) {
        Serial.println("SD logging disabled: card mount failed");
        return;
    }

    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("SD logging disabled: no card attached");
        return;
    }

    SD_MMC.mkdir("/logs");

    const uint32_t boot_ms = millis();
    const uint32_t session_id = esp_random();
    snprintf(log_file_path, sizeof(log_file_path), "/logs/boot_%010lu_%08lX.log",
             static_cast<unsigned long>(boot_ms),
             static_cast<unsigned long>(session_id));

    File file = SD_MMC.open(log_file_path, FILE_WRITE);
    if (!file) {
        Serial.println("SD logging disabled: could not create session log");
        log_file_path[0] = '\0';
        return;
    }

    file.printf("[%010lu ms] START session=%08lX\n",
                static_cast<unsigned long>(boot_ms),
                static_cast<unsigned long>(session_id));
    file.close();

    sd_logging_ready = true;
    Serial.printf("SD logging active: %s\n", log_file_path);
}

static void log_monitor_state_change(bool enabled, uint32_t now_ms)
{
    append_log_line("[%010lu ms] %s\n",
                    static_cast<unsigned long>(now_ms),
                    enabled ? "RESUME" : "STANDBY");
}

static void log_detection_if_needed(const DetectionEvent &detection, uint32_t now_ms)
{
    if (!detection.valid) {
        return;
    }

    if ((now_ms - detection.last_seen_ms) > UI_UPDATE_MS * 2U) {
        return;
    }

    const bool same_window = detection_identity_equals(detection, log_window_detection);
    if (!same_window || (now_ms - log_window_start_ms) > LOG_PACKET_WINDOW_MS) {
        log_window_detection = detection;
        log_window_start_ms = now_ms;
        log_window_entries = 0;
        last_logged_detection_seen_ms = 0;
    }

    if (log_window_entries >= LOG_PACKETS_PER_WINDOW || detection.last_seen_ms == last_logged_detection_seen_ms) {
        return;
    }

    char source_mac[18];
    char destination_mac[18];
    char target_mac[18];
    format_mac(source_mac, sizeof(source_mac), detection.source);
    format_mac(destination_mac, sizeof(destination_mac), detection.destination);
    format_mac(target_mac, sizeof(target_mac), targeted_mac_for_event(detection));

    append_log_line("[%010lu ms] DEAUTH target=%s src=%s dst=%s ch=%u kind=%u burst=%u packet_ms=%010lu\n",
                    static_cast<unsigned long>(now_ms),
                    target_mac,
                    source_mac,
                    destination_mac,
                    static_cast<unsigned>(detection.channel),
                    static_cast<unsigned>(detection.kind),
                    static_cast<unsigned>(detection.burst_count),
                    static_cast<unsigned long>(detection.last_seen_ms));

    last_logged_detection_seen_ms = detection.last_seen_ms;
    log_window_entries++;
}

static uint32_t lv_tick_get_callback(void)
{
    return millis();
}

static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *color_p)
{
    const size_t area_size = lv_area_get_size(area);
    lv_draw_sw_rgb565_swap(color_p, area_size);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              reinterpret_cast<uint16_t *>(color_p));
    lv_display_flush_ready(display);
}

static AlertKind classify_deauth(const uint8_t *destination, const uint8_t *source, const uint8_t *bssid)
{
    if (mac_equals(source, bssid)) {
        return ALERT_CLIENT_TARGETED;
    }
    if (mac_equals(destination, bssid)) {
        return ALERT_AP_TARGETED;
    }
    if (mac_is_broadcast(destination)) {
        return ALERT_CLIENT_TARGETED;
    }
    return ALERT_UNKNOWN;
}

static bool same_signature(const DetectionEvent &left, const DetectionEvent &right, uint32_t now_ms)
{
    if (!left.valid || !right.valid) {
        return false;
    }
    if ((now_ms - right.last_seen_ms) > BURST_WINDOW_MS) {
        return false;
    }
    return left.kind == right.kind &&
           left.reason == right.reason &&
           left.channel == right.channel &&
           left.destination_is_broadcast == right.destination_is_broadcast &&
           mac_equals(left.source, right.source) &&
           mac_equals(left.destination, right.destination) &&
           mac_equals(left.bssid, right.bssid);
}

static void wifi_sniffer_callback(void *buffer, wifi_promiscuous_pkt_type_t packet_type)
{
    if (packet_type != WIFI_PKT_MGMT) {
        return;
    }

    const auto *packet = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    if (packet->rx_ctrl.sig_len < 26) {
        return;
    }

    const uint8_t *payload = packet->payload;
    const uint16_t frame_control = static_cast<uint16_t>(payload[0]) |
                                   (static_cast<uint16_t>(payload[1]) << 8);
    const uint8_t frame_type = (frame_control >> 2) & 0x3;
    const uint8_t frame_subtype = (frame_control >> 4) & 0xf;

    if (frame_type != 0 || frame_subtype != 0x0c) {
        return;
    }

    DetectionEvent candidate = {};
    memcpy(candidate.destination, payload + 4, 6);
    memcpy(candidate.source, payload + 10, 6);
    memcpy(candidate.bssid, payload + 16, 6);
    candidate.reason = static_cast<uint16_t>(payload[24]) |
                       (static_cast<uint16_t>(payload[25]) << 8);
    candidate.channel = packet->rx_ctrl.channel;
    candidate.kind = classify_deauth(candidate.destination, candidate.source, candidate.bssid);
    candidate.destination_is_broadcast = mac_is_broadcast(candidate.destination);
    candidate.last_seen_ms = millis();
    candidate.valid = true;

    portENTER_CRITICAL(&detection_mux);
    DetectionEvent updated = shared_detection;

    if (same_signature(candidate, updated, candidate.last_seen_ms)) {
        updated.burst_count = static_cast<uint8_t>(min<uint16_t>(updated.burst_count + 1, 255));
    } else {
        updated = candidate;
        updated.burst_count = 1;
    }

    updated.last_seen_ms = candidate.last_seen_ms;
    shared_detection = updated;
    portEXIT_CRITICAL(&detection_mux);
}

static void set_led(const CRGB &color)
{
    leds[0] = color;
    FastLED.show();
}

static void update_led(uint32_t now_ms, bool alert_active)
{
    if ((now_ms - last_led_update_ms) < 25) {
        return;
    }

    last_led_update_ms = now_ms;

    if (!monitoring_ready || monitoring_failed) {
        set_led(CRGB(64, 0, 64));
        return;
    }

    if (!monitoring_enabled) {
        set_led(CRGB(255, 110, 0));
        return;
    }

    if (alert_active) {
        if (((now_ms / LED_BLINK_MS) % 2U) == 0U) {
            set_led(CRGB::Red);
        } else {
            set_led(CRGB::Black);
        }
        return;
    }

    set_led(CRGB(0, 30, 8));
}

static void create_marquee_line(MarqueeLine *line, lv_obj_t *parent, lv_coord_t width, lv_coord_t y,
                                lv_coord_t height, const lv_font_t *font, lv_color_t color,
                                char *cache, size_t cache_size)
{
    line->viewport = lv_obj_create(parent);
    lv_obj_set_size(line->viewport, width, height);
    lv_obj_set_pos(line->viewport, 4, y);
    lv_obj_set_style_radius(line->viewport, 0, 0);
    lv_obj_set_style_border_width(line->viewport, 0, 0);
    lv_obj_set_style_outline_width(line->viewport, 0, 0);
    lv_obj_set_style_shadow_width(line->viewport, 0, 0);
    lv_obj_set_style_pad_all(line->viewport, 0, 0);
    lv_obj_set_style_bg_color(line->viewport, lv_color_hex(0x050816), 0);
    lv_obj_set_style_bg_opa(line->viewport, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(line->viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(line->viewport, LV_OBJ_FLAG_SCROLLABLE);

    line->label = lv_label_create(line->viewport);
    lv_obj_set_pos(line->label, 0, 0);
    lv_label_set_long_mode(line->label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(line->label, font, 0);
    lv_obj_set_style_text_color(line->label, color, 0);
    lv_obj_set_style_bg_color(line->label, lv_color_hex(0x050816), 0);
    lv_obj_set_style_bg_opa(line->label, LV_OPA_COVER, 0);

    line->cache = cache;
    line->cache_size = cache_size;
    line->viewport_width = width;
    line->text_width = 0;
    line->origin_y = 0;
    line->offset_x = 0;
    line->last_step_ms = 0;
}

static void set_label_text_if_changed(lv_obj_t *label, char *cache, size_t cache_size, const char *text)
{
    if (strncmp(cache, text, cache_size) == 0) {
        return;
    }

    strncpy(cache, text, cache_size - 1);
    cache[cache_size - 1] = '\0';
    lv_label_set_text(label, cache);
}

static void set_marquee_text_if_changed(MarqueeLine *line, const char *text)
{
    if (strncmp(line->cache, text, line->cache_size) == 0) {
        return;
    }

    strncpy(line->cache, text, line->cache_size - 1);
    line->cache[line->cache_size - 1] = '\0';
    lv_label_set_text(line->label, line->cache);
    lv_obj_update_layout(line->label);
    line->text_width = lv_obj_get_width(line->label);
    line->offset_x = 0;
    line->last_step_ms = 0;

    if (line->text_width > line->viewport_width) {
        lv_obj_set_x(line->label, 0);
    } else {
        lv_obj_set_x(line->label, 0);
    }
}

static void update_marquee_line(MarqueeLine *line, uint32_t now_ms)
{
    if (!line->label || line->text_width <= line->viewport_width) {
        return;
    }
    if ((now_ms - line->last_step_ms) < MARQUEE_STEP_MS) {
        return;
    }

    line->last_step_ms = now_ms;
    lv_obj_invalidate(line->viewport);
    line->offset_x -= MARQUEE_STEP_PX;
    if (line->offset_x < -(line->text_width + MARQUEE_GAP_PX)) {
        line->offset_x = line->viewport_width;
    }
    lv_obj_set_x(line->label, line->offset_x);
    lv_obj_invalidate(line->viewport);
}

static void update_marquees(uint32_t now_ms)
{
    update_marquee_line(&summary_line, now_ms);
    // Removed details_line and footer_line updates for fixed labels
}

static void init_display()
{
    pinMode(PIN_NUM_BCKL, OUTPUT);
    digitalWrite(PIN_NUM_BCKL, HIGH);

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi_config, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    esp_lcd_panel_set_gap(panel_handle, 1, 26);
    esp_lcd_panel_swap_xy(panel_handle, true);
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    ledcAttach(PIN_NUM_BCKL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
#else
    ledcSetup(LEDC_BACKLIGHT_CHANNEL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
    ledcAttachPin(PIN_NUM_BCKL, LEDC_BACKLIGHT_CHANNEL);
#endif
    ledcWrite(LEDC_BACKLIGHT_CHANNEL, 0);

    lv_init();

    static lv_color16_t *draw_buffer = nullptr;
    static lv_color16_t *draw_buffer_secondary = nullptr;
    const size_t draw_buffer_size = LCD_PIXEL_WIDTH * LCD_PIXEL_HEIGHT * sizeof(lv_color16_t);
    draw_buffer = reinterpret_cast<lv_color16_t *>(malloc(draw_buffer_size));
    draw_buffer_secondary = reinterpret_cast<lv_color16_t *>(malloc(draw_buffer_size));
    assert(draw_buffer);
    assert(draw_buffer_secondary);

    lv_display_t *display = lv_display_create(LCD_PIXEL_WIDTH, LCD_PIXEL_HEIGHT);
    lv_display_set_buffers(display, draw_buffer, draw_buffer_secondary, draw_buffer_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_tick_set_cb(lv_tick_get_callback);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x050816), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    top_bar = lv_obj_create(screen);
    lv_obj_set_size(top_bar, 160, 16);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_shadow_width(top_bar, 0, 0);
    lv_obj_set_style_outline_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x11312e), 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    status_label = lv_label_create(top_bar);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_TRANSP, 0);

    channel_label = lv_label_create(top_bar);
    lv_obj_align(channel_label, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_text_font(channel_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(channel_label, lv_color_hex(0xcde7ff), 0);
    lv_obj_set_style_bg_opa(channel_label, LV_OPA_TRANSP, 0);

    headline_banner = lv_obj_create(screen);
    lv_obj_set_size(headline_banner, 160, 18);
    lv_obj_set_pos(headline_banner, 0, 18);
    lv_obj_set_style_radius(headline_banner, 0, 0);
    lv_obj_set_style_border_width(headline_banner, 0, 0);
    lv_obj_set_style_outline_width(headline_banner, 0, 0);
    lv_obj_set_style_shadow_width(headline_banner, 0, 0);
    lv_obj_set_style_pad_left(headline_banner, 6, 0);
    lv_obj_set_style_pad_right(headline_banner, 0, 0);
    lv_obj_set_style_pad_top(headline_banner, 0, 0);
    lv_obj_set_style_pad_bottom(headline_banner, 0, 0);
    lv_obj_set_style_bg_color(headline_banner, lv_color_hex(0x050816), 0);
    lv_obj_set_style_bg_opa(headline_banner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(headline_banner, LV_OBJ_FLAG_SCROLLABLE);

    headline_label = lv_label_create(headline_banner);
    lv_obj_align(headline_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(headline_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(headline_label, lv_color_hex(0x96f5c5), 0);
    lv_obj_set_style_bg_opa(headline_label, LV_OPA_TRANSP, 0);

    content_area = lv_obj_create(screen);
    lv_obj_set_size(content_area, 160, 38);
    lv_obj_set_pos(content_area, 0, 42);
    lv_obj_set_style_radius(content_area, 0, 0);
    lv_obj_set_style_border_width(content_area, 0, 0);
    lv_obj_set_style_outline_width(content_area, 0, 0);
    lv_obj_set_style_shadow_width(content_area, 0, 0);
    lv_obj_set_style_pad_all(content_area, 0, 0);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(content_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);

    create_marquee_line(&summary_line, content_area, 152, 0, 12, &lv_font_montserrat_10, lv_color_hex(0xf6f7fb), summary_text_cache, sizeof(summary_text_cache));

    details_label = lv_label_create(content_area);
    lv_obj_set_pos(details_label, 4, 13);
    lv_obj_set_width(details_label, 152);
    lv_label_set_long_mode(details_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(details_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(details_label, lv_color_hex(0xa7b4c8), 0);
    lv_obj_set_style_bg_opa(details_label, LV_OPA_TRANSP, 0);
    lv_label_set_text(details_label, "Channel hopping 2,4Ghz");

    footer_label = lv_label_create(content_area);
    lv_obj_set_pos(footer_label, 4, 26);
    lv_obj_set_width(footer_label, 152);
    lv_label_set_long_mode(footer_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(footer_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(footer_label, lv_color_hex(0x7ee2ff), 0);
    lv_obj_set_style_bg_opa(footer_label, LV_OPA_TRANSP, 0);
    lv_label_set_text(footer_label, "BUTTON: Toggle Pause");

    lv_timer_handler();
    delay(20);

}

static bool configure_monitoring(bool enable)
{
    esp_err_t result = esp_wifi_set_promiscuous(enable);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous(%d) failed: %s\n", enable, esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return false;
    }

    monitoring_failed = false;
    monitoring_ready = true;
    monitoring_enabled = enable;
    return true;
}

static void init_wifi_monitor()
{
    if (wifi_init_done) {
        return;
    }

    wifi_init_done = true;
    WiFi.persistent(false);
    WiFi.mode(WIFI_MODE_STA);
    WiFi.disconnect(false, true);
    delay(150);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };

    esp_err_t result = esp_wifi_set_promiscuous(false);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous(false) failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return;
    }

    result = esp_wifi_set_promiscuous_filter(&filter);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous_filter failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return;
    }

    result = esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_callback);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_promiscuous_rx_cb failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return;
    }

    result = esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("esp_wifi_set_channel failed: %s\n", esp_err_to_name(result));
        monitoring_failed = true;
        monitoring_ready = false;
        return;
    }

    if (!configure_monitoring(true)) {
        return;
    }

    Serial.println("WiFi monitor initialized");
    append_log_line("[%010lu ms] MONITOR_READY\n", static_cast<unsigned long>(millis()));
}

static void poll_button(uint32_t now_ms)
{
    static bool last_pressed = false;
    const bool pressed = digitalRead(BOOT_PIN) == LOW;

    if (pressed && !last_pressed && (now_ms - boot_debounce_ms) > 250) {
        boot_debounce_ms = now_ms;
        const bool new_enabled_state = !monitoring_enabled;
        if (configure_monitoring(new_enabled_state)) {
            log_monitor_state_change(new_enabled_state, now_ms);
        }
    }

    last_pressed = pressed;
}

static void hop_channel(uint32_t now_ms)
{
    if (!monitoring_enabled || !monitoring_ready) {
        return;
    }
    if ((now_ms - last_channel_hop_ms) < CHANNEL_HOP_MS) {
        return;
    }

    current_channel++;
    if (current_channel > CHANNEL_MAX) {
        current_channel = CHANNEL_MIN;
    }

    if (esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        last_channel_hop_ms = now_ms;
    }
}

static void copy_latest_detection()
{
    portENTER_CRITICAL(&detection_mux);
    latest_detection = shared_detection;
    portEXIT_CRITICAL(&detection_mux);
}

static void update_ui(uint32_t now_ms)
{
    if ((now_ms - last_ui_update_ms) < UI_UPDATE_MS) {
        copy_latest_detection();
        const bool alert_active_fast = is_alert_active(latest_detection, now_ms);
        update_led(now_ms, alert_active_fast);
        return;
    }

    last_ui_update_ms = now_ms;
    copy_latest_detection();

    const bool alert_active = is_alert_active(latest_detection, now_ms);

    char status_text[24];
    char channel_text[16];
    char summary_text[196];
    char details_text[196];
    char footer_text[96];
    char source_mac[18] = "";
    char destination_mac[18] = "";
    char bssid_mac[18] = "";
    char target_mac[18] = "";

    format_mac(source_mac, sizeof(source_mac), latest_detection.source);
    format_mac(destination_mac, sizeof(destination_mac), latest_detection.destination);
    format_mac(bssid_mac, sizeof(bssid_mac), latest_detection.bssid);
    format_mac(target_mac, sizeof(target_mac), targeted_mac_for_event(latest_detection));

    log_detection_if_needed(latest_detection, now_ms);

    if (monitoring_failed) {
        strcpy(status_text, "MONITOR FAIL");
    } else if (!monitoring_enabled) {
        strcpy(status_text, "PAUSED");
    } else {
        strcpy(status_text, alert_active ? "DEAUTH ALERT" : "LISTENING");
    }

    snprintf(channel_text, sizeof(channel_text), "CH %u", current_channel);

    if (alert_active) {
        if (latest_detection.kind == ALERT_AP_TARGETED) {
            set_label_text_if_changed(headline_label, headline_text_cache, sizeof(headline_text_cache), "AP TARGETED");
            snprintf(summary_text, sizeof(summary_text), "%s", target_mac);
        } else if (latest_detection.kind == ALERT_CLIENT_TARGETED) {
            set_label_text_if_changed(headline_label, headline_text_cache, sizeof(headline_text_cache),
                                      latest_detection.destination_is_broadcast ? "CLIENTS TARGETED" : "CLIENT TARGETED");
            snprintf(summary_text, sizeof(summary_text), "%s", target_mac);
        } else {
            set_label_text_if_changed(headline_label, headline_text_cache, sizeof(headline_text_cache), "DEAUTH SEEN");
            snprintf(summary_text, sizeof(summary_text), "%s", target_mac);
        }

        snprintf(details_text, sizeof(details_text), "Channel hopping 2,4Ghz");
        snprintf(footer_text, sizeof(footer_text), "BUTTON: Toggle Pause");

        lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x5a1217), 0);
        lv_obj_set_style_text_color(headline_label, lv_color_hex(0xff8d8d), 0);
    } else {
        set_label_text_if_changed(headline_label, headline_text_cache, sizeof(headline_text_cache),
                                  monitoring_enabled ? "CLEAR" : "STANDBY");
        snprintf(summary_text, sizeof(summary_text), "%s", monitoring_enabled ? "Waiting for repeated deauth bursts" : "Monitoring paused");
        strcpy(details_text, "Channel hopping 2,4Ghz");
        strcpy(footer_text, "BUTTON: Toggle Pause");

        lv_obj_set_style_bg_color(top_bar, monitoring_enabled ? lv_color_hex(0x11312e) : lv_color_hex(0x4f3311), 0);
        lv_obj_set_style_text_color(headline_label,
                                    monitoring_enabled ? lv_color_hex(0x96f5c5) : lv_color_hex(0xffd38a),
                                    0);
    }

    set_label_text_if_changed(status_label, status_text_cache, sizeof(status_text_cache), status_text);
    set_label_text_if_changed(channel_label, channel_text_cache, sizeof(channel_text_cache), channel_text);
    set_marquee_text_if_changed(&summary_line, summary_text);
    set_label_text_if_changed(details_label, details_text_cache, sizeof(details_text_cache), details_text);
    set_label_text_if_changed(footer_label, footer_text_cache, sizeof(footer_text_cache), footer_text);

    update_led(now_ms, alert_active);
}

void setup()
{
    Serial.begin(115200);
    delay(1200);
    Serial.println("T-Dongle-S3 deauth detector starting");

    init_sd_logging();

    pinMode(BOOT_PIN, INPUT);

    FastLED.addLeds<APA102, LED_DI_PIN, LED_CI_PIN, BGR>(leds, 1);
    FastLED.setBrightness(72);
    set_led(CRGB::Black);

    init_display();
    update_ui(millis());
    lv_timer_handler();
}

void loop()
{
    const uint32_t now_ms = millis();

    if (!wifi_init_done && now_ms >= wifi_init_due_ms) {
        init_wifi_monitor();
    }

    poll_button(now_ms);
    hop_channel(now_ms);
    update_ui(now_ms);
    update_marquees(now_ms);
    lv_timer_handler();
    delay(5);
}