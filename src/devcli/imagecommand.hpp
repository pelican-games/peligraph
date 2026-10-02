#pragma once

namespace Pelican::DevCli {

// IA-1: `pelican_cli image ktx2 --out <file.ktx2> --format <VK name>
//        (--raw <file> --width W --height H | --hdr <file.hdr>)`
// writes a single-level, uncompressed KTX2 (writeKtx2) from raw texels
// (little endian, rows packed) or from a Radiance .hdr read as
// R32G32B32A32_SFLOAT. Nothing is scaled or converted.
int runImageCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
