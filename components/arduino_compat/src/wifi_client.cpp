// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "WiFiClientSecure.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "lwip/sockets.h"

namespace {
const char *TAG = "net_client";

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}
}  // namespace

// ---- plain TCP -------------------------------------------------------------

bool WiFiClient::ensure_rx()
{
    if (!rx_) {
        rx_ = static_cast<uint8_t *>(heap_caps_malloc(RX_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!rx_) {
            rx_ = static_cast<uint8_t *>(malloc(RX_CAPACITY));
        }
    }
    return rx_ != nullptr;
}

int WiFiClient::connect(const char *host, uint16_t port, int32_t timeout_ms)
{
    stop();
    if (!host || !host[0]) {
        return 0;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    char service[8];
    snprintf(service, sizeof(service), "%u", unsigned(port));
    if (getaddrinfo(host, service, &hints, &result) != 0 || !result) {
        return 0;
    }
    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        return 0;
    }
    set_nonblocking(fd);
    int rc = ::connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return 0;
    }
    if (rc != 0) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int error = 0;
        socklen_t length = sizeof(error);
        if (select(fd + 1, nullptr, &writable, nullptr, &timeout) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            close(fd);
            return 0;
        }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fd_ = fd;
    eof_ = false;
    rx_len_ = rx_pos_ = 0;
    if (!ensure_rx()) {
        stop();
        return 0;
    }
    return 1;
}

bool WiFiClient::wait_writable(uint32_t timeout_ms)
{
    if (fd_ < 0) {
        return false;
    }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd_, &writable);
    timeval timeout{long(timeout_ms / 1000), long((timeout_ms % 1000) * 1000)};
    return select(fd_ + 1, nullptr, &writable, nullptr, &timeout) > 0;
}

int WiFiClient::send_some(const uint8_t *data, size_t size)
{
    ssize_t sent = ::send(fd_, data, size, 0);
    if (sent < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
    return int(sent);
}

size_t WiFiClient::write(const uint8_t *data, size_t size)
{
    size_t written = 0;
    uint32_t began = millis();
    while (written < size && connected()) {
        int sent = send_some(data + written, size - written);
        if (sent < 0) {
            break;
        }
        written += size_t(sent);
        if (written < size) {
            if (millis() - began >= io_timeout_ms_) {
                break;
            }
            wait_writable(50);
        }
    }
    return written;
}

bool WiFiClient::fill()
{
    if (fd_ < 0 || eof_ || !rx_) {
        return false;
    }
    if (rx_pos_ == rx_len_) {
        rx_pos_ = rx_len_ = 0;
    }
    if (rx_len_ == RX_CAPACITY) {
        return true;
    }
    ssize_t got = ::recv(fd_, rx_ + rx_len_, RX_CAPACITY - rx_len_, MSG_DONTWAIT);
    if (got > 0) {
        rx_len_ += size_t(got);
        return true;
    }
    if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        eof_ = true;
        return false;
    }
    return true;
}

int WiFiClient::available()
{
    fill();
    return int(rx_len_ - rx_pos_);
}

int WiFiClient::read()
{
    uint8_t value;
    return read(&value, 1) == 1 ? value : -1;
}

int WiFiClient::read(uint8_t *buffer, size_t size)
{
    if (rx_pos_ == rx_len_) {
        fill();
    }
    size_t count = std::min(size, rx_len_ - rx_pos_);
    if (!count) {
        return -1;
    }
    memcpy(buffer, rx_ + rx_pos_, count);
    rx_pos_ += count;
    return int(count);
}

int WiFiClient::peek()
{
    if (rx_pos_ == rx_len_) {
        fill();
    }
    return rx_pos_ < rx_len_ ? rx_[rx_pos_] : -1;
}

uint8_t WiFiClient::connected()
{
    if (rx_pos_ < rx_len_) {
        return 1;
    }
    fill();
    return rx_pos_ < rx_len_ || (fd_ >= 0 && !eof_);
}

void WiFiClient::stop()
{
    if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = -1;
    eof_ = true;
    rx_len_ = rx_pos_ = 0;
    free(rx_);
    rx_ = nullptr;
}

// ---- TLS ---------------------------------------------------------------------

int WiFiClientSecure::connect(const char *host, uint16_t port, int32_t timeout_ms)
{
    stop();
    // A pinned CA is mandatory; Glimdock never uses insecure TLS.
    if (!host || !host[0] || ca_.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
        return 0;
    }
    esp_tls_cfg_t config{};
    config.cacert_buf = reinterpret_cast<const unsigned char *>(ca_.c_str());
    config.cacert_bytes = ca_.size() + 1;
    config.timeout_ms = int(std::max<int32_t>(timeout_ms, 0) + int32_t(sslclient->handshake_timeout));
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        return 0;
    }
    if (esp_tls_conn_new_sync(host, int(strlen(host)), port, &config, tls) != 1) {
        ESP_LOGW(TAG, "TLS connection to port %u failed", unsigned(port));
        esp_tls_conn_destroy(tls);
        return 0;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
        esp_tls_conn_destroy(tls);
        return 0;
    }
    set_nonblocking(fd);
    tls_ = tls;
    fd_ = fd;
    eof_ = false;
    rx_len_ = rx_pos_ = 0;
    if (!ensure_rx()) {
        stop();
        return 0;
    }
    return 1;
}

bool WiFiClientSecure::fill()
{
    if (!tls_ || eof_ || !rx_) {
        return false;
    }
    if (rx_pos_ == rx_len_) {
        rx_pos_ = rx_len_ = 0;
    }
    if (rx_len_ == RX_CAPACITY) {
        return true;
    }
    ssize_t got = esp_tls_conn_read(tls_, rx_ + rx_len_, RX_CAPACITY - rx_len_);
    if (got > 0) {
        rx_len_ += size_t(got);
        return true;
    }
    if (got == ESP_TLS_ERR_SSL_WANT_READ || got == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return true;
    }
    eof_ = true;
    return false;
}

int WiFiClientSecure::send_some(const uint8_t *data, size_t size)
{
    ssize_t sent = esp_tls_conn_write(tls_, data, size);
    if (sent >= 0) {
        return int(sent);
    }
    return sent == ESP_TLS_ERR_SSL_WANT_READ || sent == ESP_TLS_ERR_SSL_WANT_WRITE ? 0 : -1;
}

uint8_t WiFiClientSecure::connected()
{
    if (rx_pos_ < rx_len_) {
        return 1;
    }
    fill();
    return rx_pos_ < rx_len_ || (tls_ && !eof_);
}

void WiFiClientSecure::stop()
{
    if (tls_) {
        esp_tls_conn_destroy(tls_);  // closes the socket as well
        tls_ = nullptr;
        fd_ = -1;
    }
    WiFiClient::stop();
}
