// Aero Engine -- engine.input (task 4.4.3): the input binding (Vm::bindInput and unbindInput) and the keyboard and
// mouse functions of the frozen object `input`. script_api.hpp states the rules every host function keeps; the ones
// that shape this file are 3 (a name is checked by tag before it is read, nothing coerces), 4 (no property of a script
// value is read) and 9 (every argument check precedes the read of the borrowed state).
//
// The state is a platform::InputState the caller owns and updates; a function reads it at the moment it is called, so
// a hook, a constructor and module code all see the frame the caller has built. The native functions are made in
// script_api.cpp (the one place a native is made) and call the handler here, the worldFunction shape.
//
// A key or button name is matched by content against the constexpr tables of input_names.hpp after JS_IsString: no
// atom is made for any name, so the namespace's budget does not grow with the vocabulary. The text of a name is never
// echoed in an error, because a script controls its length and bytes.

#include <aero/core/log.hpp>
#include <aero/platform/input.hpp>
#include <aero/script/internal/input_names.hpp>
#include <aero/script/vm.hpp>

#include "script_api.hpp"
#include "vm_impl.hpp"

#include <array>
#include <cassert>
#include <optional>
#include <string>
#include <string_view>

namespace engine::script {
namespace {

namespace names = internal;

// "<site>: <reason>" as a TypeError (a RangeError when `range`), returning JS_EXCEPTION.
JSValue throwAtSite(JSContext* ctx, const Site& site, const std::string& reason, bool range = false) {
    const std::string text = describeSite(site) + ": " + reason;
    return range ? throwRangeError(ctx, text) : throwTypeError(ctx, text);
}

template <typename Enum>
using NameLookup = std::optional<Enum> (*)(std::string_view) noexcept;

template <typename Enum>
using EnumRead = bool (platform::InputState::*)(Enum) const noexcept;

using PointerRead = platform::MousePosition (platform::InputState::*)() const noexcept;

// The name argument: a string the table of `lookup` holds. False leaves an exception pending, and nothing has been read
// from the input.
template <typename Enum>
[[nodiscard]] bool readName(JSContext* ctx, const ScriptApi& api, const Site& site, JSValueConst value,
                            NameLookup<Enum> lookup, std::string_view what, Enum& out) {
    if (!JS_IsString(value)) {
        static_cast<void>(throwAtSite(ctx, site, "expected a string, got " + describeValue(ctx, value, api)));
        return false;
    }
    const HeldCString text{ctx, value};
    if (!text.ok()) {
        return false;  // a refused allocation: its exception is pending
    }
    const std::optional<Enum> found = lookup(text.view());
    if (!found.has_value()) {
        static_cast<void>(throwAtSite(ctx, site, "not " + std::string{what}, true));
        return false;
    }
    out = *found;
    return true;
}

JSValue noInput(JSContext* ctx, const Site& site) { return throwAtSite(ctx, site, "no input is bound"); }

template <typename Enum>
[[nodiscard]] JSValue enumRead(JSContext* ctx, const ScriptApi& api, const Site& site, JSValueConst value,
                               NameLookup<Enum> lookup, std::string_view what, EnumRead<Enum> read) {
    Enum which{};
    if (!readName(ctx, api, site, value, lookup, what, which)) {
        return JS_EXCEPTION;
    }
    if (api.input == nullptr) {
        return noInput(ctx, site);
    }
    return JS_NewBool(ctx, (api.input->*read)(which));
}

[[nodiscard]] JSValue pointerRead(JSContext* ctx, const ScriptApi& api, const Site& site, PointerRead read) {
    if (api.input == nullptr) {
        return noInput(ctx, site);
    }
    const platform::MousePosition value = (api.input->*read)();
    return newDetachedVector(ctx, api, 3, {value.x, value.y, 0.0F, 0.0F});
}

}  // namespace

JSValue callInputFunction(JSContext* ctx, ScriptApi& api, InputOp op, JSValueConst* argv) {
    const Site site{"input", INPUT_FUNCTIONS[static_cast<std::size_t>(op)].name, {}};
    switch (op) {
        case InputOp::KeyDown:
            return enumRead<platform::Key>(ctx, api, site, argv[0], &names::keyFromName, "a key name",
                                           &platform::InputState::keyDown);
        case InputOp::KeyPressed:
            return enumRead<platform::Key>(ctx, api, site, argv[0], &names::keyFromName, "a key name",
                                           &platform::InputState::keyPressed);
        case InputOp::KeyReleased:
            return enumRead<platform::Key>(ctx, api, site, argv[0], &names::keyFromName, "a key name",
                                           &platform::InputState::keyReleased);
        case InputOp::MouseButtonDown:
            return enumRead<platform::MouseButton>(ctx, api, site, argv[0], &names::mouseButtonFromName,
                                                   "a mouse button name", &platform::InputState::mouseButtonDown);
        case InputOp::MouseButtonPressed:
            return enumRead<platform::MouseButton>(ctx, api, site, argv[0], &names::mouseButtonFromName,
                                                   "a mouse button name", &platform::InputState::mouseButtonPressed);
        case InputOp::MouseButtonReleased:
            return enumRead<platform::MouseButton>(ctx, api, site, argv[0], &names::mouseButtonFromName,
                                                   "a mouse button name", &platform::InputState::mouseButtonReleased);
        case InputOp::MousePosition:
            return pointerRead(ctx, api, site, &platform::InputState::mousePosition);
        case InputOp::MouseDelta:
            return pointerRead(ctx, api, site, &platform::InputState::mouseDelta);
        case InputOp::MouseWheel:
            return pointerRead(ctx, api, site, &platform::InputState::mouseWheel);
    }
    return throwAtSite(ctx, site, "unknown function");  // unreachable: the switch is exhaustive
}

// ---- the input binding ---------------------------------------------------------------------------------------------

BindStatus Vm::bindInput(const platform::InputState& input) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: bindInput on an invalid Vm (a failed create or a moved-from Vm)");
        return BindStatus::InvalidVm;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: bindInput called from a thread that is not the VM's; refused");
        return BindStatus::WrongThread;
    }
    assert(!impl->inCall);  // a host function never calls back into the VM, and nothing else runs on this thread
    impl->scriptApi.input = &input;
    return BindStatus::Ok;
}

void Vm::unbindInput() noexcept {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: unbindInput on an invalid Vm (a failed create or a moved-from Vm)");
        return;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: unbindInput called from a thread that is not the VM's; refused");
        return;
    }
    assert(!impl->inCall);
    impl->scriptApi.input = nullptr;
}

}  // namespace engine::script
