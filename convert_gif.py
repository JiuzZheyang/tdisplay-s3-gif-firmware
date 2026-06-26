#!/usr/bin/env python3
"""
GIF → frames.bin converter for T-Display-S3 firmware
====================================================
Converts any GIF to the raw RGB565 binary format used by the firmware.

Usage:
    python3 convert_gif.py input.gif [output_dir]

Output:
    frames.bin   — RGB565 little-endian, all frames concatenated
    frames.meta  — uint16_t: [W, H, frame_count, delay_ms]
"""

import sys
import struct
import os
from PIL import Image

def convert_gif(gif_path, out_dir='.'):
    img = Image.open(gif_path)
    W, H = img.size
    total = img.n_frames
    delay_ms = img.info.get('duration', 80)

    print(f"Converting: {gif_path}")
    print(f"  Size: {W}x{H}")
    print(f"  Frames: {total}")
    print(f"  Delay: {delay_ms}ms ({1000/delay_ms:.1f} fps)")

    # Write metadata
    meta_path = os.path.join(out_dir, 'frames.meta')
    with open(meta_path, 'wb') as f:
        f.write(struct.pack('<HHHH', W, H, total, delay_ms))
    print(f"  → {meta_path} ({os.path.getsize(meta_path)} bytes)")

    # Write all frames as RGB565 little-endian
    frames_path = os.path.join(out_dir, 'frames.bin')
    total_bytes = 0
    with open(frames_path, 'wb') as f:
        for i in range(total):
            img.seek(i)
            frame = img.convert('RGB')
            pixels = list(frame.getdata())
            # RGB565: RRRRRGGG GGGBBBBB
            rgb565 = struct.pack(f'<{W*H}H', *[
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                for r, g, b in pixels
            ])
            f.write(rgb565)
            total_bytes += len(rgb565)

    print(f"  → {frames_path} ({total_bytes/1024:.1f} KB total)")

    per_frame_kb = (W * H * 2) / 1024
    print(f"\nPer frame: {per_frame_kb:.1f} KB")
    print(f"Total RAM needed on ESP32: {per_frame_kb:.1f} KB (1 frame buffer)")
    print(f"SPIFFS storage: {total_bytes/1024:.1f} KB")
    print(f"\nFirmware constants to update in main.cpp:")
    print(f"  #define GIF_W     {W}")
    print(f"  #define GIF_H     {H}")
    print(f"  #define GIF_FRAMES {total}")
    print(f"  #define GIF_DELAY_MS {delay_ms}")
    print(f"\nDone! Upload with: pio run --target uploadfs")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 convert_gif.py input.gif [output_dir]")
        sys.exit(1)

    gif_path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else '.'

    if not os.path.exists(gif_path):
        print(f"Error: file not found: {gif_path}")
        sys.exit(1)

    convert_gif(gif_path, out_dir)
