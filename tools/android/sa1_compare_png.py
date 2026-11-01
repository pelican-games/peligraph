#!/usr/bin/env python3
"""Dependency-free SA1b PNG comparison (8-bit RGB/RGBA, noninterlaced)."""
import collections
import json
import math
import struct
import sys
import zlib
from pathlib import Path

def read_png(path):
    data = Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    position, compressed = 8, bytearray()
    while position < len(data):
        length = int.from_bytes(data[position:position+4], 'big')
        kind = data[position+4:position+8]
        body = data[position+8:position+8+length]
        position += length + 12
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and color in (2, 6) and interlace == 0
            channels = 3 if color == 2 else 4
        if kind == b'IDAT':
            compressed.extend(body)
    raw = zlib.decompress(compressed)
    stride, previous, pixels = width * channels, bytearray(width * channels), bytearray()
    for y in range(height):
        filter_type = raw[y * (stride + 1)]
        scan = bytearray(raw[y * (stride + 1) + 1:(y+1)*(stride+1)])
        for x in range(stride):
            a = scan[x-channels] if x >= channels else 0
            b = previous[x]
            c = previous[x-channels] if x >= channels else 0
            if filter_type == 1: predictor = a
            elif filter_type == 2: predictor = b
            elif filter_type == 3: predictor = (a+b)//2
            elif filter_type == 4:
                p = a+b-c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
            else:
                assert filter_type == 0
                predictor = 0
            scan[x] = (scan[x] + predictor) & 255
        previous = scan
        for x in range(0, stride, channels):
            pixels.extend(scan[x:x+3])
    return width, height, pixels

def write_png(path, width, height, rgb):
    def chunk(kind, data):
        return struct.pack('>I',len(data)) + kind + data + struct.pack('>I',zlib.crc32(kind+data))
    rows = b''.join(b'\0' + rgb[y*width*3:(y+1)*width*3] for y in range(height))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0)) + chunk(b'IDAT',zlib.compress(rows)) + chunk(b'IEND',b''))

def compare(first, second, out):
    w,h,a = read_png(first)
    ww,hh,b = read_png(second)
    assert (w,h)==(ww,hh)
    histogram = collections.Counter(abs(x-y) for x,y in zip(a,b))
    total = len(a)
    mae = sum(k*v for k,v in histogram.items())/total
    rmse = math.sqrt(sum(k*k*v for k,v in histogram.items())/total)
    cumulative = 0
    for p95,count in sorted(histogram.items()):
        cumulative += count
        if cumulative >= total*0.95: break
    changed = gt8 = 0
    diff = bytearray()
    backgrounds = [collections.Counter(bytes(p[i:i+3]) for i in range(0,len(p),3)).most_common(1)[0][0] for p in (a,b)]
    masks = [set(i//3 for i in range(0,len(p),3) if p[i:i+3] != background) for p,background in zip((a,b),backgrounds)]
    for i in range(0,total,3):
        delta = [abs(a[j]-b[j]) for j in range(i,i+3)]
        changed += max(delta)>0
        gt8 += max(delta)>8
        diff.extend(min(255,x*16) for x in delta)
    def bbox(mask):
        return [min(i%w for i in mask),min(i//w for i in mask),max(i%w for i in mask),max(i//w for i in mask)]
    stats = {'size':[w,h], 'rgb_8bit_mae':mae, 'rgb_8bit_rmse':rmse,
        'rgb_8bit_max':max(histogram), 'rgb_8bit_p95':p95,
        'changed_pixels':changed, 'changed_pixel_fraction':changed/(w*h),
        'pixels_any_channel_diff_gt8':gt8, 'fraction_diff_gt8':gt8/(w*h),
        'foreground_iou':len(masks[0]&masks[1])/len(masks[0]|masks[1]),
        'foreground_bboxes':[bbox(m) for m in masks], 'foreground_pixels':[len(m) for m in masks]}
    montage = bytearray()
    for y in range(h):
        montage.extend(a[y*w*3:(y+1)*w*3]); montage.extend(b'\0'*24); montage.extend(b[y*w*3:(y+1)*w*3])
    out = Path(out)
    write_png(out.with_suffix('.png'), w*2+8,h,montage)
    write_png(out.with_name(out.stem+'-diff16x.png'),w,h,diff)
    out.with_suffix('.json').write_text(json.dumps(stats,indent=2)+'\n')
    print(json.dumps(stats,indent=2))

if __name__=='__main__': compare(*sys.argv[1:])
