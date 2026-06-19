#!/bin/bash
# Take a screenshot from the Waveshare AMOLED display.
# PSRAM boards (S3) snapshot the LVGL framebuffer; PSRAM-free boards (C6)
# stream the frame strip-by-strip. Either way the host receives contiguous
# row-major RGB565, so this script handles both.
# Usage: ./screenshot.sh [output.png] [port]
# Default port: /dev/cu.usbmodem101 on macOS, /dev/ttyACM0 on Linux.

OUTPUT="${1:-screenshot.png}"
if [ -z "$2" ]; then
    case "$(uname -s)" in
        Darwin) PORT="/dev/cu.usbmodem101" ;;
        *)      PORT="/dev/ttyACM0" ;;
    esac
else
    PORT="$2"
fi

# Use pio's bundled python if pyserial isn't on the system python.
PY="python3"
if ! python3 -c "import serial" 2>/dev/null; then
    if [ -x "$HOME/.platformio/penv/bin/python" ]; then
        PY="$HOME/.platformio/penv/bin/python"
    fi
fi

echo "Taking screenshot from $PORT..."

# Capture + RGB565->PNG happen in one python process. PNG is written with the
# stdlib (struct + zlib), so no ffmpeg/Pillow dependency on the host.
"$PY" - "$PORT" "$OUTPUT" << 'PYEOF'
import serial, sys, struct, zlib

port_path, out_path = sys.argv[1], sys.argv[2]

port = serial.Serial(port_path, 115200, timeout=10)
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

w = h = raw_size = None
while True:
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line.startswith("SCREENSHOT_START"):
        parts = line.split()
        w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
        break
    if line == "SCREENSHOT_ERR":
        print("Device reported screenshot error", file=sys.stderr)
        sys.exit(1)
    if line == "SCREENSHOT_UNSUPPORTED":
        print("Device firmware reports screenshot unsupported on this board", file=sys.stderr)
        sys.exit(1)

data = b""
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        print(f"Timeout: got {len(data)} of {raw_size} bytes", file=sys.stderr)
        sys.exit(1)
    data += chunk

for _ in range(10):
    if port.readline().decode("utf-8", errors="replace").strip() == "SCREENSHOT_END":
        break
port.close()

# RGB565 little-endian -> RGB888 (bit replication keeps full-range channels).
px = struct.unpack(f"<{w * h}H", data)
rgb = bytearray(w * h * 3)
i = 0
for v in px:
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    rgb[i]     = (r << 3) | (r >> 2)
    rgb[i + 1] = (g << 2) | (g >> 4)
    rgb[i + 2] = (b << 3) | (b >> 2)
    i += 3

# Prefix each scanline with filter byte 0 (none), as PNG requires.
stride = w * 3
raw = bytearray()
for y in range(h):
    raw.append(0)
    raw += rgb[y * stride:(y + 1) * stride]

def png_chunk(tag, body):
    c = tag + body
    return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n"
png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))  # 8-bit truecolor
png += png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
png += png_chunk(b"IEND", b"")

with open(out_path, "wb") as f:
    f.write(png)

print(f"Saved: {out_path} ({w}x{h}, {len(data)} bytes raw)")
PYEOF

if [ $? -ne 0 ]; then
    echo "Screenshot failed"
    exit 1
fi
