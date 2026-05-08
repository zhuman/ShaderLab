#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "ShaderCompiler.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// BytecodeCache (Phase 8 GPU-binding work)
// =========================================
//
// Compile-once, reuse-everywhere bytecode store.
//
// Motivation
// ----------
// Phase 8 introduces gpuBindable parameters that switch a shader between
// "cbuffer" and "GPU-bound" mode at compile time via the
// `_SLPARAM_<name>_GPU=0|1` macros injected by the host. With N gpuBindable
// params per effect we want N+1 variants ready to swap in at zero render-
// thread cost: a baseline (all params in cbuffer mode) and one variant per
// param flipped to GPU mode (Phase 8 supports at most one binding active
// per consumer at a time, so we don't need 2^N).
//
// The user's stated goal: "when a user first inserts an effect into the
// graph, why don't we just compile/cache all of the options then and
// there?". The cache provides the substrate for that: enqueue compiles
// off the render thread, hand out finished bytecode synchronously.
//
// Identity model
// --------------
// The compile-key contains everything that influences the resulting
// bytecode -- canonical source hash, entry point, target, ordered
// gpu-param signature hash, embedded-include-library hash, macro bitset.
// Effect ID / version are stored as *metadata only* so the disk-cache
// layer (`p8-cache-disk`) has stable layout names; identical source
// across two renamed effects shares one cache entry.
//
// Diagnostics survive
// -------------------
// Lookups return a `BytecodeCacheResult` that carries a status + bytecode
// + error message. Existing call sites that previously consumed
// `ShaderCompileResult::ErrorMessage()` keep working: they just adapt the
// field name. Failed entries cache their diagnostics so a deterministic
// compile error doesn't recompile every frame.
//
// Threading
// ---------
// 2 worker `std::jthread`s drain a FIFO queue. The render thread can call
// `GetOrCompile` which:
//   1. returns immediately if the entry is `Ready`;
//   2. waits up to `timeoutMs` for an in-flight worker to finish;
//   3. falls back to inline compile on the calling thread.
// On race-completion the worker discards its result if the inline compile
// already won (`Pending -> Ready` only; never `Ready -> Failed`).
//
// Bounded memory
// --------------
// Total `Ready` + `Failed` bytecode bytes are capped (default 256 MB).
// `Pending` entries are never evicted. Eviction is naive LRU on insertion
// order -- good enough until disk persistence lands.

namespace ShaderLab::Effects
{
    // Compile identity. Two requests with equal keys must produce
    // byte-identical bytecode (assuming same engine version).
    struct BytecodeCompileKey
    {
        uint64_t    sourceHash;          // FNV1a-64 of canonical UTF-8 HLSL.
        uint64_t    paramSignatureHash;  // FNV1a-64 of "name0\0name1\0..." for ordered gpuBindable param names.
        uint64_t    includeLibraryHash;  // Hash of embedded-include library + cache schema version.
        uint32_t    macroBitset;         // bit i = parameter i has _SLPARAM_<name>_GPU=1.
        std::string entryPoint;          // e.g. "main".
        std::string target;              // e.g. "ps_5_0", "cs_5_0".

        bool operator==(const BytecodeCompileKey& o) const noexcept;
    };

    struct BytecodeCompileKeyHash
    {
        size_t operator()(const BytecodeCompileKey& k) const noexcept;
    };

    // Effect-level metadata stored alongside each entry. Used only by
    // disk-cache layout and diagnostics; never participates in equality.
    struct BytecodeCacheMetadata
    {
        std::wstring effectId;
        uint32_t     version{ 0 };
    };

    enum class BytecodeStatus : uint8_t
    {
        NotRequested = 0,
        Pending      = 1,
        Ready        = 2,
        Failed       = 3,
    };

    // Result handed back to callers. On success `status == Ready` and
    // `bytecode` is non-empty; on failure `status == Failed` and
    // `errorMessage` carries the D3DCompile diagnostic string.
    struct BytecodeCacheResult
    {
        BytecodeStatus       status{ BytecodeStatus::NotRequested };
        std::vector<uint8_t> bytecode;
        std::wstring         errorMessage;
        bool                 fromCache{ false };  // false if compiled inline.
    };

    // Compile request. Carries the canonical source + macros + metadata
    // so the worker can drive D3DCompile without callbacks back into
    // the data model.
    struct BytecodeCompileRequest
    {
        BytecodeCompileKey      key;
        BytecodeCacheMetadata   metadata;
        std::string             hlslSource;            // canonical UTF-8 (LF line endings).
        std::vector<std::string> gpuBindableParamNames; // index i -> bit i in macroBitset.
    };

    // Helpers (also exposed so call sites can hash without going through
    // the cache, e.g. to compute a key for `TryGet`).
    SHADERLAB_API uint64_t FNV1a64(const void* data, size_t size);
    SHADERLAB_API uint64_t HashCanonicalSource(std::string_view canonical);
    SHADERLAB_API uint64_t HashParamSignature(const std::vector<std::string>& orderedNames);
    SHADERLAB_API uint64_t IncludeLibraryHash();  // covers shaderlab_params.hlsli + cache schema.

    // Centralized source canonicalization. Every call site that hashes
    // source MUST go through this so a render-time hash matches the
    // precompile-time hash even if the call site stored the source as
    // wstring vs string with different line-endings.
    SHADERLAB_API std::string CanonicalizeHlslSource(std::wstring_view source);
    SHADERLAB_API std::string CanonicalizeHlslSource(std::string_view source);

    // The cache itself. Single global instance.
    class SHADERLAB_API BytecodeCache
    {
    public:
        static BytecodeCache& Instance();

        BytecodeCache(const BytecodeCache&) = delete;
        BytecodeCache& operator=(const BytecodeCache&) = delete;

        // Non-blocking lookup. Returns a copy of cached state. `status`
        // can be NotRequested / Pending / Ready / Failed. If the entry
        // is not in memory but exists on disk, this method silently
        // hydrates the in-memory map with the disk bytecode and returns
        // a Ready result (this is why the method is non-const).
        BytecodeCacheResult TryGet(const BytecodeCompileKey& key);

        BytecodeStatus GetStatus(const BytecodeCompileKey& key) const;

        // Configure the on-disk cache root. Call once during engine
        // init (via EngineExports). If empty (default), persistence is
        // disabled and the cache is purely in-memory. Path is created
        // recursively if missing.
        void SetDiskCacheRoot(std::wstring rootPath);
        const std::wstring& GetDiskCacheRoot() const { return m_diskRoot; }

        // Idempotent enqueue. If status is already Pending or Ready,
        // no-op. If status is Failed, no-op (caller must explicitly
        // Invalidate to retry a deterministic failure). Worker thread
        // picks it up.
        void RequestCompile(BytecodeCompileRequest request);

        // Synchronous "fetch or compile". Renders should call this:
        //   1. If Ready -> return immediately, fromCache=true.
        //   2. If Pending -> wait up to timeoutMs for worker; if still
        //      pending after timeout, fall back to inline compile on the
        //      calling thread. fromCache=false on inline.
        //   3. If miss -> compile inline, insert as Ready, return.
        //   4. If Failed cached -> return cached failure (don't re-compile).
        //
        // Race-safe: a late worker result will not overwrite a Ready
        // entry produced by an inline compile.
        BytecodeCacheResult GetOrCompile(BytecodeCompileRequest request, uint32_t timeoutMs = 50);

        // Eager precompile of N+1 variants (baseline + one per gpu-bindable
        // param). Call from UI/MCP insertion paths. Idempotent. Variants
        // already Pending or Ready are skipped.
        void PrecompileCommonShapes(
            const BytecodeCacheMetadata& metadata,
            const std::string& canonicalHlslSource,
            const std::string& entryPoint,
            const std::string& target,
            const std::vector<std::string>& gpuBindableParamNames);

        // Mark a key as needing recompile. Used by the editor when the
        // user fixes a previously-failed shader and Ctrl+Enters. Removes
        // the entry; next request recompiles.
        void Invalidate(const BytecodeCompileKey& key);

        // Reaper hooks (Phase 8 cache disk + reaper work).
        void Clear();

        // Reap stale on-disk entries. Files not modified within
        // `staleThresholdSec` are deleted. Returns counts.
        struct ReapResult { size_t filesDeleted; size_t bytesFreed; size_t errors; };
        ReapResult ReapDisk(uint64_t staleThresholdSec);

        // Delete everything under the disk cache root. In-memory
        // entries are preserved (next render still works without
        // re-compile), but any future restart will recompile.
        ReapResult ClearDisk();

        struct Stats
        {
            size_t entries;
            size_t totalBytecodeBytes;
            size_t pendingCount;
            size_t failedCount;
            size_t inlineCompiles;   // total inline-fallback compiles since startup.
            size_t workerCompiles;   // total worker compiles since startup.
            size_t cacheHits;
            size_t cacheMisses;
        };
        Stats GetStats() const;

        // Explicit shutdown. Waits for worker threads to drain (with
        // bounded timeout) and stops accepting new work. Safe to call
        // from a destructor as a fallback, but the engine should call
        // it explicitly on close.
        void Shutdown();

    private:
        BytecodeCache();
        ~BytecodeCache();

        struct Entry
        {
            BytecodeStatus       status{ BytecodeStatus::Pending };
            std::vector<uint8_t> bytecode;
            std::wstring         errorMessage;
            uint64_t             insertionOrder{ 0 };  // for naive LRU eviction.
        };

        // Compile request payload kept on the queue. The key is a copy
        // so we don't need to stabilize the entry pointer across the
        // workers and the entry map.
        struct PendingWork
        {
            BytecodeCompileRequest request;
        };

        // Run a D3DCompile in-place and mutate the entry. Caller holds
        // m_mutex on entry; this method releases it during the compile
        // and re-acquires before mutating the map. Returns the final
        // status (Ready or Failed).
        BytecodeStatus DoCompile(
            const BytecodeCompileRequest& req,
            std::unique_lock<std::mutex>& lock,
            Entry& outEntry);

        void WorkerLoop(std::stop_token stop);

        // Evict oldest Ready/Failed entries until total bytecode bytes
        // fits under m_maxBytes. Caller holds m_mutex. Pending entries
        // are never evicted.
        void EnforceByteLimit_NoLock();

        mutable std::mutex                                                              m_mutex;
        std::condition_variable                                                          m_workCv;
        std::condition_variable                                                          m_readyCv;
        std::unordered_map<BytecodeCompileKey, Entry, BytecodeCompileKeyHash>           m_entries;
        std::deque<PendingWork>                                                          m_workQueue;
        std::vector<std::jthread>                                                        m_workers;
        std::atomic<bool>                                                                m_shutdown{ false };
        uint64_t                                                                         m_nextInsertionOrder{ 1 };
        size_t                                                                           m_currentBytes{ 0 };

        // Tunables.
        size_t       m_maxBytes{ 256ull * 1024 * 1024 };  // 256 MB.
        std::wstring m_diskRoot;                          // empty = disabled.

        // Disk helpers.
        std::wstring KeyToDiskPath(
            const BytecodeCompileKey& key,
            const BytecodeCacheMetadata& meta) const;
        bool TryLoadFromDisk(
            const BytecodeCompileKey& key,
            std::vector<uint8_t>& outBytecode) const;
        void WriteToDisk(
            const BytecodeCompileKey& key,
            const BytecodeCacheMetadata& meta,
            const std::vector<uint8_t>& bytecode);

        // Stats (atomic so GetStats is lock-free for the counters).
        mutable std::atomic<size_t> m_inlineCompiles{ 0 };
        mutable std::atomic<size_t> m_workerCompiles{ 0 };
        mutable std::atomic<size_t> m_cacheHits{ 0 };
        mutable std::atomic<size_t> m_cacheMisses{ 0 };
    };
}
