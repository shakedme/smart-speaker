#include "TLSSocket.h"

#include <mbedtls/ctr_drbg.h>     // for mbedtls_ctr_drbg_free, mbedtls_ctr_...
#include <mbedtls/entropy.h>      // for mbedtls_entropy_free, mbedtls_entro...
#include <mbedtls/error.h>        // for mbedtls_ssl_conf_authmode, mbedtls_...
#include <mbedtls/net_sockets.h>  // for mbedtls_net_connect, mbedtls_net_free
#include <mbedtls/ssl.h>          // for mbedtls_ssl_conf_authmode, mbedtls_...
#include <cstring>                // for strlen, NULL
#include <stdexcept>              // for runtime_error
#include <esp_log.h>
#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "BellUtils.h"   // for BELL_SLEEP_MS
#include "X509Bundle.h"  // for shouldVerify, attach

/**
 * Platform TLSSocket implementation for the mbedtls
 */
bell::TLSSocket::TLSSocket() : isClosed(false) {
    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    if (bell::X509Bundle::shouldVerify()) {
        bell::X509Bundle::attach(&conf);
    }

    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    const char *pers = "euphonium";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char *>(pers),
                                    strlen(pers));
    if (ret != 0) {
        ESP_LOGE("TLSSocket", "Failed to seed DRBG");
        throw std::runtime_error("Failed to seed DRBG");
    }
}

void bell::TLSSocket::open(const std::string &hostUrl, uint16_t port) {
    int ret =
            mbedtls_net_connect(&server_fd, hostUrl.c_str(),
                                std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        ESP_LOGE("TLSSocket", "Connection failed for %s:%d with error %d\n",
                 hostUrl.c_str(), port, ret);
#ifdef ESP_PLATFORM
        close();
        return;
#else
        throw std::runtime_error("Connection failed");
#endif
    }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ESP_LOGE("TLSSocket", "SSL config setup failed: %d\n", ret);
#ifdef ESP_PLATFORM
        close();
        return;
#else
        throw std::runtime_error("SSL configuration failed");
#endif
    }

    if (bell::X509Bundle::shouldVerify()) {
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    //  mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_setup(&ssl, &conf);

    if ((ret = mbedtls_ssl_set_hostname(&ssl, hostUrl.c_str())) != 0) {
#ifdef ESP_PLATFORM
        ESP_LOGE("TLSSocket", "Failed to set SSL hostname: %d", ret);
        close();
        return;
#else
        throw std::runtime_error("Failed to set SSL hostname");
#endif
    }

    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv,
                        NULL);

    // Retry the handshake a limited number of times
    const int maxRetries = 5;
    int retries = maxRetries;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE("TLSSocket", "SSL handshake failed with error %d\n", ret);
#ifdef ESP_PLATFORM
            close();
            return;
#else
            throw std::runtime_error("SSL handshake failed");
#endif
        }
        if (--retries <= 0) {
            ESP_LOGE("TLSSocket", "SSL handshake retry limit reached");
#ifdef ESP_PLATFORM
            close();
            return;
#else
            throw std::runtime_error("SSL handshake retries exhausted");
#endif
        }
        BELL_SLEEP_MS(10);  // Delay between retries
    }
    isClosed = false;
}

ssize_t bell::TLSSocket::read(uint8_t *buf, size_t len) {
    if (isClosed || len == 0) {
        return -1;
    }
    int ret;
    do {
        ret = mbedtls_ssl_read(&ssl, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret < 0) {
        char error_buf[100];
        mbedtls_strerror(ret, error_buf, sizeof(error_buf));
        ESP_LOGE("TLSSocket", "Error: %s\n", error_buf);
        ESP_LOGE("TLSSocket", "Read error with code %x", ret);
        close();  //isClosed = true;
    } else
        return static_cast<ssize_t>(ret);
    return -1;
}

ssize_t bell::TLSSocket::write(const uint8_t *buf, size_t len) {
    int ret;
    do {
        ret = mbedtls_ssl_write(&ssl, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret < 0) {
        ESP_LOGE("TLSSocket", "Write error with code %x\n", ret);
        close();  //isClosed = true;
    }
    return static_cast<ssize_t>(ret);
}

size_t bell::TLSSocket::poll() {
    return mbedtls_ssl_get_bytes_avail(&ssl);
}

bool bell::TLSSocket::isOpen() {
    return !isClosed;
}

void bell::TLSSocket::close() {
    if (!isClosed) {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_net_free(&server_fd);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        isClosed = true;
    }
}
