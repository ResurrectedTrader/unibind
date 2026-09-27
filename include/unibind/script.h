#pragma once
/// \file
/// Compiling and running source, and caching what compiling produced.
///
/// ---------------------------------------------------------------------------
/// Compiled-code caching, and why it is in the abstraction
/// ---------------------------------------------------------------------------
///
/// An embedder that compiles the same source every run pays the parse and the
/// codegen every run, and both engines have a facility for not doing that: V8
/// emits and consumes a `ScriptCompiler::CachedData` blob, SpiderMonkey
/// transcodes XDR bytecode. The semantics agree closely enough to promise -
/// bytes out of one compile, bytes into the next, rejected rather than trusted
/// when they do not match - so it belongs here rather than in whatever the
/// embedder would otherwise write per engine.
///
/// **An engine that cannot do it does not pretend to.** A backend with no code
/// cache defines none of `CompileScriptWithCache`, `ScriptUsedCodeCache` or
/// `ScriptCreateCodeCache`, and calling one is then a link error at the call
/// site - which is this library's existing answer for an operation an engine
/// does not have (see `docs/status.md`). That is why caching is three separate
/// entry points rather than an extra parameter on `Compile`: a parameter a
/// backend quietly ignored would be the silent answer, and there is no way to
/// ask a linker about half a function.
///
/// ---------------------------------------------------------------------------
/// A blob is keyed to the source it was made from
/// ---------------------------------------------------------------------------
///
/// **A blob must be accepted only for the source it was compiled from, and
/// neither engine does that for you.** Both stamp a blob with an engine-build
/// identity and refuse one from a different build; that is the check they *do*
/// perform, and it is easy to mistake for the whole of it. It is not. A
/// SpiderMonkey stencil encoded from `"a"` decodes cleanly when offered for
/// `"b"`, reports success, and runs `a`.
///
/// It is one safety property at three scales:
///
///   1. **these bytes came from this engine** - the engines check this;
///   2. **these bytes came from this source** - neither engine checks this;
///   3. **these bytes are a blob at all** - neither engine reliably checks
///      this, and a release-build V8 will accept a corrupted payload.
///
/// So every blob this API emits is the engine's bytes behind a short header of
/// its own - a magic number and a format version, a 64-bit hash of the source
/// text together with its origin and the engine's build identity, and the
/// payload's length and hash - and every blob handed back is checked against
/// that header **before the engine sees it**. One that does not match is never
/// offered: the source compiles normally and `UsedCodeCache()` says false.
///
/// **The engine half of that key is the engine's build identity, not its
/// name.** That distinction is the difference between a cache that works and a
/// cache that silently never hits. A key naming only the engine survives an
/// engine upgrade unchanged, while the engine's own check does not: every blob
/// an embedder kept is then offered, refused underneath, and recompiled at full
/// price, with `UsedCodeCache()` answering false forever and nothing to say
/// why. So the key asks the engine what it would refuse the blob over -
/// `ScriptCompiler::CachedDataVersionTag()` on V8, the process build id on
/// SpiderMonkey - through `detail::BackendBuildId()`. A stale blob is then
/// rejected *here*, cheaply, by the check that can also say so.
///
/// `Platform::BackendVersion()` is deliberately not that key. It is a string
/// for a human and its shape is the engine's; a version can move without the
/// build identity moving and, worse, the other way round.
///
/// The header answers all three scales, which is why it is one mechanism and
/// not three: garbage fails the magic, a moved-on source fails the key, and
/// truncation or a bit flip fails the length and the payload hash. The last of
/// those is the one that cannot be delegated - an engine may answer a repeat
/// compile out of its own in-isolate cache without ever looking at what you
/// handed it, and then it is in no position to tell you whether your blob was
/// any good.
///
/// It is done here, once, rather than in each backend, so that a third backend
/// inherits the requirement instead of rediscovering it - and rediscovering it
/// is expensive: the failure is a wrong answer that looks like a right one, and
/// the only test that sees it **checks what actually ran, not what the API
/// reported**. Encode from one source, offer the blob for another, and run the
/// result.
///
/// **This is not a security property** and must not be read as one. A caller
/// that lets someone else choose its cache file has already lost. It catches
/// the case the promise is about: source that moved on without its cache.
///
/// The engines' own validation still runs underneath and is still worth having.
/// The cost here is one pass over the source per cache-related compile, which is
/// nothing beside compiling it.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "unibind/context.h"
#include "unibind/detail/backend.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/types.h"
#include "unibind/value.h"

namespace ub {

namespace detail {

/// The header on every blob this API emits, written in the host's own byte
/// order - a blob is only ever read back by the process family that wrote it,
/// so there is nothing to agree with anyone about.
///
/// One field per scale of the check: `magic` and `format` say this is a blob at
/// all, `key` says it came from this source on this engine build, and `payloadLength`
/// and `payloadHash` say the engine's own bytes arrived intact. The last two are
/// what a truncated or bit-flipped file fails, and they have to be here rather
/// than left to the engine: V8 does not checksum a payload in a release build
/// unless asked, and even asked, it may answer a repeat compile out of its own
/// in-isolate cache without ever looking at what you handed it.
struct CodeCacheHeader {
    std::uint32_t magic = 0;
    std::uint32_t format = 0;
    std::uint64_t key = 0;
    std::uint64_t payloadLength = 0;
    std::uint64_t payloadHash = 0;
};

inline constexpr std::uint32_t CODE_CACHE_MAGIC = 0x43757363;  // "csuC"
inline constexpr std::uint32_t CODE_CACHE_FORMAT = 1;

/// FNV-1a over the source, its origin and the engine's build identity. Not a
/// security check - the engine's own validation is underneath - just enough to
/// make "this blob was compiled from something else" a thing that cannot be
/// missed.
///
/// The origin is in the key because it moves what every position in the
/// compiled code reports: a blob made at one line offset is the wrong blob for
/// another, even where the text is identical.
[[nodiscard]] inline std::uint64_t CodeCacheKey(std::string_view source, const ScriptOrigin& origin,
                                                std::string_view engineBuildId) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto eat = [&hash](std::string_view text) {
        for (const char byte : text) {
            hash ^= static_cast<std::uint8_t>(byte);
            hash *= 1099511628211ULL;
        }
        hash ^= static_cast<std::uint64_t>(text.size());
        hash *= 1099511628211ULL;
    };
    const auto eatNumber = [&hash](std::int64_t value) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= 1099511628211ULL;
    };
    eat(source);
    eat(origin.resourceName);
    eatNumber(origin.lineOffset);
    eatNumber(origin.columnOffset);
    eat(engineBuildId);
    return hash;
}

/// FNV-1a over the engine's own bytes.
[[nodiscard]] inline std::uint64_t CodeCachePayloadHash(std::span<const std::uint8_t> payload) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::uint8_t byte : payload) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// The payload of `blob` if every scale of the check agrees, else empty - in
/// which case the caller compiles the source the ordinary way and says so.
[[nodiscard]] inline std::span<const std::uint8_t> CodeCachePayload(std::span<const std::uint8_t> blob,
                                                                    std::uint64_t key) noexcept {
    if (blob.size() <= sizeof(CodeCacheHeader)) {
        return {};
    }
    CodeCacheHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != CODE_CACHE_MAGIC || header.format != CODE_CACHE_FORMAT || header.key != key) {
        return {};
    }
    const std::span<const std::uint8_t> payload = blob.subspan(sizeof(CodeCacheHeader));
    if (payload.size() != header.payloadLength || CodeCachePayloadHash(payload) != header.payloadHash) {
        return {};
    }
    return payload;
}

}  // namespace detail

/// Compiled source, kept so it can be run more than once.
///
/// Move-only and self-rooting: compiled code is a GC object on both engines,
/// so this holds a root of its own rather than living in a handle scope. V8
/// spells it `Local<Script>`; that would tie compiled code to the scope it was
/// compiled in, which is wrong for something meant to be cached.
///
/// It outlives a scope, and it **does not outlive its isolate**: let it go, or
/// `Reset` it, before the isolate does. See `unibind/isolate.h`.
class Script {
   public:
    /// Empty if the source did not compile; the syntax error is pending.
    /// Source is UTF-8 text, so bytes in it that are not UTF-8 are a syntax
    /// error too.
    ///
    /// `options` says how much to compile now: by default what both engines
    /// do by default, which is the top level and no function body until it is
    /// first called. `CompileOptions::EagerCompile` compiles every function up
    /// front - what to ask for when the point of compiling is the code cache
    /// that `CreateCodeCache` makes next, which covers what had been compiled
    /// when it was made. It costs the compile time a lazy compile defers, and
    /// the memory for functions that may never run.
    ///
    /// It is honoured even for source this isolate has already compiled
    /// lazily. V8 answers a repeat compile out of an in-isolate cache and would
    /// otherwise answer an eager request with the lazy result it kept; its
    /// backend keeps the two apart.
    [[nodiscard]] static std::optional<Script> Compile(const Context& context, std::string_view source,
                                                       const ScriptOrigin& origin = {},
                                                       CompileOptions options = CompileOptions::NoCompileOptions) {
        detail::ScriptRec* rec = detail::CompileScript(context, source, origin, options);
        if (rec == nullptr) {
            return std::nullopt;
        }
        return Script(rec, detail::CodeCacheKey(source, origin, detail::BackendBuildId()), false);
    }

    constexpr Script() noexcept = default;
    ~Script() { Reset(); }

    Script(const Script&) = delete;
    Script& operator=(const Script&) = delete;

    Script(Script&& other) noexcept
        : rec_(std::exchange(other.rec_, nullptr)),
          key_(std::exchange(other.key_, 0)),
          offered_(std::exchange(other.offered_, false)) {}
    Script& operator=(Script&& other) noexcept {
        if (this != &other) {
            Reset();
            rec_ = std::exchange(other.rec_, nullptr);
            key_ = std::exchange(other.key_, 0);
            offered_ = std::exchange(other.offered_, false);
        }
        return *this;
    }

    /// Compile, offering a cache blob this API produced earlier for the same
    /// source.
    ///
    /// **A blob is a hint and never a risk.** One that does not belong to this
    /// source, or to this engine build, or to this blob format, is rejected
    /// here and never reaches the engine; one that does is offered, and the
    /// engine may still reject it for a reason of its own - a different set of
    /// flags, say. Either way the source compiles normally and
    /// `UsedCodeCache()` says which happened. Keeping a blob on disk across an
    /// engine upgrade is therefore safe: the worst it costs is the compile you
    /// would have paid anyway, and the key is what notices, so the cost is one
    /// compile and not one compile per run forever.
    ///
    /// `options` is `Compile`'s, and applies to whatever this call compiles
    /// from source: everything when there is no blob or the blob is refused,
    /// nothing when it is used - the blob is then what was compiled, and a
    /// blob made from an eager compile is already eager. V8 will not consume a
    /// cache and compile eagerly in one call, so this is the only meaning the
    /// combination can have on both engines. What it guarantees is the case an
    /// embedder is relying on: asking for `EagerCompile` with a stale blob
    /// still gets an eager compile, and so a fresh blob worth keeping.
    ///
    /// Empty only if the source did not compile, exactly as `Compile`.
    [[nodiscard]] static std::optional<Script> CompileWithCache(
        const Context& context, std::string_view source, std::span<const std::uint8_t> codeCache,
        const ScriptOrigin& origin = {}, CompileOptions options = CompileOptions::NoCompileOptions) {
        const std::uint64_t key = detail::CodeCacheKey(source, origin, detail::BackendBuildId());
        const std::span<const std::uint8_t> payload = detail::CodeCachePayload(codeCache, key);
        detail::ScriptRec* rec = detail::CompileScriptWithCache(context, source, origin, payload, options);
        if (rec == nullptr) {
            return std::nullopt;
        }
        return Script(rec, key, !payload.empty());
    }

    /// Whether the blob you handed to `CompileWithCache` was used.
    ///
    /// False for a script compiled without one, false for a blob that did not
    /// belong to this source or this engine build, and false for one the engine
    /// itself refused - which is exactly the case an embedder wants to know
    /// about, because it is the case where re-emitting the blob is worth it.
    ///
    /// It answers about *your blob*, not about how hard the engine worked.
    /// An engine that already has this source compiled in this isolate may
    /// answer from its own compilation cache without parsing and without
    /// looking at the blob; that is not this question and does not make this
    /// true.
    [[nodiscard]] bool UsedCodeCache() const noexcept { return offered_ && detail::ScriptUsedCodeCache(rec_); }

    /// The compiled form of this script, to feed back to `CompileWithCache`
    /// next time.
    ///
    /// **Opaque bytes**, carrying the engine's own blob behind a small header
    /// of this API's (see the top of this file) that ties it to the source it
    /// was compiled from and to the engine build that compiled it. Store it,
    /// hand it back, and let `CompileWithCache` decide whether it still fits;
    /// you do not have to key the file by hand, and a blob handed to the wrong
    /// source, the wrong engine or a newer build of the right one is refused
    /// rather than believed.
    ///
    /// Empty if the engine had nothing to give - it may decline for a script it
    /// has not fully compiled yet, and it declines for an empty script.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> CreateCodeCache() const {
        auto payload = detail::ScriptCreateCodeCache(rec_);
        if (!payload) {
            return std::nullopt;
        }
        const detail::CodeCacheHeader header{.magic = detail::CODE_CACHE_MAGIC,
                                             .format = detail::CODE_CACHE_FORMAT,
                                             .key = key_,
                                             .payloadLength = payload->size(),
                                             .payloadHash = detail::CodeCachePayloadHash(*payload)};
        std::vector<std::uint8_t> blob(sizeof(header) + payload->size());
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), payload->data(), payload->size());
        return blob;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return rec_ == nullptr; }

    /// Runs in `context`, which need not be the one it was compiled in - and
    /// when it is not, **the code sees that realm's globals**, not the
    /// compiling realm's.
    ///
    /// That is the whole point of compiled source being an artefact rather
    /// than a handle (see above): compile once, run it in every sandbox. An
    /// engine that binds compiled code to a realm has to rebind it whenever
    /// the realm changes; the alternative - the code keeps looking at the
    /// realm it was compiled in, whatever you pass - makes the parameter a lie
    /// and makes a compiled script useless for the one job it is cached for.
    ///
    /// Empty if it threw; the exception is pending.
    [[nodiscard]] std::optional<Local<Value>> Run(const Context& context) const {
        return detail::WrapSlot<Value>(detail::RunScript(context, rec_));
    }

    void Reset() noexcept {
        if (rec_ != nullptr) {
            detail::ReleaseScript(rec_);
            rec_ = nullptr;
        }
        key_ = 0;
        offered_ = false;
    }

   private:
    constexpr Script(detail::ScriptRec* rec, std::uint64_t key, bool offered) noexcept
        : rec_(rec), key_(key), offered_(offered) {}

    detail::ScriptRec* rec_ = nullptr;
    /// What a blob for this source would have to say to be believed. Kept so
    /// that `CreateCodeCache` can stamp it without being handed the source
    /// again.
    std::uint64_t key_ = 0;
    /// Whether a blob actually reached the engine, so that `UsedCodeCache` can
    /// answer about the blob rather than about the engine's own caches.
    bool offered_ = false;
};

/// Compile and run in one step, for source that is used once.
[[nodiscard]] inline std::optional<Local<Value>> Evaluate(const Context& context, std::string_view source,
                                                          const ScriptOrigin& origin = {}) {
    auto script = Script::Compile(context, source, origin);
    if (!script) {
        return std::nullopt;
    }
    return script->Run(context);
}

}  // namespace ub
