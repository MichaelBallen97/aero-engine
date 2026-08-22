/* Aero Engine — miniaudio single-header IMPLEMENTATION unit (task 0.3.3; ADR-006).
 *
 * This is the ONLY translation unit that defines MINIAUDIO_IMPLEMENTATION. Every other TU includes
 * <miniaudio.h> for DECLARATIONS only (src/audio_device.cpp; task 3.7.1 adds the editor's decode TU,
 * editor/src/audio_decode.cpp).
 *
 * Deliberately a .c file, not .cpp: it is vendored third-party C, and the CI lint globs are
 *   clang-tidy : git ls-files '*.cpp'
 *   clang-format: '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' '*.hxx' '*.inl'
 * — neither lists '*.c', so this ~90k-line implementation is auto-excluded from linting code we do not
 * own, with no per-directory .clang-tidy override and no change to the CI commands. It still builds with
 * the project's sanitizers in Debug (directory-scope compile options reach C too).
 *
 * Five trims survive, and each drops code no consumer in this tree touches: MA_NO_ENCODING (nothing
 * here ever writes an audio file — the cooker's output is .aerowave, a first-party container),
 * MA_NO_GENERATION (no waveform or noise sources; the device stub writes its own silence), and
 * MA_NO_ENGINE / MA_NO_RESOURCE_MANAGER / MA_NO_NODE_GRAPH (the whole high-level API — per ADR-006
 * this engine owns its own audio graph and its own asset flow, and pulling in miniaudio's would be a
 * second one).
 *
 * The DECODING trim is gone as of task 3.7.1, which is the handoff this comment used to name: the
 * editor decodes wav, flac and mp3 through ma_decoder_* (ogg goes through stb_vorbis, which is
 * editor-side and has nothing to do with this file). This stays the ONE implementation unit in the
 * tree — miniaudio's implementation defines ma_context, ma_device, ma_data_converter and several
 * hundred other external-linkage symbols, so a second one in the same binary is a duplicate-symbol
 * link error. That is why editor/CMakeLists.txt adds the HEADER to aero_editor_core's compile line
 * and nothing else: aero_editor_core already links aero::platform PUBLIC, so this archive is already
 * on its link line.
 *
 * The accepted, stated cost: dr_wav, dr_flac and dr_mp3 now compile into aero_platform's archive, so
 * they reach the link line of every binary that links it — including the Phase 5 runtime. Nothing on
 * the engine side references ma_decoder_* at all, so --gc-sections / /OPT:REF are EXPECTED to strip
 * them and the released runtime's cost is EXPECTED to be zero bytes. That expectation is UNVERIFIED
 * and is not task 3.7.1's to verify. The escape hatch, recorded so it is never re-derived: miniaudio's
 * MA_API is user-definable, so a decode-only implementation unit compiled with `#define MA_API static`
 * has internal linkage and does not collide with this copy. Owner: the first task that measures the
 * exported runtime's binary size, which is Phase 5's packager.
 */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
