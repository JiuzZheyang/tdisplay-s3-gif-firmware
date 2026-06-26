# T-Display-S3 GIF Player

Pure ESP-IDF 5.5 firmware for LilyGO T-Display-S3.

## Features
- Plays pre-converted RGB565 GIF frames from SPIFFS
- Long-press BOOT (3s) → deep sleep
- 170x320 ST7789 SPI display
- PSRAM frame buffer

## Build with GitHub Actions

1. Fork this repo
2. Go to **Actions** → **Build Firmware** → **Run workflow**
3. Download artifacts after build

## Manual Build (ESP-IDF 5.5)

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM10 flash
```

## Upload SPIFFS data

```bash
python esptool.py --chip esp32s3 --port COM10 write_flash 0x1B0000 data/frames.bin
```

##换自己的GIF

修改 `convert_gif.py` 中的 GIF 路径，运行转换，上传新的 `frames.bin`。
