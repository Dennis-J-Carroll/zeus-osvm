#!/usr/bin/env python3
"""Convert assets/logo.jpg to an ASCII-art boot splash."""
from PIL import Image, ImageFilter
import sys

# Ramp from dark background to bright figure.
ASCII_CHARS = " .`^,:;Il!i~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$"


def resize(image, new_width=70):
    width, height = image.size
    ratio = height / width
    new_height = int(new_width * ratio * 0.52)
    return image.resize((new_width, new_height))


def to_ascii(image, width=70):
    image = resize(image, width).convert("L")
    # Slight sharpening to bring out edges.
    image = image.filter(ImageFilter.SHARPEN)
    pixels = list(image.getdata())
    n = len(ASCII_CHARS)
    chars = "".join(ASCII_CHARS[min(p * n // 256, n - 1)] for p in pixels)
    lines = [chars[i:i + width].rstrip() for i in range(0, len(chars), width)]
    return "\n".join(lines)


if __name__ == "__main__":
    in_path = sys.argv[1] if len(sys.argv) > 1 else "assets/logo.jpg"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "assets/logo_ascii.txt"

    with Image.open(in_path) as img:
        art = to_ascii(img, width=70)

    banner = [
        "",
        "  ZZZZZ  EEEEE  UUU  UUU   SSSS   __   OOO   SSSS ",
        "     ZZ  EE     UU UU UU  SS     |  | O   O  SS   ",
        "    ZZ   EEEE   UU UU UU   SSSS  |  | O   O   SSSS",
        "   ZZ    EE     UU UU UU      SS |  | O   O      SS",
        "  ZZZZZ  EEEEE   UUU UUU   SSSS  |__|  OOO   SSSS ",
        "",
        "           Virtual Machine / Network Playground",
        "",
    ]

    with open(out_path, "w") as f:
        f.write(art + "\n")
        f.write("\n".join(banner) + "\n")
    print(f"Wrote {out_path}")
