// tests/vulkan_stack_pin.cpp — keep Mesa's lavapipe ICD mapped for LSan (task 0.5.2).
//
// Linux/LSan only. The first texture-sampling fragment shader the cube test compiles+runs spins up a
// fixed lavapipe worker-thread pool (one per core = 4 on the CI runner); each worker leaks a ~56-byte
// per-thread struct at exit (224 B / 4 allocations). It is neither the rasterizer pool
// (LP_NUM_THREADS=0 left the count unchanged, run 29679720635) nor the disk-cache writer pool
// (MESA_SHADER_CACHE_DISABLE likewise, run 29680407602) — a different, non-disableable pool. SDL's GPU
// Vulkan backend loads/UNLOADS the Vulkan loader (libvulkan.so.1) around EACH Device lifecycle
// (SDL_gpu_vulkan.c), and on this Ubuntu loader unloading it also dlclose's the ICD it opened, so
// libvulkan_lvp.so (and its libLLVM dependency) are UNMAPPED by teardown and read <unknown module> at
// LSan-time — which no tests/lsan.supp entry can match.
//
// The fix: during static init (before main, hence before any test creates a Device) dlopen the ICD
// ITSELF with RTLD_NODELETE and never dlclose it. Pinning just the loader is not enough — run
// 29681929403 proved the loader stays mapped but this loader still unmaps the ICD. RTLD_NODELETE is a
// property of the loaded object's link_map ("Do not unload the shared object during dlclose()",
// dlopen(3)); glibc records it once and every later dlopen of the same file by SDL's loader returns
// the SAME mapping (dlopen(3): "the same object handle is returned", refcounted), so no dlclose from
// any Device cycle can ever remove it. All worker threads from all Device cycles then carry return
// addresses into one permanently-mapped copy of the ICD (collapsing the two-load-base / identical-
// offset reload signature seen in runs 29679001505 / 29679720635 / 29680407602 / 29681071731 /
// 29681929403), and libLLVM stays mapped as a NODELETE-held dependency of the pinned ICD. At exit the
// frames resolve to real module names and lsan.supp's leak:libvulkan_lvp / leak:libLLVM match.
//
// Done IN-PROCESS, not via LD_PRELOAD: preloading a non-ASan library aborts the ASan runtime ("runtime
// does not come first") and leaks Mesa's libLLVM into the separate shaderc test processes, clashing
// with SDL_shadercross's vendored DXC LLVM (both observed, run 29681071731). A null return is
// non-fatal — on a driverless machine the GPU-gated tiers skip anyway.
#if defined(__linux__)

    #include <cstdio>
    #include <dlfcn.h>

namespace {

// dlopen(soname) with RTLD_NODELETE so the mapping outlives every later dlclose; true on success.
bool pinModule(const char* soname) { return ::dlopen(soname, RTLD_NOW | RTLD_NODELETE) != nullptr; }

struct VulkanStackPin {
    VulkanStackPin() {
        // Try the bare soname first (/usr/lib/<triplet> is on the default ldconfig path), then the
        // absolute path observed in run 29681071731's LD_PRELOAD log (evidence, not a guess).
        if (!pinModule("libvulkan_lvp.so") && !pinModule("/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so")) {
            std::fprintf(stderr, "[tests] note: could not pin libvulkan_lvp.so (%s)\n", ::dlerror());
        }
    }
};

const VulkanStackPin pinInstance;

}  // namespace

#endif  // defined(__linux__)
