#include "pch_engine.h"
#include "BytecodeCache.h"
#include "ShaderLabParamsHlsl.h"

#include <algorithm>
#include <chrono>

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Free helpers (canonicalization + hashing)
    // -----------------------------------------------------------------------

    uint64_t FNV1a64(const void* data, size_t size)
    {
        // Standard 64-bit FNV-1a.
        constexpr uint64_t kOffset = 0xcbf29ce484222325ull;
        constexpr uint64_t kPrime  = 0x100000001b3ull;
        const auto* p = static_cast<const uint8_t*>(data);
        uint64_t h = kOffset;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= p[i];
            h *= kPrime;
        }
        return h;
    }

    uint64_t HashCanonicalSource(std::string_view canonical)
    {
        return FNV1a64(canonical.data(), canonical.size());
    }

    uint64_t HashParamSignature(const std::vector<std::string>& orderedNames)
    {
        // Pack as "name0\0name1\0..." so order matters and "Foo|FooBar"
        // doesn't collide with "FooFooBar".
        std::string buf;
        buf.reserve(64);
        for (const auto& n : orderedNames)
        {
            buf.append(n);
            buf.push_back('\0');
        }
        return FNV1a64(buf.data(), buf.size());
    }

    uint64_t IncludeLibraryHash()
    {
        // Cache schema version: bump when the cache layout, key fields,
        // or any embedded include changes in a way that should invalidate
        // existing entries (in-memory and on disk).
        // v3: ShaderCompiler::CompileFromString now always optimizes
        //     (D3DCOMPILE_OPTIMIZATION_LEVEL3) even in debug builds.
        //     Old v2 cached entries used SKIP_OPTIMIZATION in debug
        //     -- those bytecodes are 5-10x slower at runtime. Bump
        //     so they fall out and re-compile at the new opt level.
        constexpr uint32_t kCacheSchemaVersion = 4;
        const char* libPtr = GetShaderLabParamsHLSL();
        size_t libLen = libPtr ? GetShaderLabParamsHLSLLength() : 0;
        std::string buf;
        buf.reserve(libLen + 8);
        buf.append(reinterpret_cast<const char*>(&kCacheSchemaVersion), sizeof(kCacheSchemaVersion));
        if (libPtr) buf.append(libPtr, libLen);
        return FNV1a64(buf.data(), buf.size());
    }

    static std::string CanonicalizeImpl(const char* src, size_t len)
    {
        // Normalize CRLF / CR-only line endings to LF. Don't strip
        // trailing whitespace or comments -- D3DCompile is sensitive
        // to source content via #line directives and cumulative output.
        std::string out;
        out.reserve(len);
        for (size_t i = 0; i < len; ++i)
        {
            char c = src[i];
            if (c == '\r')
            {
                out.push_back('\n');
                if (i + 1 < len && src[i + 1] == '\n') ++i;
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string CanonicalizeHlslSource(std::string_view source)
    {
        return CanonicalizeImpl(source.data(), source.size());
    }

    std::string CanonicalizeHlslSource(std::wstring_view source)
    {
        // wide -> UTF-8 first. winrt::to_string requires hstring_view;
        // do a minimal manual conversion since these are ASCII HLSL
        // sources in practice. Anything outside ASCII is preserved as
        // a raw byte sequence using WideCharToMultiByte for safety.
        std::string utf8;
        if (!source.empty())
        {
            int needed = ::WideCharToMultiByte(
                CP_UTF8, 0,
                source.data(), static_cast<int>(source.size()),
                nullptr, 0, nullptr, nullptr);
            if (needed > 0)
            {
                utf8.resize(static_cast<size_t>(needed));
                ::WideCharToMultiByte(
                    CP_UTF8, 0,
                    source.data(), static_cast<int>(source.size()),
                    utf8.data(), needed, nullptr, nullptr);
            }
        }
        return CanonicalizeImpl(utf8.data(), utf8.size());
    }

    // -----------------------------------------------------------------------
    // BytecodeCompileKey
    // -----------------------------------------------------------------------

    bool BytecodeCompileKey::operator==(const BytecodeCompileKey& o) const noexcept
    {
        return sourceHash         == o.sourceHash
            && paramSignatureHash == o.paramSignatureHash
            && includeLibraryHash == o.includeLibraryHash
            && macroBitset        == o.macroBitset
            && entryPoint         == o.entryPoint
            && target             == o.target;
    }

    size_t BytecodeCompileKeyHash::operator()(const BytecodeCompileKey& k) const noexcept
    {
        // Fold the four 64-bit hashes + macroBitset + entry/target into
        // a 64-bit then return as size_t. Cheap mixing; collisions
        // resolved by full equality.
        uint64_t h = k.sourceHash;
        auto mix = [&h](uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(k.paramSignatureHash);
        mix(k.includeLibraryHash);
        mix(static_cast<uint64_t>(k.macroBitset));
        mix(FNV1a64(k.entryPoint.data(), k.entryPoint.size()));
        mix(FNV1a64(k.target.data(),     k.target.size()));
        return static_cast<size_t>(h);
    }

    // Thread-local sentinel: set while a worker is processing a job.
    // Used by GetOrCompile to detect re-entrancy and skip waiting.
    // File-scope (not a class static) because TLS can't have dllexport.
    static thread_local bool t_isWorkerThread = false;

    // -----------------------------------------------------------------------
    // Lifetime
    // -----------------------------------------------------------------------

    BytecodeCache& BytecodeCache::Instance()
    {
        // Function-local static -- thread-safe initialization since C++11.
        static BytecodeCache instance;
        return instance;
    }

    BytecodeCache::BytecodeCache()
    {
        // Spin up worker threads. We deliberately keep this small (2):
        // typical compile is short, and we'd rather not flood D3DCompile.
        constexpr size_t kWorkerCount = 2;
        m_workers.reserve(kWorkerCount);
        for (size_t i = 0; i < kWorkerCount; ++i)
        {
            m_workers.emplace_back([this](std::stop_token stop) {
                t_isWorkerThread = true;
                WorkerLoop(stop);
            });
        }
    }

    BytecodeCache::~BytecodeCache()
    {
        Shutdown();
    }

    void BytecodeCache::Shutdown()
    {
        // Idempotent.
        if (m_shutdown.exchange(true)) return;

        {
            std::lock_guard<std::mutex> g(m_mutex);
            m_workQueue.clear();  // discard queued-but-not-started work.
        }
        m_workCv.notify_all();
        m_readyCv.notify_all();
        for (auto& w : m_workers)
        {
            w.request_stop();
        }
        // jthread destructors join on scope exit (or on the next assignment).
        m_workers.clear();
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    BytecodeCacheResult BytecodeCache::TryGet(const BytecodeCompileKey& key)
    {
        BytecodeCacheResult r;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto it = m_entries.find(key);
            if (it != m_entries.end())
            {
                r.status       = it->second.status;
                r.bytecode     = it->second.bytecode;
                r.errorMessage = it->second.errorMessage;
                r.fromCache    = (r.status == BytecodeStatus::Ready);
                return r;
            }
        }
        // Disk fallback: hydrate the in-memory map if the file exists.
        std::vector<uint8_t> fromDisk;
        if (TryLoadFromDisk(key, fromDisk))
        {
            std::lock_guard<std::mutex> g(m_mutex);
            // Re-check after acquiring the lock (another thread may have
            // populated the entry while we read the file).
            auto it = m_entries.find(key);
            if (it == m_entries.end())
            {
                Entry& e = m_entries[key];
                e.status         = BytecodeStatus::Ready;
                e.bytecode       = fromDisk;
                e.insertionOrder = m_nextInsertionOrder++;
                m_currentBytes += e.bytecode.size();
            }
            r.status    = BytecodeStatus::Ready;
            r.bytecode  = std::move(fromDisk);
            r.fromCache = true;
            return r;
        }
        r.status = BytecodeStatus::NotRequested;
        return r;
    }

    BytecodeStatus BytecodeCache::GetStatus(const BytecodeCompileKey& key) const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        auto it = m_entries.find(key);
        return (it == m_entries.end()) ? BytecodeStatus::NotRequested : it->second.status;
    }

    // -----------------------------------------------------------------------
    // Enqueue
    // -----------------------------------------------------------------------

    void BytecodeCache::RequestCompile(BytecodeCompileRequest request)
    {
        if (m_shutdown.load()) return;

        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto it = m_entries.find(request.key);
            if (it != m_entries.end())
            {
                // Already known -- no-op for Pending/Ready/Failed. Failure
                // retry requires explicit Invalidate.
                return;
            }
            // Insert a Pending placeholder so a subsequent identical
            // request from another thread short-circuits cleanly.
            Entry& e = m_entries[request.key];
            e.status         = BytecodeStatus::Pending;
            e.insertionOrder = m_nextInsertionOrder++;
            m_workQueue.push_back({ std::move(request) });
        }
        m_workCv.notify_one();
    }

    // -----------------------------------------------------------------------
    // Synchronous fetch-or-compile
    // -----------------------------------------------------------------------

    BytecodeCacheResult BytecodeCache::GetOrCompile(
        BytecodeCompileRequest request, uint32_t timeoutMs)
    {
        // Fast path: existing Ready/Failed entry in memory.
        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto it = m_entries.find(request.key);
            if (it != m_entries.end())
            {
                if (it->second.status == BytecodeStatus::Ready)
                {
                    BytecodeCacheResult r;
                    r.status       = BytecodeStatus::Ready;
                    r.bytecode     = it->second.bytecode;
                    r.fromCache    = true;
                    m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                if (it->second.status == BytecodeStatus::Failed)
                {
                    BytecodeCacheResult r;
                    r.status       = BytecodeStatus::Failed;
                    r.errorMessage = it->second.errorMessage;
                    r.fromCache    = true;
                    m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                // Pending: fall through to wait/inline-compile path.
            }
        }

        // Disk fallback BEFORE inline compile -- a previous-session
        // bytecode is much cheaper than re-running D3DCompile.
        {
            std::vector<uint8_t> fromDisk;
            if (TryLoadFromDisk(request.key, fromDisk))
            {
                std::lock_guard<std::mutex> g(m_mutex);
                auto it = m_entries.find(request.key);
                if (it == m_entries.end() ||
                    it->second.status == BytecodeStatus::Pending)
                {
                    Entry& e = m_entries[request.key];
                    if (e.insertionOrder == 0)
                        e.insertionOrder = m_nextInsertionOrder++;
                    if (e.bytecode.size() != fromDisk.size())
                        m_currentBytes += fromDisk.size() - e.bytecode.size();
                    e.bytecode = std::move(fromDisk);
                    e.status   = BytecodeStatus::Ready;
                    e.errorMessage.clear();
                }
                BytecodeCacheResult r;
                r.status    = BytecodeStatus::Ready;
                r.bytecode  = m_entries[request.key].bytecode;
                r.fromCache = true;
                m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                m_readyCv.notify_all();
                return r;
            }
        }

        m_cacheMisses.fetch_add(1, std::memory_order_relaxed);

        // Wait briefly for an in-flight worker compile to finish, but
        // not if we ARE a worker (would deadlock if a compile callback
        // re-entered the cache).
        if (timeoutMs > 0 && !t_isWorkerThread)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
            while (true)
            {
                auto it = m_entries.find(request.key);
                if (it != m_entries.end() &&
                    (it->second.status == BytecodeStatus::Ready ||
                     it->second.status == BytecodeStatus::Failed))
                {
                    BytecodeCacheResult r;
                    r.status       = it->second.status;
                    r.bytecode     = it->second.bytecode;
                    r.errorMessage = it->second.errorMessage;
                    r.fromCache    = true;
                    return r;
                }
                if (m_readyCv.wait_until(lock, deadline) == std::cv_status::timeout)
                    break;
            }
        }

        // Inline compile fallback. Place a Pending placeholder if not
        // already present, then compile on this thread.
        Entry localEntry;
        BytecodeStatus finalStatus;
        BytecodeCacheMetadata metaCopy;
        BytecodeCompileKey    keyCopy;
        std::vector<uint8_t>  bytecodeForDisk;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto& e = m_entries[request.key];
            if (e.status == BytecodeStatus::Ready ||
                e.status == BytecodeStatus::Failed)
            {
                BytecodeCacheResult r;
                r.status       = e.status;
                r.bytecode     = e.bytecode;
                r.errorMessage = e.errorMessage;
                r.fromCache    = true;
                return r;
            }
            if (e.insertionOrder == 0)
                e.insertionOrder = m_nextInsertionOrder++;
            finalStatus      = DoCompile(request, lock, e);
            localEntry       = e;
            keyCopy          = request.key;
            metaCopy         = request.metadata;
            if (finalStatus == BytecodeStatus::Ready)
                bytecodeForDisk = e.bytecode;
            EnforceByteLimit_NoLock();
        }
        m_inlineCompiles.fetch_add(1, std::memory_order_relaxed);
        m_readyCv.notify_all();

        // Persist outside the lock (best-effort; don't block render).
        if (!bytecodeForDisk.empty() && !m_diskRoot.empty())
        {
            std::lock_guard<std::mutex> g(m_mutex);
            WriteToDisk(keyCopy, metaCopy, bytecodeForDisk);
        }

        BytecodeCacheResult r;
        r.status       = finalStatus;
        r.bytecode     = std::move(localEntry.bytecode);
        r.errorMessage = std::move(localEntry.errorMessage);
        r.fromCache    = false;
        return r;
    }

    // -----------------------------------------------------------------------
    // Eager precompile
    // -----------------------------------------------------------------------

    void BytecodeCache::PrecompileCommonShapes(
        const BytecodeCacheMetadata& metadata,
        const std::string& canonicalHlslSource,
        const std::string& entryPoint,
        const std::string& target,
        const std::vector<std::string>& gpuBindableParamNames)
    {
        if (m_shutdown.load()) return;

        const uint64_t srcHash    = HashCanonicalSource(canonicalHlslSource);
        const uint64_t paramHash  = HashParamSignature(gpuBindableParamNames);
        const uint64_t includeHash = IncludeLibraryHash();
        const size_t   paramCount = gpuBindableParamNames.size();

        // Hard cap on bitset width. Today's effects use ~3-5 gpu params
        // max; complaining loudly here lets us catch a future change
        // before silent truncation bites.
        if (paramCount > 32)
        {
            // Still proceed with baseline only -- safer than crashing.
            // Future: widen to uint64_t or dynamic bitset.
        }

        auto enqueueShape = [&](uint32_t bitset)
        {
            BytecodeCompileRequest req;
            req.key.sourceHash         = srcHash;
            req.key.paramSignatureHash = paramHash;
            req.key.includeLibraryHash = includeHash;
            req.key.macroBitset        = bitset;
            req.key.entryPoint         = entryPoint;
            req.key.target             = target;
            req.metadata               = metadata;
            req.hlslSource             = canonicalHlslSource;
            req.gpuBindableParamNames  = gpuBindableParamNames;
            RequestCompile(std::move(req));
        };

        // Baseline (all params in cbuffer mode).
        enqueueShape(0u);
        // One variant per gpu-bindable param flipped to GPU mode.
        const size_t bits = std::min<size_t>(paramCount, 32);
        for (size_t i = 0; i < bits; ++i)
            enqueueShape(static_cast<uint32_t>(1u) << i);
    }

    // -----------------------------------------------------------------------
    // Invalidate / Clear / Stats
    // -----------------------------------------------------------------------

    void BytecodeCache::Invalidate(const BytecodeCompileKey& key)
    {
        std::lock_guard<std::mutex> g(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end()) return;
        if (it->second.status == BytecodeStatus::Pending)
        {
            // Don't yank an in-flight entry out from under a worker.
            return;
        }
        m_currentBytes -= it->second.bytecode.size();
        m_entries.erase(it);
    }

    void BytecodeCache::Clear()
    {
        std::lock_guard<std::mutex> g(m_mutex);
        // Don't drop Pending entries -- workers may be mid-compile.
        for (auto it = m_entries.begin(); it != m_entries.end(); )
        {
            if (it->second.status == BytecodeStatus::Pending)
            {
                ++it;
            }
            else
            {
                m_currentBytes -= it->second.bytecode.size();
                it = m_entries.erase(it);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Disk persistence (Phase 8 p8-cache-disk)
    // -----------------------------------------------------------------------
    //
    // Layout: <root>\<effectIdSafe>\<version>\<keyHashHex>.cso
    //
    // The keyHash combines all bytecode-determining fields into a single
    // 64-bit value: source + paramSig + includeLib + macroBitset + entry +
    // target. Two metadata fields (effectIdSafe, version) live in the
    // path purely so a human can inspect the cache and so the reaper can
    // find stale-version directories cheaply.

    namespace {
        std::wstring SanitizeForFilesystem(std::wstring_view in)
        {
            std::wstring out;
            out.reserve(in.size());
            for (wchar_t c : in)
            {
                if ((c >= L'A' && c <= L'Z') ||
                    (c >= L'a' && c <= L'z') ||
                    (c >= L'0' && c <= L'9') ||
                    c == L'_' || c == L'-' || c == L'.')
                    out.push_back(c);
                else
                    out.push_back(L'_');
            }
            // Cap length to keep paths well under MAX_PATH even for
            // deep cache trees. 64 chars is plenty for human-readable
            // effect names; collisions resolved by keyHash.
            if (out.size() > 64) out.resize(64);
            if (out.empty()) out = L"_unnamed";
            return out;
        }

        std::wstring HexFormat(uint64_t v)
        {
            wchar_t buf[17];
            swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(v));
            return buf;
        }

        bool EnsureDirectoryRecursive(const std::wstring& path)
        {
            // CreateDirectoryW fails if the path doesn't exist or any
            // parent is missing. Walk up and create as we go.
            if (path.empty()) return false;
            // Quick check: already exists?
            DWORD attrs = ::GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                (attrs & FILE_ATTRIBUTE_DIRECTORY))
                return true;

            // Walk parent.
            size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash > 2)  // skip drive letter
            {
                if (!EnsureDirectoryRecursive(path.substr(0, slash)))
                    return false;
            }
            BOOL ok = ::CreateDirectoryW(path.c_str(), nullptr);
            if (!ok)
            {
                DWORD err = ::GetLastError();
                if (err == ERROR_ALREADY_EXISTS) return true;
                return false;
            }
            return true;
        }
    }

    void BytecodeCache::SetDiskCacheRoot(std::wstring rootPath)
    {
        std::lock_guard<std::mutex> g(m_mutex);
        m_diskRoot = std::move(rootPath);
        if (!m_diskRoot.empty())
            EnsureDirectoryRecursive(m_diskRoot);
    }

    std::wstring BytecodeCache::KeyToDiskPath(
        const BytecodeCompileKey& key,
        const BytecodeCacheMetadata& meta) const
    {
        // Caller holds m_mutex.
        if (m_diskRoot.empty()) return {};

        // Single 64-bit hash of the full key for the filename.
        BytecodeCompileKeyHash hasher;
        const uint64_t keyHash = static_cast<uint64_t>(hasher(key));

        std::wstring effectIdSafe = SanitizeForFilesystem(
            meta.effectId.empty() ? L"_unknown" : meta.effectId);

        std::wstring path = m_diskRoot;
        if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
        path += effectIdSafe;
        path.push_back(L'\\');
        path += std::to_wstring(meta.version);
        path.push_back(L'\\');
        path += HexFormat(keyHash);
        path += L".cso";
        return path;
    }

    bool BytecodeCache::TryLoadFromDisk(
        const BytecodeCompileKey& key,
        std::vector<uint8_t>& outBytecode) const
    {
        // The on-disk filename is keyed by the FULL hash of all
        // bytecode-determining fields. We don't have effectId/version
        // for keys not in memory -- the disk layout uses metadata for
        // human-readable hierarchy, but lookup is by hash so we need
        // to know the directory. To keep `TryGet` cheap, we walk all
        // immediate `<effectIdSafe>/<version>/` children looking for
        // a file named `<keyHashHex>.cso`. With a typical cache of
        // 5-50 effects, that's a fast operation.
        if (m_diskRoot.empty()) return false;
        BytecodeCompileKeyHash hasher;
        std::wstring fname = HexFormat(static_cast<uint64_t>(hasher(key))) + L".cso";

        WIN32_FIND_DATAW fdEffect{};
        std::wstring effectGlob = m_diskRoot;
        if (!effectGlob.empty() && effectGlob.back() != L'\\') effectGlob.push_back(L'\\');
        effectGlob += L"*";
        HANDLE hEffect = ::FindFirstFileW(effectGlob.c_str(), &fdEffect);
        if (hEffect == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        std::wstring foundPath;
        do {
            if (!(fdEffect.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fdEffect.cFileName[0] == L'.') continue;
            std::wstring effectDir = m_diskRoot;
            if (effectDir.back() != L'\\') effectDir.push_back(L'\\');
            effectDir += fdEffect.cFileName;

            WIN32_FIND_DATAW fdVer{};
            std::wstring verGlob = effectDir + L"\\*";
            HANDLE hVer = ::FindFirstFileW(verGlob.c_str(), &fdVer);
            if (hVer == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fdVer.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fdVer.cFileName[0] == L'.') continue;
                std::wstring candidate = effectDir + L"\\" + fdVer.cFileName + L"\\" + fname;
                DWORD a = ::GetFileAttributesW(candidate.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                {
                    foundPath = std::move(candidate);
                    found = true;
                    break;
                }
            } while (::FindNextFileW(hVer, &fdVer));
            ::FindClose(hVer);
            if (found) break;
        } while (::FindNextFileW(hEffect, &fdEffect));
        ::FindClose(hEffect);

        if (!found) return false;

        // Read the file.
        HANDLE hFile = ::CreateFileW(foundPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!::GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0 ||
            sz.QuadPart > 16ull * 1024 * 1024)  // 16 MB sanity cap
        {
            ::CloseHandle(hFile);
            return false;
        }
        outBytecode.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        BOOL ok = ::ReadFile(hFile, outBytecode.data(),
            static_cast<DWORD>(outBytecode.size()), &got, nullptr);
        ::CloseHandle(hFile);
        if (!ok || got != outBytecode.size())
        {
            outBytecode.clear();
            return false;
        }
        // Touch atime so the reaper sees the load. Use SetFileTime with
        // current FILETIME on the LastAccessTime field.
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        HANDLE hTouch = ::CreateFileW(foundPath.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hTouch != INVALID_HANDLE_VALUE)
        {
            ::SetFileTime(hTouch, nullptr, &now, &now);
            ::CloseHandle(hTouch);
        }
        return true;
    }

    void BytecodeCache::WriteToDisk(
        const BytecodeCompileKey& key,
        const BytecodeCacheMetadata& meta,
        const std::vector<uint8_t>& bytecode)
    {
        // Caller holds m_mutex (we do file I/O while holding the lock
        // -- not great but cache-disk writes are ~10 KB and the worker
        // thread does this off the render path).
        if (m_diskRoot.empty() || bytecode.empty()) return;
        std::wstring path = KeyToDiskPath(key, meta);
        if (path.empty()) return;

        // Create parent dir.
        size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        if (!EnsureDirectoryRecursive(path.substr(0, slash))) return;

        // Atomic write: temp file in parent dir + MoveFileEx replace.
        std::wstring tmp = path + L".tmp";
        HANDLE hFile = ::CreateFileW(tmp.c_str(), GENERIC_WRITE,
            0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        BOOL ok = ::WriteFile(hFile, bytecode.data(),
            static_cast<DWORD>(bytecode.size()), &written, nullptr);
        ::CloseHandle(hFile);
        if (!ok || written != bytecode.size())
        {
            ::DeleteFileW(tmp.c_str());
            return;
        }
        if (!::MoveFileExW(tmp.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            ::DeleteFileW(tmp.c_str());
        }
    }

    BytecodeCache::ReapResult BytecodeCache::ReapDisk(uint64_t staleThresholdSec)
    {
        ReapResult r{};
        std::wstring root;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            root = m_diskRoot;
        }
        if (root.empty()) return r;

        // FILETIME = 100ns since 1601. Threshold: now - staleThresholdSec.
        // Special case: staleThresholdSec == 0 means "reap everything"
        // (force threshold above all possible atimes).
        FILETIME nowFt{};
        ::GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER nowUli; nowUli.LowPart = nowFt.dwLowDateTime;
        nowUli.HighPart = nowFt.dwHighDateTime;
        ULONGLONG thresholdUli;
        const bool reapAll = (staleThresholdSec == 0);
        if (reapAll)
            thresholdUli = ~0ull;  // any atime is < this; everything reaped.
        else
            thresholdUli = nowUli.QuadPart -
                (static_cast<ULONGLONG>(staleThresholdSec) * 10'000'000ull);

        // Walk root\effectIdSafe\version\*.cso
        WIN32_FIND_DATAW fdE{};
        std::wstring effectGlob = root;
        if (effectGlob.back() != L'\\') effectGlob.push_back(L'\\');
        effectGlob += L"*";
        HANDLE hE = ::FindFirstFileW(effectGlob.c_str(), &fdE);
        if (hE == INVALID_HANDLE_VALUE) return r;
        do {
            if (!(fdE.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fdE.cFileName[0] == L'.') continue;
            std::wstring effectDir = root;
            if (effectDir.back() != L'\\') effectDir.push_back(L'\\');
            effectDir += fdE.cFileName;

            WIN32_FIND_DATAW fdV{};
            std::wstring vGlob = effectDir + L"\\*";
            HANDLE hV = ::FindFirstFileW(vGlob.c_str(), &fdV);
            if (hV == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fdV.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fdV.cFileName[0] == L'.') continue;
                std::wstring vDir = effectDir + L"\\" + fdV.cFileName;
                WIN32_FIND_DATAW fdF{};
                std::wstring fGlob = vDir + L"\\*.cso";
                HANDLE hF = ::FindFirstFileW(fGlob.c_str(), &fdF);
                if (hF == INVALID_HANDLE_VALUE) continue;
                do {
                    ULARGE_INTEGER atime;
                    atime.LowPart  = fdF.ftLastAccessTime.dwLowDateTime;
                    atime.HighPart = fdF.ftLastAccessTime.dwHighDateTime;
                    if (atime.QuadPart >= thresholdUli) continue;

                    std::wstring full = vDir + L"\\" + fdF.cFileName;
                    LARGE_INTEGER sz{};
                    sz.LowPart  = fdF.nFileSizeLow;
                    sz.HighPart = fdF.nFileSizeHigh;
                    if (::DeleteFileW(full.c_str()))
                    {
                        ++r.filesDeleted;
                        r.bytesFreed += static_cast<size_t>(sz.QuadPart);
                    }
                    else
                    {
                        ++r.errors;
                    }
                } while (::FindNextFileW(hF, &fdF));
                ::FindClose(hF);
                // Try to rmdir empty version dir (best-effort).
                ::RemoveDirectoryW(vDir.c_str());
            } while (::FindNextFileW(hV, &fdV));
            ::FindClose(hV);
            ::RemoveDirectoryW(effectDir.c_str());
        } while (::FindNextFileW(hE, &fdE));
        ::FindClose(hE);
        return r;
    }

    BytecodeCache::ReapResult BytecodeCache::ClearDisk()
    {
        // staleThresholdSec=0 is the "reap everything" sentinel.
        return ReapDisk(0);
    }

    BytecodeCache::Stats BytecodeCache::GetStats() const
    {
        Stats s{};
        {
            std::lock_guard<std::mutex> g(m_mutex);
            s.entries            = m_entries.size();
            s.totalBytecodeBytes = m_currentBytes;
            for (const auto& [k, e] : m_entries)
            {
                if (e.status == BytecodeStatus::Pending) ++s.pendingCount;
                else if (e.status == BytecodeStatus::Failed) ++s.failedCount;
            }
        }
        s.inlineCompiles = m_inlineCompiles.load(std::memory_order_relaxed);
        s.workerCompiles = m_workerCompiles.load(std::memory_order_relaxed);
        s.cacheHits      = m_cacheHits.load(std::memory_order_relaxed);
        s.cacheMisses    = m_cacheMisses.load(std::memory_order_relaxed);
        return s;
    }

    // -----------------------------------------------------------------------
    // Worker loop + compile core
    // -----------------------------------------------------------------------

    void BytecodeCache::WorkerLoop(std::stop_token stop)
    {
        while (!stop.stop_requested() && !m_shutdown.load())
        {
            BytecodeCompileRequest req;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workCv.wait(lock, [&] {
                    return stop.stop_requested() || m_shutdown.load() || !m_workQueue.empty();
                });
                if (stop.stop_requested() || m_shutdown.load()) return;
                if (m_workQueue.empty()) continue;
                req = std::move(m_workQueue.front().request);
                m_workQueue.pop_front();
            }

            // Compile outside the queue mutex.
            std::unique_lock<std::mutex> lock(m_mutex);
            auto it = m_entries.find(req.key);
            if (it == m_entries.end())
            {
                // Got Invalidated between enqueue and dequeue -- skip.
                continue;
            }
            // If the inline-compile path already filled this entry while
            // we were dequeueing, leave it alone.
            if (it->second.status == BytecodeStatus::Ready ||
                it->second.status == BytecodeStatus::Failed)
            {
                continue;
            }

            DoCompile(req, lock, it->second);
            EnforceByteLimit_NoLock();
            m_workerCompiles.fetch_add(1, std::memory_order_relaxed);

            // Persist to disk on success (best-effort, with the lock
            // held -- file writes are small and the worker is off the
            // render thread).
            if (it->second.status == BytecodeStatus::Ready &&
                !m_diskRoot.empty())
            {
                WriteToDisk(req.key, req.metadata, it->second.bytecode);
            }

            lock.unlock();
            m_readyCv.notify_all();
        }
    }

    BytecodeStatus BytecodeCache::DoCompile(
        const BytecodeCompileRequest& req,
        std::unique_lock<std::mutex>& lock,
        Entry& outEntry)
    {
        // Build the macro list from the param-name vector + bitset.
        // Strings must outlive D3DCompile, so keep them owned locally.
        std::vector<std::string> defNames;
        std::vector<std::string> defValues;
        defNames.reserve(req.gpuBindableParamNames.size());
        defValues.reserve(req.gpuBindableParamNames.size());
        for (size_t i = 0; i < req.gpuBindableParamNames.size() && i < 32; ++i)
        {
            defNames.push_back("_SLPARAM_" + req.gpuBindableParamNames[i] + "_GPU");
            defValues.push_back(((req.key.macroBitset >> i) & 1u) ? "1" : "0");
        }
        std::vector<ShaderCompiler::MacroDef> macros;
        macros.reserve(defNames.size());
        for (size_t i = 0; i < defNames.size(); ++i)
            macros.push_back({ defNames[i].c_str(), defValues[i].c_str() });

        // Drop the lock around D3DCompile -- it can take 50-300ms in
        // debug. Other readers can still TryGet the (Pending) entry,
        // and workers can pick up other queue items.
        std::string hlsl = req.hlslSource;
        std::string entry = req.key.entryPoint;
        std::string target = req.key.target;
        lock.unlock();

        ShaderCompileResult result = ShaderCompiler::CompileFromString(
            hlsl, "BytecodeCache", entry, target, macros);

        lock.lock();
        // Race check: if the entry was wiped or completed under us,
        // discard our result. Specifically, NEVER overwrite a Ready
        // entry with a Failed result (or any worker result with a
        // Ready inline-compile result that beat us).
        // outEntry is a reference into m_entries[req.key]; the entry's
        // status drives the merge.
        if (outEntry.status == BytecodeStatus::Ready ||
            outEntry.status == BytecodeStatus::Failed)
        {
            return outEntry.status;
        }

        if (result.bytecode)
        {
            const uint8_t* p = static_cast<const uint8_t*>(result.bytecode->GetBufferPointer());
            size_t sz = result.bytecode->GetBufferSize();
            outEntry.bytecode.assign(p, p + sz);
            outEntry.status = BytecodeStatus::Ready;
            outEntry.errorMessage.clear();
            m_currentBytes += sz;
            return BytecodeStatus::Ready;
        }

        outEntry.status = BytecodeStatus::Failed;
        outEntry.errorMessage = result.ErrorMessage();
        outEntry.bytecode.clear();
        if (outEntry.errorMessage.empty())
            outEntry.errorMessage = L"D3DCompile failed (no diagnostic).";
        return BytecodeStatus::Failed;
    }

    void BytecodeCache::EnforceByteLimit_NoLock()
    {
        if (m_currentBytes <= m_maxBytes) return;

        // Naive LRU on insertion order. Build a vector of
        // (insertionOrder, key) for non-Pending entries, sort by
        // order ascending, evict from front until we're back under cap.
        struct Victim { uint64_t order; BytecodeCompileKey key; };
        std::vector<Victim> victims;
        victims.reserve(m_entries.size());
        for (auto& [k, e] : m_entries)
        {
            if (e.status == BytecodeStatus::Pending) continue;
            victims.push_back({ e.insertionOrder, k });
        }
        std::sort(victims.begin(), victims.end(),
            [](const Victim& a, const Victim& b) { return a.order < b.order; });

        for (auto& v : victims)
        {
            if (m_currentBytes <= m_maxBytes) break;
            auto it = m_entries.find(v.key);
            if (it == m_entries.end()) continue;
            m_currentBytes -= it->second.bytecode.size();
            m_entries.erase(it);
        }
    }
}
