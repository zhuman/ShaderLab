#pragma once

#include "../../EngineExport.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

// Forward-declare to avoid pulling winsock headers into every TU.
typedef unsigned long long SOCKET_T;

namespace ShaderLab
{
    // Lightweight HTTP server using raw Winsock2 TCP sockets.
    // Runs on a background thread; handlers must dispatch UI-thread work
    // via DispatcherQueue themselves.
    class SHADERLAB_API McpHttpServer
    {
    public:
        struct Response
        {
            uint16_t    statusCode{ 200 };
            std::string body;
            std::string contentType{ "application/json" };
        };

        using Handler = std::function<Response(const std::wstring& path, const std::string& body)>;

        // Activity callback: invoked on the listener thread immediately after each
        // request is routed (one call per HTTP request, regardless of route match).
        // The callback MUST be cheap and thread-safe — UI updates should be deferred
        // to the UI thread by the consumer.  Arguments:
        //   method      — HTTP verb (e.g. "GET", "POST")
        //   path        — full request path (after query strip)
        //   statusCode  — final HTTP status returned to the client
        //   peerAddress — "127.0.0.1:54321" style string for the remote endpoint
        using ActivityCallback = std::function<void(
            const std::string& method,
            const std::wstring& path,
            uint16_t statusCode,
            const std::string& peerAddress)>;

        McpHttpServer() = default;
        ~McpHttpServer();

        void AddRoute(const std::wstring& method, const std::wstring& pathPrefix, Handler handler);
        bool Start(uint16_t port = 47808);
        void Stop();
        bool IsRunning() const { return m_running.load(); }
        uint16_t Port() const { return m_port; }

        // Register a callback invoked after every HTTP request the server handles.
        // Set to nullptr to clear.  Safe to call before or after Start().
        void SetActivityCallback(ActivityCallback cb);

        // Route a request programmatically (used by the MCP JSON-RPC handler).
        Response RouteRequest(const std::wstring& method, const std::wstring& path, const std::string& body);

    private:
        void ListenerThread(uint16_t port);
        void HandleConnection(SOCKET_T clientSock);

        struct Route
        {
            std::wstring method;
            std::wstring pathPrefix;
            Handler      handler;
        };

        std::vector<Route>  m_routes;
        SOCKET_T            m_listenSock{ ~0ULL };
        std::jthread        m_thread;
        std::atomic<bool>   m_running{ false };
        uint16_t            m_port{ 0 };

        std::mutex          m_activityMutex;
        ActivityCallback    m_activityCallback;
    };
}
