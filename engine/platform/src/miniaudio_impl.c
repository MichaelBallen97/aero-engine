/* Aero Engine — miniaudio single-header IMPLEMENTATION unit (task 0.3.3; ADR-006).
 *
 * This is the ONLY translation unit that defines MINIAUDIO_IMPLEMENTATION. Every other TU includes
 * <miniaudio.h> for DECLARATIONS only (currently just src/audio_device.cpp).
 *
 * Deliberately a .c file, not .cpp: it is vendored third-party C, and the CI lint globs are
 *   clang-tidy : git ls-files '*.cpp'
 *   clang-format: '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' '*.hxx' '*.inl'
 * — neither lists '*.c', so this ~90k-line implementation is auto-excluded from linting code we do not
 * own, with no per-directory .clang-tidy override and no change to the CI commands. It still builds with
 * the project's sanitizers in Debug (directory-scope compile options reach C too).
 *
 * Device-only trims: the stub needs only ma_context/ma_device. These drop the decoders/encoders/
 * generators and the high-level ma_engine/resource-manager/node-graph. Task 3.7.1 (audio clips) will
 * remove MA_NO_DECODING (and add stb_vorbis for ogg); see the spec's §7 handoff.
 */
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
