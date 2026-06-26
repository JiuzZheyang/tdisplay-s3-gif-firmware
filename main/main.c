//
// T-Display-S3 GIF Player — Pure ESP-IDF 5.5
//
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"

static const char* TAG = "GIF";

#define PIN_LCD_MOSI  11
#define PIN_LCD_CLK    9
#define PIN_LCD_CS    10
#define PIN_LCD_DC     2
#define PIN_LCD_BL    15
#define PIN_LCD_RST    3

#define DISP_W 170
#define DISP_H 320
#define FRAME_SIZE (DISP_W * DISP_H * 2)

#define SPIFFS_OFFSET 0x1B0000

#define ST7789_NOP     0x00
#define ST7789_SWRESET 0x01
#define ST7789_SLPOUT  0x11
#define ST7789_SLPIN   0x10
#define ST7789_NORON   0x13
#define ST7789_DINVOFF 0x20
#define ST7789_DINVON  0x21
#define ST7789_CASET   0x2A
#define ST7789_RASET   0x2B
#define ST7789_RAMWR   0x2C
#define ST7789_MADCTL  0x36
#define ST7789_COLMOD  0x3A
#define ST7789_DISPON  0x29

spi_device_handle_t spi;

typedef struct {
    uint16_t w;
    uint16_t h;
    uint16_t frames;
    uint16_t delay_ms;
} gif_meta_t;

static void st7789_wr_cmd(uint8_t cmd)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = (void*)0;
    spi_device_transmit(spi, &t);
}

static void st7789_wr_data(const uint8_t* data, int len)
{
    if (len == 0) return;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (size_t)len * 8;
    t.tx_buffer = data;
    t.user = (void*)1;
    spi_device_transmit(spi, &t);
}

static void st7789_set_win(int x1, int y1, int x2, int y2)
{
    uint8_t ca[4] = {
        (uint8_t)((x1 >> 8) & 0xFF), (uint8_t)(x1 & 0xFF),
        (uint8_t)((x2 >> 8) & 0xFF), (uint8_t)(x2 & 0xFF)
    };
    uint8_t ra[4] = {
        (uint8_t)((y1 >> 8) & 0xFF), (uint8_t)(y1 & 0xFF),
        (uint8_t)((y2 >> 8) & 0xFF), (uint8_t)(y2 & 0xFF)
    };
    st7789_wr_cmd(ST7789_CASET);
    st7789_wr_data(ca, 4);
    st7789_wr_cmd(ST7789_RASET);
    st7789_wr_data(ra, 4);
    st7789_wr_cmd(ST7789_RAMWR);
}

static void st7789_init(void)
{
    gpio_set_direction((gpio_num_t)PIN_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    st7789_wr_cmd(ST7789_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));
    st7789_wr_cmd(ST7789_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(255));

    st7789_wr_cmd(ST7789_MADCTL);
    st7789_wr_data((uint8_t[]){ 0x00 }, 1);

    st7789_wr_cmd(ST7789_COLMOD);
    st7789_wr_data((uint8_t[]){ 0x55 }, 1);

    gpio_set_level((gpio_num_t)PIN_LCD_BL, 1);
    st7789_wr_cmd(ST7789_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    st7789_wr_cmd(ST7789_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "ST7789 OK");
}

static void st7789_draw_frame(const uint16_t* rgb565)
{
    st7789_set_win(0, 0, DISP_W - 1, DISP_H - 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (size_t)FRAME_SIZE * 8;
    t.tx_buffer = rgb565;
    t.user = (void*)1;
    spi_device_transmit(spi, &t);
}

static void st7789_clear(void)
{
    static uint16_t black[DISP_W] = {0};
    for (int y = 0; y < DISP_H; y++) {
        st7789_set_win(0, y, DISP_W - 1, y);
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = (size_t)DISP_W * 2 * 8;
        t.tx_buffer = black;
        t.user = (void*)1;
        spi_device_transmit(spi, &t);
    }
}

static void power_off(void)
{
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    st7789_wr_cmd(ST7789_SLPIN);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "Deep sleep...");
    esp_deep_sleep_start();
}

static void spi_init(void)
{
    spi_bus_config_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.mosi_io_num = PIN_LCD_MOSI;
    bus.miso_io_num = -1;
    bus.sclk_io_num = PIN_LCD_CLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;

    spi_device_interface_config_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.clock_speed_hz = 40000000;
    dev.mode = 2;
    dev.spics_io_num = PIN_LCD_CS;
    dev.queue_size = 1;
    dev.flags = SPI_DEVICE_NO_DUMMY;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));
    ESP_LOGI(TAG, "SPI OK @ 40 MHz");
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

#define PIN_BOOT  0

void app_main(void)
{
    printf("\n====================================\n");
    printf("  T-Display-S3 GIF Player\n");
    printf("  Pure ESP-IDF 5.5\n");
    printf("====================================\n");

    gpio_set_direction((gpio_num_t)PIN_BOOT, GPIO_MODE_INPUT);

    spi_init();
    st7789_init();

    gif_meta_t meta;
    if (!read_file("/spiffs/frames.meta", 0, &meta, sizeof(meta))) {
        ESP_LOGE(TAG, "Cannot read frames.meta - run upload_spiffs.bat first");
        return;
    }
    ESP_LOGI(TAG, "GIF: %dx%d  frames=%u  delay=%u ms",
             meta.w, meta.h, meta.frames, meta.delay_ms);

    void* frame_buf = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!frame_buf) {
        ESP_LOGW(TAG, "PSRAM unavailable, using DRAM");
        frame_buf = malloc(FRAME_SIZE);
    }
    if (!frame_buf) {
        ESP_LOGE(TAG, "Cannot alloc frame buffer");
        return;
    }

    size_t frame_off = sizeof(gif_meta_t);

    st7789_clear();

    if (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level((gpio_num_t)PIN_BOOT) == 0) {
            ESP_LOGI(TAG, "BOOT held at startup - sleep");
            free(frame_buf);
            power_off();
            return;
        }
    }

    ESP_LOGI(TAG, "Playing %u frames...", meta.frames);

    int frame_idx = 0;
    int64_t last_frame = 0;

    while (1) {
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

        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_frame >= meta.delay_ms) {
            frame_idx = (frame_idx + 1) % meta.frames;
            size_t off = frame_off + (size_t)frame_idx * FRAME_SIZE;
            if (!read_file("/spiffs/frames.bin", off, frame_buf, FRAME_SIZE)) {
                ESP_LOGE(TAG, "Frame read error idx=%d", frame_idx);
                break;
            }
            st7789_draw_frame((uint16_t*)frame_buf);
            last_frame = now;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(frame_buf);
    ESP_LOGI(TAG, "Done.");
}
