#pragma once
// Aero Engine -- the script layer's PRIVATE join between the public Vm and the backend (task 4.1.1). It
// includes <quickjs.h>, which is why it lives under src/ -- the side of the boundary that
// .github/scripts/check-script-boundary.sh permits. Nothing outside engine/script may include it.

#include <aero/script/internal/heap_accounting.hpp>
#include <aero/script/vm.hpp>

#include <quickjs.h>
#include <thread>

namespace engine::script {

struct Vm::Impl {
    explicit Impl(const VmConfig& vmConfig) noexcept;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;  // pinned: the backend holds this address
    Impl& operator=(Impl&&) = delete;
    ~Impl();  // static_cast<void>(shutdown()) unless already finished

    // ---- lifecycle (vm.cpp) ------------------------------------------------------------------------------
    [[nodiscard]] VmTeardownReport shutdown() noexcept;
    [[nodiscard]] bool onVmThread() const noexcept { return std::this_thread::get_id() == vmThread; }

    // ---- state: `heap` FIRST, so it is destroyed LAST ------------------------------------------------------
    internal::HeapAccounting heap;
    VmConfig config;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    std::thread::id vmThread;
    bool finished = false;
};

}  // namespace engine::script
