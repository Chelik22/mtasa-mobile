// This symbol identifies the build probe without initializing/loading the game.
// Candidate implementation TUs are linked with --no-gc-sections so unresolved
// backend dependencies cannot disappear merely because this entry is trivial.
extern "C" __attribute__((visibility("default"))) int MtaPositionCandidateBuildAbi()
{
    return 28;
}
