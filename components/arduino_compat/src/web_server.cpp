// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "DNSServer.h"
#include "WebServer.h"
#include "lwip/sockets.h"

namespace {
constexpr size_t MAX_REQUEST = 8192 + 2048;
constexpr uint32_t REQUEST_DEADLINE_MS = 3000;

int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += char(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "Status";
    }
}
}  // namespace

void WebServer::begin()
{
    stop();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(port_));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    listen_fd_ = fd;
}

void WebServer::stop()
{
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
    listen_fd_ = -1;
}

void WebServer::on(const String &uri, HTTPMethod method, Handler handler)
{
    routes_.push_back({uri.c_str(), method, std::move(handler)});
}

void WebServer::parse_arguments(const std::string &encoded)
{
    size_t start = 0;
    while (start <= encoded.size()) {
        size_t end = encoded.find('&', start);
        std::string pair = encoded.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!pair.empty()) {
            size_t equals = pair.find('=');
            args_.emplace_back(url_decode(pair.substr(0, equals)),
                               equals == std::string::npos ? std::string() : url_decode(pair.substr(equals + 1)));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

String WebServer::arg(const String &name) const
{
    for (auto &entry : args_) {
        if (entry.first == name.c_str()) {
            return String(entry.second);
        }
    }
    return String();
}

bool WebServer::hasArg(const String &name) const
{
    for (auto &entry : args_) {
        if (entry.first == name.c_str()) {
            return true;
        }
    }
    return false;
}

void WebServer::sendHeader(const String &name, const String &value, bool first)
{
    std::string line = std::string(name.c_str()) + ": " + value.c_str() + "\r\n";
    response_headers_ = first ? line + response_headers_ : response_headers_ + line;
}

void WebServer::send(int code, const char *content_type, const String &body)
{
    if (client_fd_ < 0 || responded_) {
        return;
    }
    responded_ = true;
    std::string head = "HTTP/1.0 " + std::to_string(code) + " " + reason(code) + "\r\n" + response_headers_ +
                       "Content-Type: " + (content_type ? content_type : "text/plain") +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.length()) + "\r\n\r\n";
    std::string all = head + body.c_str();
    size_t sent = 0;
    uint32_t began = millis();
    while (sent < all.size() && millis() - began < REQUEST_DEADLINE_MS) {
        ssize_t n = ::send(client_fd_, all.data() + sent, all.size() - sent, 0);
        if (n > 0) {
            sent += size_t(n);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        } else {
            delay(5);
        }
    }
}

void WebServer::handleClient()
{
    if (listen_fd_ < 0) {
        return;
    }
    int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    std::string request;
    size_t header_end = std::string::npos, body_length = 0;
    uint32_t began = millis();
    char chunk[512];
    while (millis() - began < REQUEST_DEADLINE_MS && request.size() < MAX_REQUEST) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            request.append(chunk, size_t(n));
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            break;
        } else {
            delay(5);
        }
        if (header_end == std::string::npos) {
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string lower = request.substr(0, header_end);
                for (auto &c : lower) c = char(tolower((unsigned char)c));
                size_t at = lower.find("\r\ncontent-length:");
                body_length = at == std::string::npos ? 0 : size_t(strtoul(lower.c_str() + at + 17, nullptr, 10));
                if (body_length > 8192) {
                    break;
                }
            }
        }
        if (header_end != std::string::npos && request.size() >= header_end + 4 + body_length) {
            break;
        }
    }
    client_fd_ = fd;
    responded_ = false;
    response_headers_.clear();
    args_.clear();
    if (header_end != std::string::npos && body_length <= 8192 && request.size() >= header_end + 4 + body_length) {
        size_t method_end = request.find(' ');
        size_t uri_end = method_end == std::string::npos ? std::string::npos : request.find(' ', method_end + 1);
        if (uri_end != std::string::npos) {
            std::string method = request.substr(0, method_end);
            std::string uri = request.substr(method_end + 1, uri_end - method_end - 1);
            size_t query = uri.find('?');
            if (query != std::string::npos) {
                parse_arguments(uri.substr(query + 1));
                uri.resize(query);
            }
            if (method == "POST") {
                parse_arguments(request.substr(header_end + 4, body_length));
            }
            HTTPMethod verb = method == "POST" ? HTTP_POST : HTTP_GET;
            bool handled = false;
            for (auto &route : routes_) {
                if (route.uri == uri && (route.method == HTTP_ANY || route.method == verb)) {
                    route.handler();
                    handled = true;
                    break;
                }
            }
            if (!handled && not_found_) {
                not_found_();
            }
        }
    }
    if (!responded_) {
        send(400, "text/plain", "Bad request");
    }
    // Form fields include Wi-Fi passwords and tokens.
    std::fill(request.begin(), request.end(), '\0');
    for (auto &entry : args_) {
        std::fill(entry.second.begin(), entry.second.end(), '\0');
    }
    args_.clear();
    close(fd);
    client_fd_ = -1;
}

// ---- captive DNS ---------------------------------------------------------

bool DNSServer::start(uint16_t port, const String &, IPAddress address)
{
    stop();
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(port);
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&bind_address), sizeof(bind_address)) != 0) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fd_ = fd;
    address_ = address;
    return true;
}

void DNSServer::stop()
{
    if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = -1;
}

void DNSServer::processNextRequest()
{
    if (fd_ < 0) {
        return;
    }
    uint8_t packet[512];
    sockaddr_in from{};
    socklen_t from_length = sizeof(from);
    ssize_t n = recvfrom(fd_, packet, sizeof(packet) - 16, 0, reinterpret_cast<sockaddr *>(&from), &from_length);
    // Header (12) plus one question; only standard queries are answered.
    if (n < 17 || (packet[2] & 0x80) || ((packet[2] >> 3) & 0x0f) != 0 || packet[4] != 0 || packet[5] != 1) {
        return;
    }
    size_t at = 12;
    while (at < size_t(n) && packet[at]) {
        at += size_t(packet[at]) + 1;
    }
    if (at + 5 > size_t(n)) {
        return;
    }
    size_t question_end = at + 5;
    packet[2] = 0x84 | (packet[2] & 0x01);  // response, authoritative, keep RD
    packet[3] = 0x80;                      // recursion available, no error
    packet[6] = 0;
    packet[7] = 1;  // one answer
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    size_t length = question_end;
    const uint8_t answer[] = {0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04};
    memcpy(packet + length, answer, sizeof(answer));
    length += sizeof(answer);
    uint32_t ip = uint32_t(address_);
    memcpy(packet + length, &ip, 4);
    length += 4;
    sendto(fd_, packet, length, 0, reinterpret_cast<sockaddr *>(&from), from_length);
}
