#!/usr/bin/env python3
"""PT-1a image and shader checks using only the Python standard library."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import struct
import sys
from typing import Iterable, Sequence
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class CheckError(RuntimeError):
    """A malformed input or invalid check request."""


def _paeth(left: int, up: int, upper_left: int) -> int:
    estimate = left + up - upper_left
    left_distance = abs(estimate - left)
    up_distance = abs(estimate - up)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= up_distance and left_distance <= upper_left_distance:
        return left
    if up_distance <= upper_left_distance:
        return up
    return upper_left


def read_png(path: Path) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise CheckError(f"{path}: not a PNG file")

    offset = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    saw_iend = False
    while offset < len(data):
        if offset + 12 > len(data):
            raise CheckError(f"{path}: truncated PNG chunk")
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4 : offset + 8]
        payload_start = offset + 8
        payload_end = payload_start + length
        chunk_end = payload_end + 4
        if chunk_end > len(data):
            raise CheckError(f"{path}: truncated {chunk_type!r} chunk")
        payload = data[payload_start:payload_end]
        if chunk_type == b"IHDR":
            if length != 13 or width is not None:
                raise CheckError(f"{path}: invalid IHDR")
            (
                width,
                height,
                bit_depth,
                color_type,
                compression,
                filter_method,
                interlace,
            ) = struct.unpack(">IIBBBBB", payload)
            if width == 0 or height == 0:
                raise CheckError(f"{path}: empty PNG")
            if compression != 0 or filter_method != 0:
                raise CheckError(f"{path}: unsupported PNG compression/filter method")
        elif chunk_type == b"IDAT":
            compressed.extend(payload)
        elif chunk_type == b"IEND":
            saw_iend = True
            break
        offset = chunk_end

    if width is None or not compressed or not saw_iend:
        raise CheckError(f"{path}: incomplete PNG")
    if bit_depth != 8 or color_type not in (2, 6):
        raise CheckError(
            f"{path}: expected 8-bit RGB/RGBA PNG, got bit_depth={bit_depth} "
            f"color_type={color_type}"
        )
    if interlace != 0:
        raise CheckError(f"{path}: interlaced PNG is not supported")

    channels = 3 if color_type == 2 else 4
    stride = width * channels
    decoded = zlib.decompress(bytes(compressed))
    expected_size = height * (stride + 1)
    if len(decoded) != expected_size:
        raise CheckError(
            f"{path}: decoded byte count {len(decoded)} != expected {expected_size}"
        )

    rows: list[bytearray] = []
    cursor = 0
    prior = bytearray(stride)
    for row_index in range(height):
        filter_type = decoded[cursor]
        cursor += 1
        scanline = decoded[cursor : cursor + stride]
        cursor += stride
        reconstructed = bytearray(stride)
        for index, encoded in enumerate(scanline):
            left = reconstructed[index - channels] if index >= channels else 0
            up = prior[index]
            upper_left = prior[index - channels] if index >= channels else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                predictor = _paeth(left, up, upper_left)
            else:
                raise CheckError(
                    f"{path}: unsupported PNG filter {filter_type} on row {row_index}"
                )
            reconstructed[index] = (encoded + predictor) & 0xFF
        rows.append(reconstructed)
        prior = reconstructed

    pixels: list[tuple[int, int, int, int]] = []
    for row in rows:
        for index in range(0, stride, channels):
            red, green, blue = row[index : index + 3]
            alpha = row[index + 3] if channels == 4 else 255
            pixels.append((red, green, blue, alpha))
    return width, height, pixels


def srgb_channel_to_linear(encoded: int) -> float:
    value = encoded / 255.0
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def linear_luminance(pixel: tuple[int, int, int, int]) -> float:
    red = srgb_channel_to_linear(pixel[0])
    green = srgb_channel_to_linear(pixel[1])
    blue = srgb_channel_to_linear(pixel[2])
    return 0.2126 * red + 0.7152 * green + 0.0722 * blue


def parse_pair(value: str) -> tuple[float, float]:
    fields = value.split(",")
    if len(fields) != 2:
        raise argparse.ArgumentTypeError("expected cx,cy")
    try:
        return float(fields[0]), float(fields[1])
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected numeric cx,cy") from error


def parse_rect(value: str) -> tuple[int, int, int, int]:
    fields = value.split(",")
    if len(fields) != 4:
        raise argparse.ArgumentTypeError("expected x0,y0,x1,y1")
    try:
        return tuple(int(field) for field in fields)  # type: ignore[return-value]
    except ValueError as error:
        raise argparse.ArgumentTypeError("rectangle coordinates must be integers") from error


def pixels_in_rect(
    width: int,
    height: int,
    pixels: Sequence[tuple[int, int, int, int]],
    rect: tuple[int, int, int, int],
) -> list[tuple[int, int, int, int]]:
    x0, y0, x1, y1 = rect
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        raise CheckError(f"rectangle {rect} is outside {width}x{height}")
    return [pixels[y * width + x] for y in range(y0, y1) for x in range(x0, x1)]


def mean(values: Sequence[float]) -> float:
    if not values:
        raise CheckError("selection contains no pixels")
    return math.fsum(values) / len(values)


def population_stddev(values: Sequence[float]) -> float:
    average = mean(values)
    variance = math.fsum((value - average) ** 2 for value in values) / len(values)
    return math.sqrt(variance)


def run_furnace(arguments: argparse.Namespace) -> int:
    width, height, pixels = read_png(arguments.png)
    center_x, center_y = arguments.center
    radius_squared = arguments.radius * arguments.radius
    selected = [
        pixels[y * width + x]
        for y in range(height)
        for x in range(width)
        if (x + 0.5 - center_x) ** 2 + (y + 0.5 - center_y) ** 2
        <= radius_squared
    ]
    average = mean([linear_luminance(pixel) for pixel in selected])
    passed = abs(average - arguments.expect) <= arguments.tol
    print(
        f"furnace mean={average:.9f} pixels={len(selected)} "
        f"expect={arguments.expect:.9f} tol={arguments.tol:.9f} "
        f"status={'pass' if passed else 'fail'}"
    )
    return 0 if passed else 1


def run_sky(arguments: argparse.Namespace) -> int:
    width, height, pixels = read_png(arguments.png)
    selected = pixels_in_rect(width, height, pixels, arguments.rect)
    average = mean([linear_luminance(pixel) for pixel in selected])
    passed = abs(average - arguments.expect) <= arguments.tol
    print(
        f"sky mean={average:.9f} pixels={len(selected)} "
        f"expect={arguments.expect:.9f} tol={arguments.tol:.9f} "
        f"status={'pass' if passed else 'fail'}"
    )
    return 0 if passed else 1


def run_noise(arguments: argparse.Namespace) -> int:
    width_a, height_a, pixels_a = read_png(arguments.png_a)
    width_b, height_b, pixels_b = read_png(arguments.png_b)
    if (width_a, height_a) != (width_b, height_b):
        raise CheckError(
            f"PNG dimensions differ: {width_a}x{height_a} vs {width_b}x{height_b}"
        )
    selected_a = pixels_in_rect(width_a, height_a, pixels_a, arguments.rect)
    selected_b = pixels_in_rect(width_b, height_b, pixels_b, arguments.rect)
    sigma_a = population_stddev([linear_luminance(pixel) for pixel in selected_a])
    sigma_b = population_stddev([linear_luminance(pixel) for pixel in selected_b])
    if sigma_b == 0.0:
        ratio = math.inf if sigma_a > 0.0 else 1.0
    else:
        ratio = sigma_a / sigma_b
    passed = ratio >= arguments.min_ratio
    print(
        f"noise sigma_a={sigma_a:.9f} sigma_b={sigma_b:.9f} "
        f"ratio={ratio:.9f} pixels={len(selected_a)} "
        f"minimum={arguments.min_ratio:.9f} "
        f"status={'pass' if passed else 'fail'}"
    )
    return 0 if passed else 1


def run_noise_pair(arguments: argparse.Namespace) -> int:
    """Estimate Monte Carlo noise from accumulated frames without spatial bias.

    A_n is the mean of the first n samples and A_2n the mean of the first 2n.
    The second half's mean is B = 2 * A_2n - A_n, independent of A_n, so the
    per-pixel standard deviation of (A_n - A_2n) equals sigma_n / sqrt(2)
    regardless of how much spatial structure the region contains.
    """
    def sigma(path_n: Path, path_2n: Path) -> tuple[float, int]:
        width_n, height_n, pixels_n = read_png(path_n)
        width_2n, height_2n, pixels_2n = read_png(path_2n)
        if (width_n, height_n) != (width_2n, height_2n):
            raise CheckError("noise-pair images differ in size")
        selected_n = pixels_in_rect(width_n, height_n, pixels_n, arguments.rect)
        selected_2n = pixels_in_rect(width_2n, height_2n, pixels_2n, arguments.rect)
        differences = [
            linear_luminance(a) - linear_luminance(b)
            for a, b in zip(selected_n, selected_2n)
        ]
        return population_stddev(differences) * math.sqrt(2.0), len(differences)

    sigma_a, count = sigma(arguments.a_n, arguments.a_2n)
    sigma_b, _ = sigma(arguments.b_n, arguments.b_2n)
    ratio = sigma_a / sigma_b if sigma_b > 0.0 else float("inf")
    passed = ratio >= arguments.min_ratio
    print(
        f"noise-pair sigma_a={sigma_a:.9f} sigma_b={sigma_b:.9f} ratio={ratio:.9f} "
        f"pixels={count} minimum={arguments.min_ratio:.9f} status={'pass' if passed else 'fail'}"
    )
    return 0 if passed else 1


def run_identical(arguments: argparse.Namespace) -> int:
    data_a = arguments.a.read_bytes()
    data_b = arguments.b.read_bytes()
    passed = data_a == data_b
    print(
        f"identical bytes_a={len(data_a)} bytes_b={len(data_b)} "
        f"status={'pass' if passed else 'fail'}"
    )
    return 0 if passed else 1


def shader_files(directory: Path, excluded: set[str]) -> dict[str, bytes]:
    if not directory.is_dir():
        raise CheckError(f"{directory}: not a directory")
    result: dict[str, bytes] = {}
    for path in sorted(candidate for candidate in directory.rglob("*") if candidate.is_file()):
        relative = path.relative_to(directory).as_posix()
        if relative in excluded or path.name in excluded:
            continue
        result[relative] = path.read_bytes()
    return result


def run_shaders_identical(arguments: argparse.Namespace) -> int:
    excluded = set(arguments.excluded)
    files_a = shader_files(arguments.dir_a, excluded)
    files_b = shader_files(arguments.dir_b, excluded)
    names_a = set(files_a)
    names_b = set(files_b)
    missing_from_a = sorted(names_b - names_a)
    missing_from_b = sorted(names_a - names_b)
    differing = sorted(
        name for name in names_a & names_b if files_a[name] != files_b[name]
    )
    passed = not missing_from_a and not missing_from_b and not differing
    print(
        f"shaders-identical files_a={len(files_a)} files_b={len(files_b)} "
        f"missing_a={missing_from_a} missing_b={missing_from_b} "
        f"differing={differing} status={'pass' if passed else 'fail'}"
    )
    return 0 if passed else 1


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)

    furnace = subcommands.add_parser("furnace", help="check a linear furnace mean")
    furnace.add_argument("png", type=Path)
    furnace.add_argument("--center", type=parse_pair, required=True)
    furnace.add_argument("--radius", type=float, required=True)
    furnace.add_argument("--expect", type=float, default=0.5)
    furnace.add_argument("--tol", type=float, default=0.02)
    furnace.set_defaults(run=run_furnace)

    sky = subcommands.add_parser("sky", help="check a linear sky mean")
    sky.add_argument("png", type=Path)
    sky.add_argument("--rect", type=parse_rect, required=True)
    sky.add_argument("--expect", type=float, default=1.0)
    sky.add_argument("--tol", type=float, default=0.01)
    sky.set_defaults(run=run_sky)

    noise = subcommands.add_parser("noise", help="compare spatial luminance noise")
    noise.add_argument("png_a", type=Path)
    noise.add_argument("png_b", type=Path)
    noise.add_argument("--rect", type=parse_rect, required=True)
    noise.add_argument("--min-ratio", type=float, default=4.0)
    noise.set_defaults(run=run_noise)

    noise_pair = subcommands.add_parser(
        "noise-pair", help="compare Monte Carlo noise from accumulated frame pairs"
    )
    noise_pair.add_argument("a_n", type=Path)
    noise_pair.add_argument("a_2n", type=Path)
    noise_pair.add_argument("b_n", type=Path)
    noise_pair.add_argument("b_2n", type=Path)
    noise_pair.add_argument("--rect", type=parse_rect, required=True)
    noise_pair.add_argument("--min-ratio", type=float, default=4.0)
    noise_pair.set_defaults(run=run_noise_pair)

    identical = subcommands.add_parser("identical", help="compare two files byte-for-byte")
    identical.add_argument("a", type=Path)
    identical.add_argument("b", type=Path)
    identical.set_defaults(run=run_identical)

    shaders = subcommands.add_parser(
        "shaders-identical", help="compare directory file names and bytes"
    )
    shaders.add_argument("dir_a", type=Path)
    shaders.add_argument("dir_b", type=Path)
    shaders.add_argument("--except", dest="excluded", action="append", default=[])
    shaders.set_defaults(run=run_shaders_identical)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = make_parser()
    arguments = parser.parse_args(argv)
    try:
        return int(arguments.run(arguments))
    except (CheckError, OSError, ValueError, struct.error, zlib.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
