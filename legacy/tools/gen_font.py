from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
chars = [chr(c) for c in range(0x20, 0x7F)]

best = None
for size in range(10, 18):
    f = ImageFont.truetype(FONT, size)
    ok = True
    for ch in chars:
        bb = f.getbbox(ch)
        if bb is None: continue
        w = bb[2]-bb[0]; h = bb[3]-bb[1]
        if w > 8 or h > 15:
            ok = False; break
    if ok:
        best = size
    else:
        break
assert best, "no size fits"
print("chosen font size:", best)

f = ImageFont.truetype(FONT, best)
rows = []
for ch in chars:
    img = Image.new("1", (8, 16), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=f, fill=1)
    data = list(img.get_flattened_data() if hasattr(img,"get_flattened_data") else img.getdata())
    glyph = []
    for y in range(16):
        b = 0
        for x in range(8):
            if data[y*8+x]:
                b |= 1 << (7-x)
        glyph.append(b)
    rows.append((ch, glyph))

with open("/workspace/aether-linux/aether-font.h", "w") as out:
    out.write("/*\n")
    out.write(" * aether-font.h - 8x16 bitmap font, generated from DejaVu Sans Mono\n")
    out.write(" * (Bitstream Vera derived, free license) at size %d via tools/gen_font.py.\n")
    out.write(" * One byte per scanline, MSB = leftmost pixel. ASCII 0x20-0x7E.\n")
    out.write(" */\n")
    out.write("#ifndef AETHER_FONT_H\n#define AETHER_FONT_H\n\n")
    out.write("#define AETHER_FONT_W 8u\n#define AETHER_FONT_H 16u\n\n")
    out.write("static const unsigned char aether_font_8x16[95][16] = {\n")
    for ch, glyph in rows:
        out.write("    { /* %s */ " % (ch if ch not in '\\*"?' else {'\\':'\\\\','*':'\\*','"':'\\"','?':'\\?'}[ch] if ch in '\\*"?' else ch))
        out.write(", ".join("0x%02x" % b for b in glyph))
        out.write(" },\n")
    out.write("};\n\n#endif /* AETHER_FONT_H */\n")
print("written /workspace/aether-linux/aether-font.h")
