#pragma once

namespace Pelican::DevCli {

// AB-1: `pelican_cli audio analyze --asset <file> --out <file.ktx2>
//        [--features rms,peak,bands] [--bands N] [--hop-ms MS] [--window-ms MS]
//        [--attack-ms MS] [--release-ms MS] [--channels mix|left|right]`
// bakes the sound's analysis exactly as the engine's bake_audio_analysis does
// (bakeAudioAnalysis: the same decoders, table and manifest) and prints
// {out, manifest, layout} as JSON. A refusal prints its code and fields and
// returns non-zero.
int runAudioCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
