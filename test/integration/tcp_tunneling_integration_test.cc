#include <chrono>
#include <memory>

#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/core/v3/proxy_protocol.pb.h"
#include "envoy/extensions/access_loggers/file/v3/file.pb.h"
#include "envoy/extensions/filters/network/tcp_proxy/v3/tcp_proxy.pb.h"
#include "envoy/extensions/upstreams/http/tcp/v3/tcp_connection_pool.pb.h"

#include "test/integration/filters/add_header_filter.pb.h"
#include "test/integration/filters/stop_and_continue_filter_config.pb.h"
#include "test/integration/http_integration.h"
#include "test/integration/http_protocol_integration.h"
#include "test/integration/tcp_tunneling_integration.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace {

// Terminating CONNECT and sending raw TCP upstream.
class ConnectTerminationIntegrationTest : public HttpProtocolIntegrationTest {
public:
  ConnectTerminationIntegrationTest() { enableHalfClose(true); }

  void initialize() override {
    useAccessLog("%UPSTREAM_WIRE_BYTES_SENT% %UPSTREAM_WIRE_BYTES_RECEIVED% "
                 "%UPSTREAM_HEADER_BYTES_SENT% %UPSTREAM_HEADER_BYTES_RECEIVED% %ACCESS_LOG_TYPE%");
    config_helper_.addConfigModifier(
        [&](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) {
          hcm.mutable_delayed_close_timeout()->set_seconds(1);
          ConfigHelper::setConnectConfig(hcm, !terminate_via_cluster_config_, allow_post_,
                                         downstream_protocol_ == Http::CodecType::HTTP3);

          if (enable_timeout_) {
            hcm.mutable_stream_idle_timeout()->set_seconds(0);
            hcm.mutable_stream_idle_timeout()->set_nanos(200 * 1000 * 1000);
          }
          if (exact_match_) {
            auto* route_config = hcm.mutable_route_config();
            ASSERT_EQ(1, route_config->virtual_hosts_size());
            route_config->mutable_virtual_hosts(0)->clear_domains();
            route_config->mutable_virtual_hosts(0)->add_domains("foo.lyft.com:80");
          }
        });
    HttpIntegrationTest::initialize();
  }

  void setUpConnection() {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    auto encoder_decoder = codec_client_->startRequest(connect_headers_);
    request_encoder_ = &encoder_decoder.first;
    response_ = std::move(encoder_decoder.second);
    ASSERT_TRUE(fake_upstreams_[0]->waitForRawConnection(fake_raw_upstream_connection_));
    response_->waitForHeaders();
  }

  void sendBidirectionalData(const char* downstream_send_data = "hello",
                             const char* upstream_received_data = "hello",
                             const char* upstream_send_data = "there!",
                             const char* downstream_received_data = "there!") {
    // Send some data upstream.
    codec_client_->sendData(*request_encoder_, downstream_send_data, false);
    ASSERT_TRUE(fake_raw_upstream_connection_->waitForData(
        FakeRawConnection::waitForInexactMatch(upstream_received_data)));

    // Send some data downstream.
    ASSERT_TRUE(fake_raw_upstream_connection_->write(upstream_send_data));
    response_->waitForBodyData(strlen(downstream_received_data));
    EXPECT_EQ(downstream_received_data, response_->body());
  }

  Http::TestRequestHeaderMapImpl connect_headers_{{":method", "CONNECT"},
                                                  {":authority", "foo.lyft.com:80"}};

  void sendBidirectionalDataAndCleanShutdown() {
    sendBidirectionalData("hello", "hello", "there!", "there!");
    // Send a second set of data to make sure for example headers are only sent once.
    sendBidirectionalData(",bye", "hello,bye", "ack", "there!ack");

    // Send an end stream. This should result in half close upstream.
    codec_client_->sendData(*request_encoder_, "", true);
    ASSERT_TRUE(fake_raw_upstream_connection_->waitForHalfClose());

    // Now send a FIN from upstream. This should result in clean shutdown downstream.
    ASSERT_TRUE(fake_raw_upstream_connection_->close());
    if (downstream_protocol_ == Http::CodecType::HTTP1) {
      ASSERT_TRUE(codec_client_->waitForDisconnect());
    } else {
      ASSERT_TRUE(response_->waitForEndStream());
      ASSERT_FALSE(response_->reset());
    }

    // expected_header_bytes_* is 0 as we do not use proxy protocol.
    // expected_wire_bytes_sent is 9 as the Envoy sends "hello,bye".
    // expected_wire_bytes_received is 9 as the Upstream sends "there!ack".
    const int expected_wire_bytes_sent = 9;
    const int expected_wire_bytes_received = 9;
    const int expected_header_bytes_sent = 0;
    const int expected_header_bytes_received = 0;
    checkAccessLogOutput(expected_wire_bytes_sent, expected_wire_bytes_received,
                         expected_header_bytes_sent, expected_header_bytes_received);
    ++access_log_entry_;
  }

  void checkAccessLogOutput(int expected_wire_bytes_sent, int expected_wire_bytes_received,
                            int expected_header_bytes_sent, int expected_header_bytes_received) {
    std::string log = waitForAccessLog(access_log_name_, access_log_entry_);
    std::vector<std::string> log_entries = absl::StrSplit(log, ' ');
    const int wire_bytes_sent = std::stoi(log_entries[0]),
              wire_bytes_received = std::stoi(log_entries[1]),
              header_bytes_sent = std::stoi(log_entries[2]),
              header_bytes_received = std::stoi(log_entries[3]);
    EXPECT_EQ(wire_bytes_sent, expected_wire_bytes_sent);
    EXPECT_EQ(wire_bytes_received, expected_wire_bytes_received);
    EXPECT_EQ(header_bytes_sent, expected_header_bytes_sent);
    EXPECT_EQ(header_bytes_received, expected_header_bytes_received);
  }

  FakeRawConnectionPtr fake_raw_upstream_connection_;
  IntegrationStreamDecoderPtr response_;
  bool terminate_via_cluster_config_{};
  bool enable_timeout_{};
  bool exact_match_{};
  bool allow_post_{};
  uint32_t access_log_entry_{0};
};

TEST_P(ConnectTerminationIntegrationTest, ManyStreams) {
  if (downstream_protocol_ == Http::CodecType::HTTP1) {
    // Resetting an individual stream requires HTTP/2 or later.
    return;
  }
  autonomous_upstream_ = true; // Sending raw HTTP/1.1
  setUpstreamProtocol(Http::CodecType::HTTP1);
  initialize();

  auto upstream = reinterpret_cast<AutonomousUpstream*>(fake_upstreams_.front().get());
  upstream->setResponseHeaders(std::make_unique<Http::TestResponseHeaderMapImpl>(
      Http::TestResponseHeaderMapImpl({{":status", "200"}, {"content-length", "0"}})));
  upstream->setResponseTrailers(std::make_unique<Http::TestResponseTrailerMapImpl>(Http::TestResponseTrailerMapImpl({{"Trailer-Header", "Trailer-Value"}})));
  upstream->setResponseBody("");
  std::string response_body = "HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n";
  codec_client_ = makeHttpConnection(lookupPort("http"));

  std::vector<Http::RequestEncoder*> encoders;
  std::vector<IntegrationStreamDecoderPtr> responses;
  const int num_loops = 50;

  // Do 2x loops and reset half the streams to fuzz lifetime issues
  for (int i = 0; i < num_loops * 2; ++i) {
    auto encoder_decoder = codec_client_->startRequest(connect_headers_);
    if (i % 2 == 0) {
      codec_client_->sendReset(encoder_decoder.first);
    } else {
      encoders.push_back(&encoder_decoder.first);
      responses.push_back(std::move(encoder_decoder.second));
    }
  }

  // Finish up the non-reset streams. The autonomous upstream will send the response.
  for (int i = 0; i < num_loops; ++i) {
    // Send some data upstream.
    codec_client_->sendData(*encoders[i], "GET / HTTP/1.1\r\nHost: www.google.com\r\n\r\n", false);
    responses[i]->waitForBodyData(response_body.length());
  }

  codec_client_->close();
}

INSTANTIATE_TEST_SUITE_P(HttpAndIpVersions, ConnectTerminationIntegrationTest,
                         testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams(
                             {Http::CodecType::HTTP1, Http::CodecType::HTTP2,
                              Http::CodecType::HTTP3},
                             {Http::CodecType::HTTP1})),
                         HttpProtocolIntegrationTest::protocolTestParamsToString);

using Params = std::tuple<Network::Address::IpVersion, Http::CodecType>;

// Test with proxy protocol headers.
class ProxyProtocolConnectTerminationIntegrationTest : public ConnectTerminationIntegrationTest {
protected:
  void initialize() override {
    useAccessLog("%UPSTREAM_WIRE_BYTES_SENT% %UPSTREAM_WIRE_BYTES_RECEIVED% "
                 "%UPSTREAM_HEADER_BYTES_SENT% %UPSTREAM_HEADER_BYTES_RECEIVED%");
    config_helper_.addConfigModifier(
        [&](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) {
          hcm.mutable_delayed_close_timeout()->set_seconds(1);
          ConfigHelper::setConnectConfig(hcm, !terminate_via_cluster_config_, allow_post_,
                                         downstream_protocol_ == Http::CodecType::HTTP3,
                                         proxy_protocol_version_);
        });
    HttpIntegrationTest::initialize();
  }

  envoy::config::core::v3::ProxyProtocolConfig::Version proxy_protocol_version_{
      envoy::config::core::v3::ProxyProtocolConfig::V1};

  void sendBidirectionalDataAndCleanShutdownWithProxyProtocol() {
    sendBidirectionalData("hello", "hello", "there!", "there!");
    // Send a second set of data to make sure for example headers are only sent once.
    sendBidirectionalData(",bye", "hello,bye", "ack", "there!ack");

    // Send an end stream. This should result in half close upstream.
    codec_client_->sendData(*request_encoder_, "", true);
    ASSERT_TRUE(fake_raw_upstream_connection_->waitForHalfClose());

    // Now send a FIN from upstream. This should result in clean shutdown downstream.
    ASSERT_TRUE(fake_raw_upstream_connection_->close());
    if (downstream_protocol_ == Http::CodecType::HTTP1) {
      ASSERT_TRUE(codec_client_->waitForDisconnect());
    } else {
      ASSERT_TRUE(response_->waitForEndStream());
      ASSERT_FALSE(response_->reset());
    }
    const bool is_ipv4 = version_ == Network::Address::IpVersion::v4;

    // expected_header_bytes_sent is based on the proxy protocol header sent to
    //  the upstream.
    // expected_header_bytes_received is zero since Envoy initiates the proxy
    // protocol.
    // expected_wire_bytes_sent changes depending on the proxy protocol header
    // size which varies based on ip version. Envoy also sends "hello,bye".
    // expected_wire_bytes_received is 9 as the Upstream sends "there!ack".
    int expected_wire_bytes_sent;
    int expected_header_bytes_sent;
    if (proxy_protocol_version_ == envoy::config::core::v3::ProxyProtocolConfig::V1) {
      expected_wire_bytes_sent = is_ipv4 ? 53 : 41;
      expected_header_bytes_sent = is_ipv4 ? 44 : 32;
    } else {
      expected_wire_bytes_sent = is_ipv4 ? 37 : 61;
      expected_header_bytes_sent = is_ipv4 ? 28 : 52;
    }
    const int expected_wire_bytes_received = 9;
    const int expected_header_bytes_received = 0;
    checkAccessLogOutput(expected_wire_bytes_sent, expected_wire_bytes_received,
                         expected_header_bytes_sent, expected_header_bytes_received);
  }
};
}}