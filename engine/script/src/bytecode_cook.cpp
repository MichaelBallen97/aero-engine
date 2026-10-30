// Aero Engine -- the bytecode cook (task 4.3.2): one ES module's source in, the backend's bytecode out. The order of
// the steps below is the contract:
//   1. the source is capped before any VM exists;
//   2. VM A compiles the source under COOK_MODULE_NAME and writes it (pass 1), then under COOK_PROBE_NAME (pass 2);
//   3. the rename proof: pass 1's payload, renamed by the loader's own rule, must equal pass 2 byte for byte -- the
//      script itself holds neither name, or a load would rename the script's own key;
//   4. VM B, fresh, reads pass 1 back and writes it again: the two must be identical.
// Nothing is logged: a cook's VMs are quiet and its result carries every failure. Every backend value is held the
// moment it is received (HeldValue; the writer's buffer in WriteBuffer). This file holds the layer's one
// JS_WriteObject; the one JS_ReadObject is vm_modules.cpp's.

#include <aero/assets/cooked_mesh.hpp>  // getU32
#include <aero/assets/cooked_script.hpp>
#include <aero/script/bytecode.hpp>
#include <aero/script/internal/bytecode_cook.hpp>
#include <aero/script/internal/bytecode_format.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/vm.hpp>

#include "vm_impl.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <quickjs.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

namespace {

static_assert(QJS_VERSION_MAJOR < 256 && QJS_VERSION_MINOR < 256 && QJS_VERSION_PATCH < 256,
              "the backend release packs one byte per component");

// What the cook writes with: the backend's byte code form with each function's source text stripped (and nothing
// else: the debug tables stay, so a frame keeps its file, line and column, and import.meta keeps working).
constexpr int COOK_WRITE_FLAGS = JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE;
constexpr std::uint32_t COOK_STRIP_FLAGS = 1U;
static_assert(COOK_STRIP_FLAGS == assets::COOKED_SCRIPT_STRIP_SOURCE,
              "the cook's strip flags are the container's source-stripped bit");

// The writer's buffer, held from the moment JS_WriteObject returns it.
class WriteBuffer {
public:
    WriteBuffer(JSContext* context, JSValueConst value) noexcept
        : context(context), data(JS_WriteObject(context, &length, value, COOK_WRITE_FLAGS)) {}
    WriteBuffer(const WriteBuffer&) = delete;
    WriteBuffer& operator=(const WriteBuffer&) = delete;
    WriteBuffer(WriteBuffer&&) = delete;
    WriteBuffer& operator=(WriteBuffer&&) = delete;
    ~WriteBuffer() {
        if (data != nullptr) {
            js_free(context, data);
        }
    }

    [[nodiscard]] bool ok() const noexcept { return data != nullptr; }
    [[nodiscard]] std::vector<std::byte> copy() const {
        const auto* first = reinterpret_cast<const std::byte*>(data);
        return std::vector<std::byte>(first, first + length);
    }

private:
    JSContext* context;
    std::size_t length = 0;  // declared before `data`: the writer fills it while `data` initialises
    std::uint8_t* data;
};

[[nodiscard]] std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

// A name the author never wrote must not reach the author: both placeholders become the source's own name. Neither is
// a prefix of the other, so the order does not matter.
[[nodiscard]] ScriptError scrub(ScriptError error, std::string_view sourceName) {
    for (std::string* field : {&error.message, &error.stack}) {
        *field = replaceAll(std::move(*field), internal::COOK_MODULE_NAME, sourceName);
        *field = replaceAll(std::move(*field), internal::COOK_PROBE_NAME, sourceName);
    }
    return error;
}

[[nodiscard]] ScriptCookResult refusal(ScriptCookStatus status, std::string message) {
    ScriptCookResult result;
    result.status = status;
    result.error = ScriptError{"Error", std::move(message), {}};
    return result;
}

}  // namespace

namespace internal {

// The friend of Vm (vm.hpp): the one place outside the VM's own files that reaches a Vm's Impl.
struct BytecodeCook {
    struct Pass {
        ScriptCookStatus status = ScriptCookStatus::Ok;
        ScriptError error;
        std::vector<std::byte> bytes;
    };

    // A pass runs no script code and polls no interrupt, so a trapped failure is a load refusal, a heap refusal or a
    // compile error: Exception, Interrupted, InvalidVm and WrongThread are unreachable and read as CompileError.
    [[nodiscard]] static ScriptCookStatus statusOf(EvalStatus status) {
        switch (status) {
            case EvalStatus::LoadError:
                return ScriptCookStatus::LoadError;
            case EvalStatus::OutOfMemory:
                return ScriptCookStatus::OutOfMemory;
            case EvalStatus::Ok:
            case EvalStatus::CompileError:
            case EvalStatus::Exception:
            case EvalStatus::Interrupted:
            case EvalStatus::InvalidVm:
            case EvalStatus::WrongThread:
                return ScriptCookStatus::CompileError;
        }
        return ScriptCookStatus::CompileError;
    }

    [[nodiscard]] static Vm::Impl& implOf(Vm& vm) { return *vm.impl; }

    [[nodiscard]] static Pass trapped(Vm::Impl& impl, std::string_view sourceName) {
        EvalResult failure = impl.trap(Phase::Compile);
        Pass out;
        out.status = statusOf(failure.status);
        out.error = scrub(std::move(failure.error), sourceName);
        return out;
    }

    // Writes `value` and takes the payload, or reports why not: the writer's pending exception, or no buffer at all.
    [[nodiscard]] static Pass writeValue(Vm::Impl& impl, JSValueConst value, std::string_view sourceName) {
        const WriteBuffer buffer{impl.ctx, value};
        if (JS_HasException(impl.ctx)) {
            return trapped(impl, sourceName);
        }
        if (!buffer.ok()) {
            Pass out;
            out.status = ScriptCookStatus::OutOfMemory;
            out.error = ScriptError{"InternalError", "the writer returned no buffer", {}};
            return out;
        }
        Pass out;
        out.bytes = buffer.copy();
        impl.clearPendingException();
        return out;
    }

    // One compile (or read) and one write, inside the scopes of a call that decides its own load failure.
    [[nodiscard]] static Pass pass(Vm::Impl& impl, std::string_view sourceName, const CookOptions& options,
                                   const std::string* text, std::string_view name, std::span<const std::byte> payload) {
        const std::string fileName{sourceName};
        const Vm::Impl::CallScope scope{impl, fileName};
        LoadAttempt attempt{/*decides=*/true};
        attempt.refusesImports = options.refusesImports;
        attempt.growthRefusalsAtStart = impl.heap.growthRefusals();
        const Vm::Impl::AttemptScope attempting{impl, attempt};
        if (JS_HasException(impl.ctx)) {
            return trapped(impl, sourceName);
        }
        ++attempt.depth;
        attempt.compiling.emplace_back(fileName);
        const HeldValue module =
            text != nullptr ? impl.compileModuleText(*text, std::string{name}) : impl.readModuleBytes(payload);
        attempt.compiling.pop_back();
        --attempt.depth;
        if (module.isException() || JS_HasException(impl.ctx)) {
            return trapped(impl, sourceName);
        }
        return writeValue(impl, module.get(), sourceName);
    }

    [[nodiscard]] static VmCreateResult createVm(const VmConfig& config, const CookOptions& options) {
        return options.modules != nullptr ? Vm::create(config, *options.modules) : Vm::create(config);
    }

    [[nodiscard]] static ScriptCookResult cook(std::string_view source, std::string_view sourceName,
                                               const CookOptions& options) {
        if (source.size() > MAX_COOK_SOURCE_BYTES) {
            return refusal(ScriptCookStatus::TooLarge,
                           std::format("the source is {} bytes, over the {}-byte cook limit", source.size(),
                                       MAX_COOK_SOURCE_BYTES));
        }
        const VmConfig config{COOK_HEAP_LIMIT_BYTES, DEFAULT_STACK_LIMIT_BYTES, MAX_CALL_TIME_BUDGET};
        const std::string text{source};  // the backend reads text[text.size()]
        std::vector<std::byte> first;
        {
            VmCreateResult created = createVm(config, options);
            if (!created.vm.valid()) {
                return refusal(ScriptCookStatus::OutOfMemory, "the cook's VM could not be created");
            }
            Vm::Impl& impl = implOf(created.vm);
            impl.quiet = true;
            Pass one = pass(impl, sourceName, options, &text, COOK_MODULE_NAME, {});
            if (one.status != ScriptCookStatus::Ok) {
                return failure(one);
            }
            if (one.bytes.empty() || std::to_integer<std::uint8_t>(one.bytes[0]) != SCRIPT_BYTECODE_VERSION ||
                (one.bytes.size() >= 5 && assets::getU32(one.bytes, 1) == BYTECODE_CHECKSUM_BYPASS)) {
                return refusal(ScriptCookStatus::UnsupportedOutput,
                               std::format("cannot cook '{}': the backend wrote bytecode this engine does not "
                                           "support (it pins version {} with a checksum)",
                                           sourceName, static_cast<unsigned>(SCRIPT_BYTECODE_VERSION)));
            }
            const Pass two = pass(impl, sourceName, options, &text, COOK_PROBE_NAME, {});
            if (two.status != ScriptCookStatus::Ok) {
                return failure(two);
            }
            const RenamedBytecode renamed = renameBytecodeModule(one.bytes, COOK_MODULE_NAME, COOK_PROBE_NAME);
            if (renamed.status != BytecodeHeadStatus::Ok || renamed.bytes != two.bytes) {
                return refusal(ScriptCookStatus::NotRenameable,
                               std::format("cannot cook '{}': it holds a string the cook reserves for the "
                                           "module's own name, so a load could not rename the script safely",
                                           sourceName));
            }
            first = std::move(one.bytes);
            static_cast<void>(created.vm.shutdown());
        }
        VmCreateResult created = createVm(config, options);
        if (!created.vm.valid()) {
            return refusal(ScriptCookStatus::OutOfMemory, "the cook's VM could not be created");
        }
        Vm::Impl& impl = implOf(created.vm);
        impl.quiet = true;
        const Pass third = pass(impl, sourceName, options, nullptr, {}, first);
        if (third.status != ScriptCookStatus::Ok || third.bytes != first) {
            const ScriptCookStatus status = third.status == ScriptCookStatus::OutOfMemory
                                                ? ScriptCookStatus::OutOfMemory
                                                : ScriptCookStatus::ReadBackMismatch;
            ScriptCookResult result =
                refusal(status, std::format("cannot cook '{}': the bytecode did not read back and write again to the "
                                            "same bytes",
                                            sourceName));
            if (third.status != ScriptCookStatus::Ok) {
                result.error = third.error;
            }
            return result;
        }
        static_cast<void>(created.vm.shutdown());
        ScriptCookResult result;
        result.status = ScriptCookStatus::Ok;
        result.payload = std::move(first);
        result.backendRelease = scriptBackendRelease();
        result.bytecodeVersion = scriptBytecodeVersion();
        result.stripFlags = COOK_STRIP_FLAGS;
        return result;
    }

    [[nodiscard]] static ScriptCookResult failure(const Pass& pass) {
        ScriptCookResult result;
        result.status = pass.status;
        result.error = pass.error;
        return result;
    }
};

ScriptCookResult cookScriptBytecodeWith(std::string_view source, std::string_view sourceName,
                                        const CookOptions& options) {
    return BytecodeCook::cook(source, sourceName, options);
}

}  // namespace internal

ScriptCookResult cookScriptBytecode(std::string_view source, std::string_view sourceName) {
    return internal::cookScriptBytecodeWith(source, sourceName, internal::CookOptions{});
}

std::string_view scriptCookStatusLabel(ScriptCookStatus status) noexcept {
    switch (status) {
        case ScriptCookStatus::Ok:
            return "Ok";
        case ScriptCookStatus::TooLarge:
            return "TooLarge";
        case ScriptCookStatus::CompileError:
            return "CompileError";
        case ScriptCookStatus::LoadError:
            return "LoadError";
        case ScriptCookStatus::OutOfMemory:
            return "OutOfMemory";
        case ScriptCookStatus::NotRenameable:
            return "NotRenameable";
        case ScriptCookStatus::ReadBackMismatch:
            return "ReadBackMismatch";
        case ScriptCookStatus::UnsupportedOutput:
            return "UnsupportedOutput";
    }
    return "Unknown";
}

std::uint32_t scriptBackendRelease() noexcept {
    return (static_cast<std::uint32_t>(QJS_VERSION_MAJOR) << 16U) |
           (static_cast<std::uint32_t>(QJS_VERSION_MINOR) << 8U) | static_cast<std::uint32_t>(QJS_VERSION_PATCH);
}

std::uint32_t scriptBytecodeVersion() noexcept { return internal::SCRIPT_BYTECODE_VERSION; }

}  // namespace engine::script
