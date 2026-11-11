#include "net.h"

#include <random>

namespace who {

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string UrlEncode(std::string_view s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

namespace {

std::string winError(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "server name not found";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "couldn't connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection lost";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
    case ERROR_WINHTTP_SECURE_FAILURE: return "secure connection failed";
    case ERROR_WINHTTP_INVALID_URL: return "invalid URL";
    }
    wchar_t* msg = nullptr;
    HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             mod, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string s = n ? Narrow(std::wstring_view(msg, n)) : "error " + std::to_string(code);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}

struct Target {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool crack(const std::string& url, Target& t, std::string& error) {
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        error = winError(GetLastError());
        return false;
    }
    t.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    t.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) t.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (t.path.empty()) t.path = L"/";
    t.port = uc.nPort;
    t.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::wstring headerBlock(const Headers& headers, const std::string& contentType) {
    std::wstring out;
    for (const auto& [k, v] : headers) out += Widen(k) + L": " + Widen(v) + L"\r\n";
    if (!contentType.empty()) out += L"Content-Type: " + Widen(contentType) + L"\r\n";
    return out;
}

DWORD statusOf(HINTERNET req) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

std::map<std::string, std::string> responseHeaders(HINTERNET req) {
    std::map<std::string, std::string> out;
    DWORD size = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return out;
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                             WINHTTP_NO_HEADER_INDEX))
        return out;
    std::string s = Narrow(raw);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find("\r\n", pos);
        if (end == std::string::npos) end = s.size();
        std::string line = s.substr(pos, end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (auto& c : name) c = char(tolower((unsigned char)c));
            size_t v = colon + 1;
            while (v < line.size() && line[v] == ' ') v++;
            out[name] = line.substr(v);
        }
        pos = end + 2;
    }
    return out;
}

}  // namespace

Http::Http(const std::string& userAgent) {
    session_ = WinHttpOpen(Widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        WinHttpSetTimeouts(session_, 10000, 10000, 30000, 60000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(session_, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
        DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(session_, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2));
    }
}

Http::~Http() {
    if (session_) WinHttpCloseHandle(session_);
}

HttpResult Http::Send(const HttpRequest& in) {
    HttpResult r;
    Target t;
    if (!session_) {
        r.error = "WinHTTP unavailable";
        return r;
    }
    if (!crack(in.url, t, r.error)) return r;
    HINTERNET conn = WinHttpConnect(session_, t.host.c_str(), t.port, 0);
    if (!conn) {
        r.error = winError(GetLastError());
        return r;
    }
    HINTERNET req = WinHttpOpenRequest(conn, Widen(in.method).c_str(), t.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, t.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        r.error = winError(GetLastError());
        WinHttpCloseHandle(conn);
        return r;
    }
    std::wstring headers = headerBlock(in.headers, in.contentType);
    LPVOID data = in.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(in.body.data());
    if (!WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), DWORD(-1), data,
                            DWORD(in.body.size()), DWORD(in.body.size()), 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        r.error = winError(GetLastError());
    } else {
        r.status = statusOf(req);
        r.headers = responseHeaders(req);
        char buf[32768];
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(req, buf, sizeof(buf), &n)) {
                r.error = winError(GetLastError());
                break;
            }
            if (n == 0) break;
            if (r.body.size() + n > in.maxBytes) {
                r.error = "response too large";
                break;
            }
            r.body.append(buf, n);
        }
        if (r.error.empty() && !r.ok()) r.error = "server returned " + std::to_string(r.status);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return r;
}

Multipart::Multipart() {
    std::random_device rd;
    char buf[40];
    snprintf(buf, sizeof(buf), "----who%08x%08x", rd(), rd());
    boundary_ = buf;
}

void Multipart::Field(const std::string& name, const std::string& value, const std::string& type) {
    body_ += "--" + boundary_ + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n";
    if (!type.empty()) body_ += "Content-Type: " + type + "\r\n";
    body_ += "\r\n" + value + "\r\n";
}

void Multipart::File(const std::string& name, const std::string& filename, const std::string& bytes,
                     const std::string& type) {
    std::string safe;
    for (char c : filename) safe += (c == '"' || c == '\r' || c == '\n') ? '_' : c;
    body_ += "--" + boundary_ + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" + safe +
             "\"\r\nContent-Type: " + type + "\r\n\r\n";
    body_ += bytes;
    body_ += "\r\n";
}

WebSocket::WebSocket(const std::string& userAgent) : http_(userAgent) {}

WebSocket::~WebSocket() {
    Abort();
    if (connect_) WinHttpCloseHandle(connect_);
}

bool WebSocket::Connect(const std::string& url, const Headers& headers, std::string& error) {
    std::string u = url;
    if (u.rfind("wss://", 0) == 0) u = "https://" + u.substr(6);
    Target t;
    if (!crack(u, t, error)) return false;
    connect_ = WinHttpConnect(http_.session(), t.host.c_str(), t.port, 0);
    if (!connect_) {
        error = winError(GetLastError());
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(connect_, L"GET", t.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        error = winError(GetLastError());
        return false;
    }
    std::wstring hb = headerBlock(headers, {});
    bool ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(req, hb.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hb.c_str(), DWORD(-1),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        error = winError(GetLastError());
        WinHttpCloseHandle(req);
        return false;
    }
    if (DWORD status = statusOf(req); status != 101) {
        error = "server returned " + std::to_string(status);
        WinHttpCloseHandle(req);
        return false;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!ws) {
        error = winError(GetLastError());
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    ws_ = ws;
    return true;
}

bool WebSocket::Send(std::string_view text) {
    std::lock_guard<std::mutex> send(sendMu_);
    HINTERNET ws;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ws = ws_;
    }
    if (!ws) return false;
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(),
                                DWORD(text.size())) == NO_ERROR;
}

bool WebSocket::SendBinary(std::string_view bytes) {
    std::lock_guard<std::mutex> send(sendMu_);
    HINTERNET ws;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ws = ws_;
    }
    if (!ws) return false;
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)bytes.data(),
                                DWORD(bytes.size())) == NO_ERROR;
}

bool WebSocket::Receive(std::string& message, std::string& error, USHORT* closeCode, bool* binary) {
    message.clear();
    if (closeCode) *closeCode = 0;
    char buf[65536];
    for (;;) {
        HINTERNET ws;
        {
            std::lock_guard<std::mutex> lock(mu_);
            ws = ws_;
        }
        if (!ws) {
            error = "connection closed";
            return false;
        }
        DWORD n = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD rc = WinHttpWebSocketReceive(ws, buf, sizeof(buf), &n, &type);
        if (rc != NO_ERROR) {
            error = winError(rc);
            return false;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT code = 0;
            char reason[128];
            DWORD len = 0;
            WinHttpWebSocketQueryCloseStatus(ws, &code, reason, sizeof(reason), &len);
            if (closeCode) *closeCode = code;
            error = "closed (" + std::to_string(code) + (len ? ": " + std::string(reason, len) : "") + ")";
            return false;
        }
        message.append(buf, n);
        if (message.size() > (256u << 20)) {
            error = "message too large";
            return false;
        }
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if (binary) *binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            return true;
        }
    }
}

HINTERNET WebSocket::take() {
    std::lock_guard<std::mutex> lock(mu_);
    HINTERNET ws = ws_;
    ws_ = nullptr;
    return ws;
}

void WebSocket::Abort() {
    if (HINTERNET ws = take()) WinHttpCloseHandle(ws);
}

void WebSocket::Close(USHORT code) {
    HINTERNET ws;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ws = ws_;
    }
    if (ws) WinHttpWebSocketShutdown(ws, code, nullptr, 0);
    Abort();
}

}  // namespace who
