// This file does NOT use the precompiled header because WinSock2.h
// must be included before windows.h, which the PCH already includes.
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_  // Prevent winsock1 from windows.h
#include <windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <format>
#include <cstdlib>

// Minimal redefinition of SOCKET_T to match header.
typedef unsigned long long SOCKET_T;

#include "McpHttpServer.h"

namespace ShaderLab
{
    McpHttpServer::~McpHttpServer()
    {
        Stop();
    }

    void McpHttpServer::AddRoute(const std::wstring& method, const std::wstring& pathPrefix, Handler handler)
    {
        m_routes.push_back({ method, pathPrefix, std::move(handler) });
    }

    void McpHttpServer::SetActivityCallback(ActivityCallback cb)
    {
        std::lock_guard lock(m_activityMutex);
        m_activityCallback = std::move(cb);
    }

    bool McpHttpServer::Start(uint16_t port)
    {
        if (m_running.load())
            return true;

        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
            return false;

        m_thread = std::jthread([this, port](std::stop_token) { ListenerThread(port); });
        return true;
    }

    void McpHttpServer::Stop()
    {
        m_running.store(false);

        // Close the listen socket to unblock accept().
        if (m_listenSock != ~0ULL)
        {
            closesocket(static_cast<SOCKET>(m_listenSock));
            m_listenSock = ~0ULL;
        }

        if (m_thread.joinable())
            m_thread.join();

        WSACleanup();
    }

    void McpHttpServer::ListenerThread(uint16_t port)
    {
        SOCKET sock = INVALID_SOCKET;

        // Try up to 10 ports starting from the requested port.
        for (uint16_t attempt = 0; attempt < 10; ++attempt)
        {
            uint16_t tryPort = port + attempt;
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET)
            {
                OutputDebugStringW(L"[MCP] socket() failed\n");
                return;
            }

            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(tryPort);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
            {
                OutputDebugStringW(std::format(L"[MCP] bind() failed on port {}, trying next\n", tryPort).c_str());
                closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }

            if (listen(sock, SOMAXCONN) == SOCKET_ERROR)
            {
                OutputDebugStringW(std::format(L"[MCP] listen() failed on port {}\n", tryPort).c_str());
                closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }

            // Success!
            m_port = tryPort;
            OutputDebugStringW(std::format(L"[MCP] Listening on port {}\n", tryPort).c_str());
            break;
        }

        if (sock == INVALID_SOCKET)
        {
            OutputDebugStringW(L"[MCP] Failed to bind to any port\n");
            return;
        }
        m_listenSock = static_cast<SOCKET_T>(sock);

        OutputDebugStringW(std::format(L"[MCP] Listening on http://localhost:{}/\n", port).c_str());
        m_running.store(true);

        while (m_running.load())
        {
            SOCKET client = accept(sock, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                break;

            // Handle each connection synchronously (simple; sufficient for MCP bridge).
            HandleConnection(static_cast<SOCKET_T>(client));
        }

        m_running.store(false);
    }

    void McpHttpServer::HandleConnection(SOCKET_T clientSock)
    {
        SOCKET s = static_cast<SOCKET>(clientSock);

        try
        {
        // Read the full HTTP request (up to 64KB).
        std::string raw;
        raw.resize(65536);
        int totalRead = 0;

        // Read until we have the complete headers + body.
        while (totalRead < static_cast<int>(raw.size()) - 1)
        {
            int n = recv(s, raw.data() + totalRead, static_cast<int>(raw.size()) - totalRead - 1, 0);
            if (n <= 0)
                break;
            totalRead += n;

            // Check if we have a complete HTTP request.
            std::string_view sv(raw.data(), totalRead);
            auto headerEnd = sv.find("\r\n\r\n");
            if (headerEnd != std::string_view::npos)
            {
                // Detect Transfer-Encoding: chunked (case-insensitive).
                bool chunked = false;
                {
                    std::string headersLower(sv.substr(0, headerEnd));
                    for (auto& c : headersLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    auto tePos = headersLower.find("transfer-encoding:");
                    if (tePos != std::string::npos)
                    {
                        auto teEnd = headersLower.find("\r\n", tePos);
                        if (teEnd != std::string::npos &&
                            headersLower.find("chunked", tePos, teEnd - tePos) != std::string::npos)
                        {
                            chunked = true;
                        }
                    }
                }

                if (chunked)
                {
                    // Done when we've seen the terminating "0\r\n\r\n" chunk.
                    if (sv.find("\r\n0\r\n\r\n", headerEnd) != std::string_view::npos)
                        break;
                    continue;
                }

                // Check Content-Length for body.
                auto clPos = sv.find("Content-Length:");
                if (clPos == std::string_view::npos)
                    clPos = sv.find("content-length:");
                if (clPos != std::string_view::npos)
                {
                    auto valStart = clPos + 15;
                    while (valStart < sv.size() && sv[valStart] == ' ') ++valStart;
                    auto valEnd = sv.find("\r\n", valStart);
                    int contentLen = std::atoi(std::string(sv.substr(valStart, valEnd - valStart)).c_str());
                    int bodyStart = static_cast<int>(headerEnd) + 4;
                    if (totalRead >= bodyStart + contentLen)
                        break; // Complete request.
                }
                else
                {
                    break; // No body expected.
                }
            }
        }
        raw.resize(totalRead);

        // Parse method and path from the request line.
        std::wstring method, path;
        std::string body;
        {
            auto lineEnd = raw.find("\r\n");
            if (lineEnd == std::string::npos)
            {
                closesocket(s);
                return;
            }
            std::string requestLine = raw.substr(0, lineEnd);
            auto sp1 = requestLine.find(' ');
            auto sp2 = requestLine.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos)
            {
                closesocket(s);
                return;
            }

            std::string methodStr = requestLine.substr(0, sp1);
            std::string pathStr = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

            method = std::wstring(methodStr.begin(), methodStr.end());
            path = std::wstring(pathStr.begin(), pathStr.end());

            // Remove query string.
            auto qpos = path.find(L'?');
            if (qpos != std::wstring::npos)
                path = path.substr(0, qpos);

            // Extract body.
            auto headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos && headerEnd + 4 < raw.size())
                body = raw.substr(headerEnd + 4);

            // If chunked, decode chunks into the actual payload.
            std::string headersBlock = (headerEnd != std::string::npos) ? raw.substr(0, headerEnd) : std::string{};
            std::string headersLower = headersBlock;
            for (auto& c : headersLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (headersLower.find("transfer-encoding:") != std::string::npos &&
                headersLower.find("chunked") != std::string::npos)
            {
                std::string decoded;
                size_t i = 0;
                while (i < body.size())
                {
                    auto crlf = body.find("\r\n", i);
                    if (crlf == std::string::npos) break;
                    std::string sizeLine = body.substr(i, crlf - i);
                    // Strip optional chunk extensions after ';'.
                    auto semi = sizeLine.find(';');
                    if (semi != std::string::npos) sizeLine.resize(semi);
                    size_t chunkSize = 0;
                    try { chunkSize = std::stoul(sizeLine, nullptr, 16); }
                    catch (...) { break; }
                    i = crlf + 2;
                    if (chunkSize == 0) break;
                    if (i + chunkSize > body.size()) break;
                    decoded.append(body, i, chunkSize);
                    i += chunkSize;
                    if (i + 2 <= body.size() && body[i] == '\r' && body[i + 1] == '\n')
                        i += 2;
                }
                body = std::move(decoded);
            }
        }

        // Handle CORS preflight.
        if (method == L"OPTIONS")
        {
            std::string resp =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Content-Length: 0\r\n"
                "\r\n";
            send(s, resp.data(), static_cast<int>(resp.size()), 0);
            closesocket(s);
            return;
        }

        // Route and respond.
        {
            std::string mlog(method.begin(), method.end());
            std::string plog(path.begin(), path.end());
            OutputDebugStringA(("[MCP] " + mlog + " " + plog + " (body=" + std::to_string(body.size()) + "B)\n").c_str());
            // Log headers for diagnosing parse failures (one-shot, capped).
            auto headerEnd = raw.find("\r\n\r\n");
            std::string headers = (headerEnd != std::string::npos) ? raw.substr(0, headerEnd) : raw;
            if (headers.size() > 1024) headers.resize(1024);
            OutputDebugStringA(("[MCP] headers:\n" + headers + "\n").c_str());
        }
        Response resp = RouteRequest(method, path, body);

        // Notify the activity callback BEFORE we send the response so
        // the caller's UI indicator pulses as close to the request edge
        // as possible.  Failure to format the peer address is non-fatal.
        {
            ActivityCallback cb;
            {
                std::lock_guard lock(m_activityMutex);
                cb = m_activityCallback;
            }
            if (cb)
            {
                std::string peerStr;
                sockaddr_in peer{};
                int peerLen = sizeof(peer);
                if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0)
                {
                    char ipBuf[64]{};
                    inet_ntop(AF_INET, &peer.sin_addr, ipBuf, sizeof(ipBuf));
                    peerStr = std::format("{}:{}", ipBuf, ntohs(peer.sin_port));
                }
                std::string methodUtf8;
                methodUtf8.reserve(method.size());
                for (wchar_t wc : method)
                    methodUtf8.push_back(static_cast<char>(wc & 0x7F));
                try { cb(methodUtf8, path, resp.statusCode, peerStr); }
                catch (...) { OutputDebugStringA("[MCP] activity callback threw\n"); }
            }
        }

        std::string statusText;
        switch (resp.statusCode)
        {
        case 200: statusText = "OK"; break;
        case 202: statusText = "Accepted"; break;
        case 204: statusText = "No Content"; break;
        case 400: statusText = "Bad Request"; break;
        case 404: statusText = "Not Found"; break;
        case 500: statusText = "Internal Server Error"; break;
        default:  statusText = "Error"; break;
        }
        std::string httpResp = std::format(
            "HTTP/1.1 {} {}\r\n"
            "Content-Type: {}\r\n"
            "Content-Length: {}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n",
            resp.statusCode, statusText, resp.contentType, resp.body.size());
        httpResp += resp.body;

        send(s, httpResp.data(), static_cast<int>(httpResp.size()), 0);
        closesocket(s);
        }
        catch (const std::exception& ex)
        {
            OutputDebugStringA((std::string("[MCP] HandleConnection std::exception: ") + ex.what() + "\n").c_str());
            try { closesocket(s); } catch (...) {}
        }
        catch (...)
        {
            // Last-resort barrier: any escaping exception (winrt::hresult_error,
            // SEH translation, etc.) tearing down the listener thread can crash
            // the process. Swallow + log + close the socket.
            OutputDebugStringW(L"[MCP] HandleConnection: unhandled non-standard exception\n");
            try { closesocket(s); } catch (...) {}
        }
    }

    McpHttpServer::Response McpHttpServer::RouteRequest(
        const std::wstring& method, const std::wstring& path, const std::string& body)
    {
        const Route* bestRoute = nullptr;
        size_t bestLen = 0;

        for (const auto& route : m_routes)
        {
            if (route.method != method)
                continue;
            if (path.starts_with(route.pathPrefix) && route.pathPrefix.size() > bestLen)
            {
                bestRoute = &route;
                bestLen = route.pathPrefix.size();
            }
        }

        if (bestRoute)
        {
            try
            {
                return bestRoute->handler(path, body);
            }
            catch (const std::exception& ex)
            {
                return { 500, std::string(R"({"error":")") + ex.what() + R"("})" };
            }
            catch (...)
            {
                // winrt::hresult_error and SEH translations land here. The
                // route handler must not allow these to escape because they
                // would unwind off the listener thread and (with /EHa) can
                // take the process with them.
                return { 500, R"({"error":"Unhandled non-standard exception in route handler"})" };
            }
        }

        return { 404, R"({"error":"Not found"})" };
    }
}
