"""Regenerate the tiny committed BC fixtures used by WP92.

The 16-byte blocks are deterministic opaque GPU payloads. They are not decoded
by the engine; the fixtures exercise KTX2 metadata, level transfer, and format
capability handling. See README.md for the Khronos-tool validation command.
"""

from pathlib import Path
import struct


IDENTIFIER = bytes.fromhex("AB4B5458203230BB0D0A1A0A")


def dfd(model: int, transfer: int) -> bytes:
    # KHR_DF basic descriptor: one 128-bit compressed-block sample.
    sample = struct.pack("<HBB4BII", 0, 127, 0, 0, 0, 0, 0, 0, 0xFFFFFFFF)
    descriptor = struct.pack(
        "<IHHBBBB4B8B",
        0,  # Khronos vendor + BASICFORMAT descriptor type
        2,
        40,
        model,
        1,  # BT.709 primaries
        transfer,
        0,
        3,
        3,
        0,
        0,  # 4x4x1 block
        16,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    ) + sample
    return struct.pack("<I", 4 + len(descriptor)) + descriptor


def write_fixture(path: Path, vk_format: int, model: int, transfer: int, block: bytes) -> None:
    descriptor = dfd(model, transfer)
    dfd_offset = 80 + 24
    data_offset = (dfd_offset + len(descriptor) + 15) // 16 * 16
    header = IDENTIFIER + struct.pack(
        "<9I4I2Q",
        vk_format,
        1,
        1,
        1,
        0,
        0,
        1,
        1,
        0,
        dfd_offset,
        len(descriptor),
        0,
        0,
        0,
        0,
    )
    level = struct.pack("<3Q", data_offset, len(block), len(block))
    padding = bytes(data_offset - dfd_offset - len(descriptor))
    path.write_bytes(header + level + descriptor + padding + block)


root = Path(__file__).parent
write_fixture(root / "bc7_srgb_1x1.ktx2", 146, 134, 2, bytes.fromhex("40C0FF00000000000000000000000000"))
write_fixture(root / "bc5_unorm_1x1.ktx2", 141, 132, 1, bytes.fromhex("FF00000000000000FF00000000000000"))
