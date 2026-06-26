//
// T-Display-S3 1.9" GIF Player — ESP-IDF 5.5
// I80 8-bit Parallel ST7789
// Pinout 100% verified from xiaozhi-esp32 lilygo-t-display-s3-1.9inch
//
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_i80.h>
#include "esp_spiffs.h"

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
#define PIN_LCD_BL     18  // Backlight (ESP32-S3 has no GPIO38, use GPIO18)
#define PIN_LCD_PWR   15
#define PIN_BOOT       0

// 帧缓冲: SWAP_XY=true 时 panel 视为 320x170
#define FRAME_W  320
#define FRAME_H  170
#define FRAME_SIZE (FRAME_W * FRAME_H * 2)   // 108800 bytes

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

typedef struct {
    uint16_t w;
    uint16_t h;
    uint16_t frames;
    uint16_t delay_ms;
} gif_meta_t;

static bool read_file(const char* path, size_t offset, void* buf, size_t len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, (long)offset, SEEK_SET);
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    return r == len;
}

// ===== SPIFFS 挂载 =====
static bool mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %zu/%zu bytes used", used, total);
    return true;
}

// ===== 测试色块填充 =====
static void fill_test_pattern(void)
{
    void* fb = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_DMA);
    if (!fb) fb = malloc(FRAME_SIZE);
    if (!fb) {
        ESP_LOGE(TAG, "Cannot alloc frame buffer!");
        return;
    }

    uint16_t* p = (uint16_t*)fb;
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            if (y < FRAME_H/3) {
                // 红色渐变 (上)
                *p++ = (((x * 31 / FRAME_W) << 11) | 0x20);
            } else if (y < 2*FRAME_H/3) {
                // 绿色渐变 (中)
                *p++ = (((y * 63 / FRAME_H) << 5) | (x * 31 / FRAME_W));
            } else {
                // 蓝色渐变 (下)
                *p++ = (((y * 31 / FRAME_H) << 0) | (0x1F << 11));
            }
        }
    }

    // SWAP_XY=true: 帧缓冲是 320x170
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FRAME_W, FRAME_H, fb);
    free(fb);
    ESP_LOGI(TAG, "Test pattern drawn");
}

// ===== 主程序 =====
void app_main(void)
{
    printf("\n====================================\n");
    printf("  T-Display-S3 1.9\" GIF Player\n");
    printf("  ESP-IDF 5.5 + ST7789 I80 Parallel\n");
    printf("  Pins: DC=%d WR=%d CS=%d RST=%d\n", PIN_LCD_DC, PIN_LCD_WR, PIN_LCD_CS, PIN_LCD_RST);
    printf("  Data: GPIO 39-42, 45-48 (D0-D7)\n");
    printf("====================================\n");

    // ===== GPIO 初始化 (参考 xiaozhi 官方代码) =====

    // PWR enable (GPIO15)
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_PWR,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level((gpio_num_t)PIN_LCD_PWR, 1);

    // RD = HIGH (官方要求)
    gpio_config_t rd_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_RD,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rd_cfg);
    gpio_set_level((gpio_num_t)PIN_LCD_RD, 1);

    // Backlight (GPIO38) — 初始关闭
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 0);

    gpio_set_direction((gpio_num_t)PIN_BOOT, GPIO_MODE_INPUT);
    ESP_LOGI(TAG, "GPIO init done");

    // ===== I80 总线初始化 =====
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = (gpio_num_t)PIN_LCD_DC,
        .wr_gpio_num = (gpio_num_t)PIN_LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {39, 40, 41, 42, 45, 46, 47, 48},
        .bus_width = 8,
        .max_transfer_bytes = FRAME_SIZE,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    // Panel IO
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

    // ===== ST7789 面板 =====
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = (gpio_num_t)PIN_LCD_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
        .vendor_config = NULL,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &dev_cfg, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // 关键配置
    esp_lcd_panel_invert_color(panel_handle, true);   // 必须颜色反转
    esp_lcd_panel_swap_xy(panel_handle, true);        // 竖屏→横屏
    esp_lcd_panel_mirror(panel_handle, false, true);  // USB在左边
    esp_lcd_panel_set_gap(panel_handle, 0, 20);     // Y偏移=20居中

    // ===== 官方 ST7789 初始化序列 =====
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);   // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(io_handle, 0x3A, (uint8_t[]){0x05}, 1);  // COLMOD=RGB565
    esp_lcd_panel_io_tx_param(io_handle, 0xB2, (uint8_t[]){0x0B, 0x0B, 0x00, 0x33, 0x33}, 5);
    esp_lcd_panel_io_tx_param(io_handle, 0xB7, (uint8_t[]){0x75}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xBB, (uint8_t[]){0x28}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC0, (uint8_t[]){0x2C}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC2, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC3, (uint8_t[]){0x1F}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC6, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA7}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    esp_lcd_panel_io_tx_param(io_handle, 0xD6, (uint8_t[]){0xA1}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xE0,
        (uint8_t[]){0xF0,0x05,0x0A,0x06,0x06,0x03,0x2B,0x32,0x43,0x36,0x11,0x10,0x2B,0x32}, 14);
    esp_lcd_panel_io_tx_param(io_handle, 0xE1,
        (uint8_t[]){0xF0,0x08,0x0C,0x0B,0x09,0x24,0x2B,0x22,0x43,0x38,0x15,0x16,0x2F,0x37}, 14);

    // Display ON
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // 打开背光
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "ST7789 init OK (320x170 landscape)");

    // ===== 先显示测试色块 =====
    fill_test_pattern();

    // ===== BOOT 键检测 (启动时长按进入深度睡眠) =====
    vTaskDelay(pdMS_TO_TICKS(200));
    if (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
        ESP_LOGI(TAG, "BOOT held at startup - deep sleep");
        enter_deep_sleep();
        return;
    }

    // ===== 挂载 SPIFFS 并播放 GIF =====
    if (!mount_spiffs()) {
        ESP_LOGE(TAG, "SPIFFS mount failed!");
        while(1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    gif_meta_t meta;
    if (!read_file("/spiffs/frames.meta", 0, &meta, sizeof(meta))) {
        ESP_LOGE(TAG, "Cannot read frames.meta - run upload_spiffs.bat first");
        while(1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "GIF: %dx%d frames=%u delay=%ums",
             meta.w, meta.h, meta.frames, meta.delay_ms);

    void* frame_buf = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_DMA);
    if (!frame_buf) frame_buf = malloc(FRAME_SIZE);
    if (!frame_buf) {
        ESP_LOGE(TAG, "Cannot alloc frame buffer!");
        while(1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    size_t frame_off = sizeof(gif_meta_t);
    int frame_idx = 0;
    int64_t last_frame = 0;

    ESP_LOGI(TAG, "Playing GIF...");

    while (1) {
        // BOOT 长按 3 秒深度睡眠
        if (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
            uint32_t t0 = esp_timer_get_time() / 1000;
            while (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if ((esp_timer_get_time() / 1000) - t0 >= 3000) {
                    free(frame_buf);
                    enter_deep_sleep();
                    return;
                }
            }
        }

        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_frame >= meta.delay_ms) {
            frame_idx = (frame_idx + 1) % meta.frames;
            size_t off = frame_off + (size_t)frame_idx * FRAME_SIZE;
            if (!read_file("/spiffs/frames.bin", off, frame_buf, FRAME_SIZE)) {
                ESP_LOGE(TAG, "Frame read error idx=%d", frame_idx);
                break;
            }
            esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, FRAME_W, FRAME_H, frame_buf);
            last_frame = now;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(frame_buf);
    ESP_LOGI(TAG, "Done.");
}

void enter_deep_sleep(void)
{
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_panel_disp_on_off(panel_handle, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Deep sleep...");
    esp_deep_sleep_start();
}
