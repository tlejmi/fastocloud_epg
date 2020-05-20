/*  Copyright (C) 2014-2020 FastoGT. All right reserved.
    This file is part of fastocloud.
    fastocloud is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    fastocloud is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with fastocloud.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "https_client.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <common/net/isocket.h>
#include <common/net/socket_tcp.h>

namespace fastocloud {
namespace server {

class SocketTls : public common::net::ISocketFd {
 public:
  explicit SocketTls(const common::net::HostAndPort& host) : hs_(host), ssl_(nullptr) {
    SSL_library_init();
    SSLeay_add_ssl_algorithms();
    SSL_load_error_strings();
  }

  common::net::socket_descr_t GetFd() const override { return hs_.GetFd(); }

  void SetFd(common::net::socket_descr_t fd) override { hs_.SetFd(fd); }

  common::ErrnoError Connect(struct timeval* tv = nullptr) WARN_UNUSED_RESULT {
    common::net::ClientSocketTcp hs(hs_.GetHost());
    common::ErrnoError err = hs.Connect(tv);
    if (err) {
      return err;
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    const SSL_METHOD* method = TLSv1_2_client_method();
#else
    const SSL_METHOD* method = TLS_client_method();
#endif
    if (!method) {
      hs.Disconnect();
      return common::make_errno_error_inval();
    }

    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
      hs.Disconnect();
      return common::make_errno_error_inval();
    }

    SSL* ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    ctx = nullptr;
    if (!ssl) {
      SSL_free(ssl);
      hs.Disconnect();
      return common::make_errno_error_inval();
    }

    SSL_set_fd(ssl, hs.GetFd());
    int e = SSL_connect(ssl);
    if (e < 0) {
      int err = SSL_get_error(ssl, e);
      char* str = ERR_error_string(err, nullptr);
      SSL_free(ssl);
      hs.Disconnect();
      return common::make_errno_error(str, EINTR);
    }

    hs_.SetInfo(hs.GetInfo());
    ssl_ = ssl;
    return common::ErrnoError();
  }

  common::ErrnoError Disconnect() WARN_UNUSED_RESULT { return Close(); }

  bool IsConnected() const { return hs_.IsConnected(); }

  common::net::HostAndPort GetHost() const { return hs_.GetHost(); }

  bool IsValid() const override { return hs_.IsValid(); }

 private:
  common::ErrnoError WriteImpl(const void* data, size_t size, size_t* nwrite_out) override {
    int len = SSL_write(ssl_, data, size);
    if (len < 0) {
      int err = SSL_get_error(ssl_, len);
      char* str = ERR_error_string(err, nullptr);
      return common::make_errno_error(str, EINTR);
    }

    *nwrite_out = len;
    return common::ErrnoError();
  }

  common::ErrnoError ReadImpl(void* out_data, size_t max_size, size_t* nread_out) override {
    int len = SSL_read(ssl_, out_data, max_size);
    if (len < 0) {
      int err = SSL_get_error(ssl_, len);
      char* str = ERR_error_string(err, nullptr);
      return common::make_errno_error(str, EINTR);
    }

    *nread_out = len;
    return common::ErrnoError();
  }

  common::ErrnoError CloseImpl() override {
    if (ssl_) {
      SSL_free(ssl_);
      ssl_ = nullptr;
    }

    return hs_.Close();
  }

  common::net::ClientSocketTcp hs_;
  SSL* ssl_;
};

HttpsClient::HttpsClient(const common::net::HostAndPort& host) : base_class(new SocketTls(host)) {}

common::ErrnoError HttpsClient::Connect(struct timeval* tv) {
  SocketTls* sock = static_cast<SocketTls*>(GetSocket());
  return sock->Connect(tv);
}

bool HttpsClient::IsConnected() const {
  SocketTls* sock = static_cast<SocketTls*>(GetSocket());
  return sock->IsConnected();
}

common::ErrnoError HttpsClient::Disconnect() {
  SocketTls* sock = static_cast<SocketTls*>(GetSocket());
  return sock->Disconnect();
}

common::net::HostAndPort HttpsClient::GetHost() const {
  SocketTls* sock = static_cast<SocketTls*>(GetSocket());
  return sock->GetHost();
}

common::ErrnoError HttpsClient::SendFile(descriptor_t file_fd, size_t file_size) {
  SocketTls* sock = static_cast<SocketTls*>(GetSocket());
  return sock->SendFile(file_fd, file_size);
}

}  // namespace server
}  // namespace fastocloud
