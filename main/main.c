//
// T-Display-S3 1.9" GIF Player — ESP-IDF 5.5
// I80 8-bit Parallel ST7789 (from xiaozhi-esp32 lilygo-t-display-s3-1.9inch)
//
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_st7789.h>
#include <esp_lcd_io_i80.h>

static const char* TAG = "GIF";

// ===== T-Display-S3 1.9" ST7789 硬件引脚 (I80 8-bit 并行) =====
// 100% verified from xiaozhi-esp32 official board support
#define PIN_LCD_CS     6
#define PIN_LCD_DC     7
#define PIN_LCD_WR     8
#define PIN_LCD_RD     9
#define PIN_LCD_RST    5
#define PIN_LCD_BL    38
#define PIN_LCD_PWR   15   // 电源使能

#define PIN_BOOT       0

// Display: 原生 170x320，SWAP_XY 旋转后帧缓冲 320x170
#define DISP_W_NATIVE  170
#define DISP_H_NATIVE  320
#define FRAME_SIZE (DISP_W_NATIVE * DISP_H_NATIVE * 2)   // 108800 bytes

#define SPIFFS_OFFSET  0x500000

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

typedef struct {
    uint16_t w;
    uint16_t h;
    uint16_t frames;
    uint16_t delay_ms;
} gif_meta_t;

// GPIO 批量配置辅助
static void gpio_config_output(uint32_t pin_mask)
{
    gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
}

// ===== 硬件初始化 =====
static void lcd_hardware_init(void)
{
    // PWR enable (GPIO15)
    gpio_config_output(1ULL << PIN_LCD_PWR);
    gpio_set_level((gpio_num_t)PIN_LCD_PWR, 1);

    // RD pin = HIGH (required by official code)
    gpio_config_output(1ULL << PIN_LCD_RD);
    gpio_set_level((gpio_num_t)PIN_LCD_RD, 1);

    // Backlight off initially
    gpio_config_output(1ULL << PIN_LCD_BL);
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 0);

    ESP_LOGI(TAG, "GPIO hardware init done");
}

// ===== I80 总线初始化 =====
static void lcd_i80_bus_init(void)
{
    // Data pins D0-D7
    gpio_num_t data_gpios[8] = {
        (gpio_num_t)39, (gpio_num_t)40, (gpio_num_t)41, (gpio_num_t)42,
        (gpio_num_t)45, (gpio_num_t)46, (gpio_num_t)47, (gpio_num_t)48
    };

    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = (gpio_num_t)PIN_LCD_DC,
        .wr_gpio_num = (gpio_num_t)PIN_LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {39, 40, 41, 42, 45, 46, 47, 48},
        .bus_width = 8,
        .max_transfer_bytes = FRAME_SIZE,
        .dma_burst_size = 64,
    };

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    // Panel IO for I80
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = (gpio_num_t)PIN_LCD_CS,
        .pclk_hz = 10 * 1000 * 1000,   // 10MHz
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle));
    ESP_LOGI(TAG, "I80 bus init done");
}

// ===== ST7789 完整初始化序列 (来自 LilyGo 官方) =====
static void st7789_init_sequence(void)
{
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);   // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_panel_io_tx_param(io_handle, 0x3A, (uint8_t[]){0x05}, 1);  // COLMOD=RGB565

    esp_lcd_panel_io_tx_param(io_handle, 0xB2, (uint8_t[]){0x0B, 0x0B, 0x00, 0x33, 0x33}, 5);  // PORCTRL
    esp_lcd_panel_io_tx_param(io_handle, 0xB7, (uint8_t[]){0x75}, 1);  // GCTRL
    esp_lcd_panel_io_tx_param(io_handle, 0xBB, (uint8_t[]){0x28}, 1);  // VCOMS
    esp_lcd_panel_io_tx_param(io_handle, 0xC0, (uint8_t[]){0x2C}, 1);  // LCMCTRL
    esp_lcd_panel_io_tx_param(io_handle, 0xC2, (uint8_t[]){0x01}, 1);  // VDVVRHEN
    esp_lcd_panel_io_tx_param(io_handle, 0xC3, (uint8_t[]){0x1F}, 1);  // VRHS
    esp_lcd_panel_io_tx_param(io_handle, 0xC6, (uint8_t[]){0x13}, 1);  // FRCTR2
    esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA7}, 1);  // PWCTRL1
    esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);  // PWCTRL1
    esp_lcd_panel_io_tx_param(io_handle, 0xD6, (uint8_t[]){0xA1}, 1);  // D6
    esp_lcd_panel_io_tx_param(io_handle, 0xE0,  // Positive Gamma
        (uint8_t[]){0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14);
    esp_lcd_panel_io_tx_param(io_handle, 0xE1,  // Negative Gamma
        (uint8_t[]){0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14);
}

// ===== 面板初始化 =====
static void lcd_panel_init(void)
{
    lcd_i80_bus_init();

    // Panel device config
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = (gpio_num_t)PIN_LCD_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
        .vendor_config = NULL,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &dev_cfg, &panel_handle));

    // Reset panel
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(150));

    // Init panel
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // 关键配置
    esp_lcd_panel_invert_color(panel_handle, true);   // ST7789 必须颜色反转
    esp_lcd_panel_swap_xy(panel_handle, true);       // 竖屏→横屏旋转
    esp_lcd_panel_mirror(panel_handle, false, true); // USB在左边: Y镜像

    // 偏移量 (X=35 让内容居中显示)
    esp_lcd_panel_set_gap(panel_handle, 35, 0);

    // 执行 official ST7789 init sequence
    st7789_init_sequence();

    // Display ON
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // Turn on backlight
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "ST7789 panel init OK (320x170 landscape)");
}

// ===== 绘制帧 =====
// SWAP_XY=true: 帧缓冲 320x170，源数据是原生 170x320
// draw_bitmap 的参数应该是原始坐标(170x320)，panel内部处理旋转
static void lcd_draw_frame(const uint16_t* rgb565)
{
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, DISP_W_NATIVE, DISP_H_NATIVE, rgb565);
}

static void lcd_clear(void)
{
    static uint16_t black[DISP_W_NATIVE] = {0};
    for (int y = 0; y < DISP_H_NATIVE; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISP_W_NATIVE, y + 1, black);
    }
}

static void power_off(void)
{
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_panel_disp_on_off(panel_handle, false);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "Deep sleep...");
    esp_deep_sleep_start();
}

static bool read_file(const char* path, size_t offset, void* buf, size_t len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, (long)offset, SEEK_SET);
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    return r == len;
}

void app_main(void)
{
    printf("\n====================================\n");
    printf("  T-Display-S3 1.9\" GIF Player\n");
    printf("  ESP-IDF 5.5 + ST7789 I80 Parallel\n");
    printf("  Pins: DC=7 WR=8 CS=6 RST=5\n");
    printf("        BL=38 PWR=15 RD=9\n");
    printf("  Data: GPIO 39-42, 45-48 (D0-D7)\n");
    printf("====================================\n");

    gpio_set_direction((gpio_num_t)PIN_BOOT, GPIO_MODE_INPUT);

    lcd_hardware_init();
    lcd_panel_init();

    // 分配帧缓冲 (PSRAM)
    void* frame_buf = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!frame_buf) {
        ESP_LOGW(TAG, "PSRAM unavailable, using DRAM");
        frame_buf = malloc(FRAME_SIZE);
    }
    if (!frame_buf) {
        ESP_LOGE(TAG, "Cannot alloc frame buffer!");
        return;
    }
    ESP_LOGI(TAG, "Frame buffer: %d bytes (%s)",
             FRAME_SIZE,
             heap_caps_check_integrity(FRAME_SIZE, MALLOC_CAP_SPIRAM) ? "PSRAM" : "DRAM");

    // 读取 GIF 元数据
    gif_meta_t meta;
    if (!read_file("/spiffs/frames.meta", 0, &meta, sizeof(meta))) {
        ESP_LOGE(TAG, "Cannot read frames.meta - run upload_spiffs.bat first");
        free(frame_buf);
        return;
    }
    ESP_LOGI(TAG, "GIF: %dx%d  frames=%u  delay=%u ms",
             meta.w, meta.h, meta.frames, meta.delay_ms);

    size_t frame_off = sizeof(gif_meta_t);

    lcd_clear();

    // 启动时检查 BOOT 键
    if (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
            ESP_LOGI(TAG, "BOOT held at startup - entering deep sleep");
            free(frame_buf);
            power_off();
            return;
        }
    }

    ESP_LOGI(TAG, "Playing %u frames...", meta.frames);

    int frame_idx = 0;
    int64_t last_frame = 0;

    while (1) {
        // 检查 BOOT 长按 (3秒深度睡眠)
        if (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
            uint32_t t0 = esp_timer_get_time() / 1000;
            while (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if ((esp_timer_get_time() / 1000) - t0 >= 3000) {
                    ESP_LOGI(TAG, "BOOT held 3s - deep sleep");
                    free(frame_buf);
                    power_off();
                    return;
                }
            }
        }

        // 帧定时
        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_frame >= meta.delay_ms) {
            frame_idx = (frame_idx + 1) % meta.frames;
            size_t off = frame_off + (size_t)frame_idx * FRAME_SIZE;
            if (!read_file("/spiffs/frames.bin", off, frame_buf, FRAME_SIZE)) {
                ESP_LOGE(TAG, "Frame read error idx=%d", frame_idx);
                break;
            }
            lcd_draw_frame((uint16_t*)frame_buf);
            last_frame = now;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(frame_buf);
    ESP_LOGI(TAG, "Done.");
}
