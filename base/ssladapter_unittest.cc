/*
 *  Copyright 2014 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <string>

#include "webrtc/base/gunit.h"
#include "webrtc/base/ipaddress.h"
#include "webrtc/base/socketstream.h"
#include "webrtc/base/ssladapter.h"
#include "webrtc/base/sslstreamadapter.h"
#include "webrtc/base/stream.h"
#include "webrtc/base/virtualsocketserver.h"

static const int kTimeout = 5000;

static rtc::AsyncSocket* CreateSocket(const rtc::SSLMode& ssl_mode) {
  rtc::SocketAddress address(rtc::IPAddress(INADDR_ANY), 0);

  rtc::AsyncSocket* socket = rtc::Thread::Current()->
      socketserver()->CreateAsyncSocket(
      address.family(), (ssl_mode == rtc::SSL_MODE_DTLS) ?
      SOCK_DGRAM : SOCK_STREAM);
  socket->Bind(address);

  return socket;
}

static std::string GetSSLProtocolName(const rtc::SSLMode& ssl_mode) {
  return (ssl_mode == rtc::SSL_MODE_DTLS) ? "DTLS" : "TLS";
}

class SSLAdapterTestDummyClient : public sigslot::has_slots<> {
 public:
  explicit SSLAdapterTestDummyClient(const rtc::SSLMode& ssl_mode)
      : ssl_mode_(ssl_mode) {
    rtc::AsyncSocket* socket = CreateSocket(ssl_mode_);

    ssl_adapter_.reset(rtc::SSLAdapter::Create(socket));

    // Ignore any certificate errors for the purpose of testing.
    // Note: We do this only because we don't have a real certificate.
    // NEVER USE THIS IN PRODUCTION CODE!
    ssl_adapter_->set_ignore_bad_cert(true);

    ssl_adapter_->SignalReadEvent.connect(this,
        &SSLAdapterTestDummyClient::OnSSLAdapterReadEvent);
    ssl_adapter_->SignalCloseEvent.connect(this,
        &SSLAdapterTestDummyClient::OnSSLAdapterCloseEvent);
  }

  rtc::AsyncSocket::ConnState GetState() const {
    return ssl_adapter_->GetState();
  }

  const std::string& GetReceivedData() const {
    return data_;
  }

  int Connect(const std::string& hostname, const rtc::SocketAddress& address) {
    LOG(LS_INFO) << "Starting " << GetSSLProtocolName(ssl_mode_)
        << " handshake with " << hostname;

    if (ssl_adapter_->StartSSL(hostname.c_str(), false) != 0) {
      return -1;
    }

    LOG(LS_INFO) << "Initiating connection with " << address;

    return ssl_adapter_->Connect(address);
  }

  int Close() {
    return ssl_adapter_->Close();
  }

  int Send(const std::string& message) {
    LOG(LS_INFO) << "Client sending '" << message << "'";

    return ssl_adapter_->Send(message.data(), message.length());
  }

  void OnSSLAdapterReadEvent(rtc::AsyncSocket* socket) {
    char buffer[4096] = "";

    // Read data received from the server and store it in our internal buffer.
    int read = socket->Recv(buffer, sizeof(buffer) - 1);
    if (read != -1) {
      buffer[read] = '\0';

      LOG(LS_INFO) << "Client received '" << buffer << "'";

      data_ += buffer;
    }
  }

  void OnSSLAdapterCloseEvent(rtc::AsyncSocket* socket, int error) {
    // OpenSSLAdapter signals handshake failure with a close event, but without
    // closing the socket! Let's close the socket here. This way GetState() can
    // return CS_CLOSED after failure.
    if (socket->GetState() != rtc::AsyncSocket::CS_CLOSED) {
      socket->Close();
    }
  }

 private:
  const rtc::SSLMode ssl_mode_;

  rtc::scoped_ptr<rtc::SSLAdapter> ssl_adapter_;

  std::string data_;
};

class SSLAdapterTestDummyServer : public sigslot::has_slots<> {
 public:
  explicit SSLAdapterTestDummyServer(const rtc::SSLMode& ssl_mode)
      : ssl_mode_(ssl_mode) {
    // Generate a key pair and a certificate for this host.
    ssl_identity_.reset(rtc::SSLIdentity::Generate(GetHostname()));

    server_socket_.reset(CreateSocket(ssl_mode_));

    server_socket_->SignalReadEvent.connect(this,
        &SSLAdapterTestDummyServer::OnServerSocketReadEvent);

    server_socket_->Listen(1);

    LOG(LS_INFO) << ((ssl_mode_ == rtc::SSL_MODE_DTLS) ? "UDP" : "TCP")
        << " server listening on " << server_socket_->GetLocalAddress();
  }

  rtc::SocketAddress GetAddress() const {
    return server_socket_->GetLocalAddress();
  }

  std::string GetHostname() const {
    // Since we don't have a real certificate anyway, the value here doesn't
    // really matter.
    return "example.com";
  }

  const std::string& GetReceivedData() const {
    return data_;
  }

  int Send(const std::string& message) {
    if (ssl_stream_adapter_ == NULL
        || ssl_stream_adapter_->GetState() != rtc::SS_OPEN) {
      // No connection yet.
      return -1;
    }

    LOG(LS_INFO) << "Server sending '" << message << "'";

    size_t written;
    int error;

    rtc::StreamResult r = ssl_stream_adapter_->Write(message.data(),
        message.length(), &written, &error);
    if (r == rtc::SR_SUCCESS) {
      return written;
    } else {
      return -1;
    }
  }

  void OnServerSocketReadEvent(rtc::AsyncSocket* socket) {
    if (ssl_stream_adapter_ != NULL) {
      // Only a single connection is supported.
      return;
    }

    rtc::SocketAddress address;
    rtc::AsyncSocket* new_socket = socket->Accept(&address);
    rtc::SocketStream* stream = new rtc::SocketStream(new_socket);

    ssl_stream_adapter_.reset(rtc::SSLStreamAdapter::Create(stream));
    ssl_stream_adapter_->SetServerRole();

    // SSLStreamAdapter is normally used for peer-to-peer communication, but
    // here we're testing communication between a client and a server
    // (e.g. a WebRTC-based application and an RFC 5766 TURN server), where
    // clients are not required to provide a certificate during handshake.
    // Accordingly, we must disable client authentication here.
    ssl_stream_adapter_->set_client_auth_enabled(false);

    ssl_stream_adapter_->SetIdentity(ssl_identity_->GetReference());

    // Set a bogus peer certificate digest.
    unsigned char digest[20];
    size_t digest_len = sizeof(digest);
    ssl_stream_adapter_->SetPeerCertificateDigest(rtc::DIGEST_SHA_1, digest,
        digest_len);

    ssl_stream_adapter_->StartSSLWithPeer();

    ssl_stream_adapter_->SignalEvent.connect(this,
        &SSLAdapterTestDummyServer::OnSSLStreamAdapterEvent);
  }

  void OnSSLStreamAdapterEvent(rtc::StreamInterface* stream, int sig, int err) {
    if (sig & rtc::SE_READ) {
      char buffer[4096] = "";

      size_t read;
      int error;

      // Read data received from the client and store it in our internal
      // buffer.
      rtc::StreamResult r = stream->Read(buffer,
          sizeof(buffer) - 1, &read, &error);
      if (r == rtc::SR_SUCCESS) {
        buffer[read] = '\0';

        LOG(LS_INFO) << "Server received '" << buffer << "'";

        data_ += buffer;
      }
    }
  }

 private:
  const rtc::SSLMode ssl_mode_;

  rtc::scoped_ptr<rtc::AsyncSocket> server_socket_;
  rtc::scoped_ptr<rtc::SSLStreamAdapter> ssl_stream_adapter_;

  rtc::scoped_ptr<rtc::SSLIdentity> ssl_identity_;

  std::string data_;
};

class SSLAdapterTestBase : public testing::Test,
                           public sigslot::has_slots<> {
 public:
  explicit SSLAdapterTestBase(const rtc::SSLMode& ssl_mode)
      : ssl_mode_(ssl_mode),
        ss_scope_(new rtc::VirtualSocketServer(NULL)),
        server_(new SSLAdapterTestDummyServer(ssl_mode_)),
        client_(new SSLAdapterTestDummyClient(ssl_mode_)),
        handshake_wait_(kTimeout) {
  }

  static void SetUpTestCase() {
    rtc::InitializeSSL();
  }

  static void TearDownTestCase() {
    rtc::CleanupSSL();
  }

  void SetHandshakeWait(int wait) {
    handshake_wait_ = wait;
  }

  void TestHandshake(bool expect_success) {
    int rv;

    // The initial state is CS_CLOSED
    ASSERT_EQ(rtc::AsyncSocket::CS_CLOSED, client_->GetState());

    rv = client_->Connect(server_->GetHostname(), server_->GetAddress());
    ASSERT_EQ(0, rv);

    // Now the state should be CS_CONNECTING
    ASSERT_EQ(rtc::AsyncSocket::CS_CONNECTING, client_->GetState());

    if (expect_success) {
      // If expecting success, the client should end up in the CS_CONNECTED
      // state after handshake.
      EXPECT_EQ_WAIT(rtc::AsyncSocket::CS_CONNECTED, client_->GetState(),
          handshake_wait_);

      LOG(LS_INFO) << GetSSLProtocolName(ssl_mode_) << " handshake complete.";

    } else {
      // On handshake failure the client should end up in the CS_CLOSED state.
      EXPECT_EQ_WAIT(rtc::AsyncSocket::CS_CLOSED, client_->GetState(),
          handshake_wait_);

      LOG(LS_INFO) << GetSSLProtocolName(ssl_mode_) << " handshake failed.";
    }
  }

  void TestTransfer(const std::string& message) {
    int rv;

    rv = client_->Send(message);
    ASSERT_EQ(static_cast<int>(message.length()), rv);

    // The server should have received the client's message.
    EXPECT_EQ_WAIT(message, server_->GetReceivedData(), kTimeout);

    rv = server_->Send(message);
    ASSERT_EQ(static_cast<int>(message.length()), rv);

    // The client should have received the server's message.
    EXPECT_EQ_WAIT(message, client_->GetReceivedData(), kTimeout);

    LOG(LS_INFO) << "Transfer complete.";
  }

 private:
  const rtc::SSLMode ssl_mode_;

  const rtc::SocketServerScope ss_scope_;

  rtc::scoped_ptr<SSLAdapterTestDummyServer> server_;
  rtc::scoped_ptr<SSLAdapterTestDummyClient> client_;

  int handshake_wait_;
};

class SSLAdapterTestTLS : public SSLAdapterTestBase {
 public:
  SSLAdapterTestTLS() : SSLAdapterTestBase(rtc::SSL_MODE_TLS) {}
};


#if SSL_USE_OPENSSL

// Basic tests: TLS

// Test that handshake works
TEST_F(SSLAdapterTestTLS, TestTLSConnect) {
  TestHandshake(true);
}

// Test transfer between client and server
TEST_F(SSLAdapterTestTLS, TestTLSTransfer) {
  TestHandshake(true);
  TestTransfer("Hello, world!");
}

#endif  // SSL_USE_OPENSSL

