// HTTP and WebSockets over WinHTTP.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace who {

std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);
// UrlEncode percent-encodes everything but unreserved characters.
std::string UrlEncode(std::string_view s);

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method = "GET", url;
    Headers headers;
    std::string body, contentType;
    size_t maxBytes = 32 << 20;
};

struct HttpResult {
    DWORD status = 0;  // 0 if the request failed before a response
    std::string body, error;
    std::map<std::string, std::string> headers;  // names in lower case
    bool ok() const { return status >= 200 && status < 300; }
    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

// Http is a WinHTTP session. Requests are synchronous and safe to make from
// several threads at once.
class Http {
public:
    explicit Http(const std::string& userAgent = "who");
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    HttpResult Send(const HttpRequest& req);
    HINTERNET session() const { return session_; }

private:
    HINTERNET session_ = nullptr;
};

// Multipart builds a multipart/form-data body.
class Multipart {
public:
    Multipart();
    void Field(const std::string& name, const std::string& value, const std::string& type = {});
    void File(const std::string& name, const std::string& filename, const std::string& bytes, const std::string& type);
    std::string Body() const { return body_ + "--" + boundary_ + "--\r\n"; }
    std::string ContentType() const { return "multipart/form-data; boundary=" + boundary_; }

private:
    std::string boundary_, body_;
};

// WebSocket is one WebSocket connection. Receive runs on one thread; Send
// and Abort may be called from others.
class WebSocket {
public:
    explicit WebSocket(const std::string& userAgent);
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    bool Connect(const std::string& url, const Headers& headers, std::string& error);
    bool Send(std::string_view text);
    bool SendBinary(std::string_view bytes);
    // Receive waits for a whole message. It returns false when the
    // connection closes or fails, with the close code if the server sent one.
    // binary, if given, says whether the message was binary rather than text.
    bool Receive(std::string& message, std::string& error, USHORT* closeCode, bool* binary = nullptr);
    // Abort drops the connection, making a Receive in progress return.
    void Abort();
    // Close says goodbye politely, then drops the connection.
    void Close(USHORT code = 1000);

private:
    HINTERNET take();
    Http http_;
    HINTERNET connect_ = nullptr;
    std::mutex mu_;
    HINTERNET ws_ = nullptr;
    std::mutex sendMu_;
};

}  // namespace who
