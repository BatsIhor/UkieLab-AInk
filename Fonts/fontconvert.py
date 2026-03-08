#!python3
import freetype
import zlib
import sys
import re
import math
import argparse
from collections import namedtuple
"""
py .\fontconvert.py --compress --min 48 --max 250 OpenSans24B 24 opensans-bold.ttf > opensans24b.h

python3 fontconvert.py --compress all demo 16 msyh.ttc> font.h
python3 fontconvert.py --compress max 255 demo 16 msyh.ttc> font.h

Explanation of specific parameters:

python3 fontconvert.py --compress [generated font name] [font size] [font file path]> [generated font file]

now has three new arguments to build the intervals on the fly

--all create bitmaps for all in the file
--max get all chars up to this characters unicode
--min get all chars starting from to this characters unicode
it also sets the defaults to the max in all included characters

sometimes a font will fail on all try setting the min and max to a smaller range 
some files dont report valid characters



"""

parser = argparse.ArgumentParser(description="Generate a header file from a font to be used with epdiy.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--compress", dest="compress", action="store_true", help="compress glyph bitmaps.")
parser.add_argument("--all", dest="all_chars", action="store_true", help="use all available characters in the file")
parser.add_argument("--max", type=int, help="get all chars up to this characters number .")
parser.add_argument("--min", type=int, help="get all chars from this characters number .")
args = parser.parse_args()

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "compressed_size", "data_offset", "code_point"])

font_stack = [freetype.Face(f) for f in args.fontstack]
compress = args.compress
size = args.size
font_name = args.name

for face in font_stack:
    # shift by 6 bytes, because sizes are given as 6-bit fractions
    # the display has about 150 dpi.
    face.set_char_size(size << 6, size << 6, 150, 150)

# inclusive unicode code point intervals
# must not overlap and be in ascending order
if args.all_chars or args.max :
  maximum = args.max
  if maximum == None:
    maximum = 0x10FFFF
  intervals = []
  minimum = args.min
  next_char = face.get_first_char()
  #I know there is a better way but this is clear and simple
  if minimum == None:
    minimum = 32;
  while  next_char[0] < minimum: #we just go over list of chars untill get where we need to start :) 
    next_char = face.get_next_char(next_char[0], next_char[1])

  low_int = next_char[0]
  while  next_char[0] != 0:
    old_char = next_char
    next_char = face.get_next_char(old_char[0], old_char[1])
    #print(next_char, file=sys.stderr)
    if old_char[0] + 1 != next_char[0] or next_char[0] > maximum:
      interval = [low_int, old_char[0]]
      intervals.append(interval)
      low_int = next_char[0]
    if next_char[0] > maximum:
      #print(next_char[0], file=sys.stderr)
      #print(next_char, file=sys.stderr)
      break # continue here until we get to degree sign.
      #continue
else:
  #  Below is where you deffine specific intervals if not using min max or all

  intervals = [
      (32,34),
  ]


def norm_floor(val):
    return int(math.floor(val / (1 << 6)))

def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))

def chunks(l, n):
    for i in range(0, len(l), n):
        yield l[i:i + n]

total_size = 0
total_packed = 0
all_glyphs = []
total_chars = 0
ascender = 0
descender = 100
f_height = 0

def load_glyph(code_point):
    face_index = 0
    while face_index < len(font_stack):
        face = font_stack[face_index]
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER)
            global ascender
            if ascender < face.size.ascender:
              ascender = face.size.ascender
            global descender
            if descender > face.size.descender:
              descender = face.size.descender
            global f_height
            if f_height < face.size.height:
              f_height = face.size.height
            global total_chars
            total_chars += 1
            return face
            break
        face_index += 1
        print (f"falling back to font {face_index} for {chr(code_point)}({code_point}).", file=sys.stderr)
    raise ValueError(f"code point {code_point} not found in font stack!")

for i_start, i_end in intervals:
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        print (f"\n{chr(code_point)}", end ="", file=sys.stderr)
        if chr(code_point) == " ":
           print (f"\n YOOOO!\n !{chr(code_point)}", end ="!", file=sys.stderr)
           #continue
        bitmap = face.glyph.bitmap
        pixels = []
        px = 0
        for i, v in enumerate(bitmap.buffer):
            y = i / bitmap.width
            x = i % bitmap.width
            if x % 2 == 0:
                px = (v >> 4)
            else:
                px = px | (v & 0xF0)
                pixels.append(px)
                px = 0
            # eol
            if x == bitmap.width - 1 and bitmap.width % 2 > 0:
                pixels.append(px)
                px = 0

        packed = bytes(pixels)
        total_packed += len(packed)
        compressed = packed
        if compress:
            compressed = zlib.compress(packed)

        glyph = GlyphProps(
            width = bitmap.width,
            height = bitmap.rows,
            advance_x = norm_floor(face.glyph.advance.x),
            left = face.glyph.bitmap_left,
            top = face.glyph.bitmap_top,
            compressed_size = len(compressed),
            data_offset = total_size,
            code_point = code_point,
        )
        total_size += len(compressed)
        all_glyphs.append((glyph, compressed))

# pipe seems to be a good heuristic for the "real" descender
#///  on  19 February 2022↴
# needs to be a little less specific

# ~ face = load_glyph(ord('|'))

glyph_data = []
glyph_props = []
for index, glyph in enumerate(all_glyphs):
    props, compressed = glyph
    glyph_data.extend([b for b in compressed])
    glyph_props.append(props)

print(f"\nFrom {args.fontstack} with {total_chars} characters", file=sys.stderr)
print("total", total_packed, file=sys.stderr)
print("compressed", total_size, file=sys.stderr)

print("#pragma once")
print("#include \"epd_driver.h\"")
print(f"/*\n Available {total_chars} characters")
for i, g in enumerate(glyph_props):
    print (f"{chr(g.code_point)}", end ="" )
print("\n*/") 

print(f"const uint8_t {font_name}Bitmaps[{len(glyph_data)}] = {{")
for c in chunks(glyph_data, 16):
    print ("    " + " ".join(f"0x{b:02X}," for b in c))
print ("};");

print(f"const GFXglyph {font_name}Glyphs[] = {{")
for i, g in enumerate(glyph_props):
    print ("    { " + ", ".join([f"{a}" for a in list(g[:-1])]),"},", f"// code point {g.code_point} {chr(g.code_point) if g.code_point != 92 else '<backslash>'}")
print ("};");

print(f"const UnicodeInterval {font_name}Intervals[] = {{")
offset = 0
for i_start, i_end in intervals:
    print (f"    {{ 0x{i_start:X}, 0x{i_end:X}, 0x{offset:X} }},")
    offset += i_end - i_start + 1
print ("};");

print(f"const GFXfont {font_name} = {{")
print(f"    (uint8_t*){font_name}Bitmaps,")
print(f"    (GFXglyph*){font_name}Glyphs,")
print(f"    (UnicodeInterval*){font_name}Intervals,")
print(f"    {len(intervals)},")
print(f"    {1 if compress else 0},")
print(f"    {norm_ceil(f_height)},")
print(f"    {norm_ceil(ascender)},")
print(f"    {norm_floor(descender)},")
print("};")
print("/* \nintervals = ")  
for i_start, i_end in intervals:
    print (f"    ( {i_start}, {i_end},), # {chr(i_start)} -  {chr(i_end)}")
print("\n*/") 
