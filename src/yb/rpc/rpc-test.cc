// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/rpc/rpc-test-base.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <boost/ptr_container/ptr_vector.hpp>
#include <gtest/gtest.h>

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"

#include "yb/rpc/secure_stream.h"
#include "yb/rpc/serialization.h"
#include "yb/rpc/tcp_stream.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/util/countdown_latch.h"
#include "yb/util/env.h"
#include "yb/util/test_util.h"

METRIC_DECLARE_histogram(handler_latency_yb_rpc_test_CalculatorService_Sleep);
METRIC_DECLARE_histogram(rpc_incoming_queue_time);

DEFINE_int32(rpc_test_connection_keepalive_num_iterations, 1,
  "Number of iterations in TestRpc.TestConnectionKeepalive");

DECLARE_uint64(rpc_connection_timeout_ms);
DECLARE_int32(num_connections_to_server);
DECLARE_bool(enable_rpc_keepalive);

using namespace std::chrono_literals;
using std::string;
using std::shared_ptr;
using std::unordered_map;

namespace yb {
namespace rpc {

class TestRpc : public RpcTestBase {
 public:
  void CheckServerMessengerConnections(size_t num_connections) {
    ReactorMetrics metrics;
    ASSERT_OK(server_messenger()->TEST_GetReactorMetrics(0, &metrics));
    ASSERT_EQ(metrics.num_server_connections_, num_connections)
        << "Server should have " << num_connections << " server connection(s)";
    ASSERT_EQ(metrics.num_client_connections_, 0) << "Server should have 0 client connections";
  }

  void CheckClientMessengerConnections(Messenger* messenger, size_t num_connections) {
    ReactorMetrics metrics;
    ASSERT_OK(messenger->TEST_GetReactorMetrics(0, &metrics));
    ASSERT_EQ(metrics.num_server_connections_, 0) << "Client should have 0 server connections";
    ASSERT_EQ(metrics.num_client_connections_, num_connections)
        << "Client should have " << num_connections << " client connection(s)";
  }
};

namespace {

// Used only to test parsing.
const uint16_t kDefaultPort = 80;

void CheckParseEndpoint(const std::string& input, std::string expected = std::string()) {
  if (expected.empty()) {
    expected = input;
  }
  auto endpoint = ParseEndpoint(input, kDefaultPort);
  ASSERT_TRUE(endpoint.ok()) << "input: " << input << ", status: " << endpoint.status().ToString();
  ASSERT_EQ(expected, yb::ToString(*endpoint));
}

} // namespace

TEST_F(TestRpc, Endpoint) {
  Endpoint addr1, addr2;
  addr1.port(1000);
  addr2.port(2000);
  ASSERT_TRUE(addr1 < addr2);
  ASSERT_FALSE(addr2 < addr1);
  ASSERT_EQ(1000, addr1.port());
  ASSERT_EQ(2000, addr2.port());
  ASSERT_EQ(string("0.0.0.0:1000"), yb::ToString(addr1));
  ASSERT_EQ(string("0.0.0.0:2000"), yb::ToString(addr2));
  Endpoint addr3(addr1);
  ASSERT_EQ(string("0.0.0.0:1000"), yb::ToString(addr3));

  CheckParseEndpoint("127.0.0.1", "127.0.0.1:80");
  CheckParseEndpoint("192.168.0.1:123");
  CheckParseEndpoint("[10.8.0.137]", "10.8.0.137:80");
  CheckParseEndpoint("[10.8.0.137]:123", "10.8.0.137:123");

  CheckParseEndpoint("fe80::1", "[fe80::1]:80");
  CheckParseEndpoint("[fe80::1]", "[fe80::1]:80");
  CheckParseEndpoint("fe80::1:123", "[fe80::1:123]:80");
  CheckParseEndpoint("[fe80::1]:123");

  ASSERT_NOK(ParseEndpoint("[127.0.0.1]:", kDefaultPort));
  ASSERT_NOK(ParseEndpoint("[127.0.0.1:123", kDefaultPort));
  ASSERT_NOK(ParseEndpoint("fe80::1:12345", kDefaultPort));
}

TEST_F(TestRpc, TestMessengerCreateDestroy) {
  std::unique_ptr<Messenger> messenger = CreateMessenger("TestCreateDestroy");
  LOG(INFO) << "started messenger " << messenger->name();
  messenger->Shutdown();
}

// Test starting and stopping a messenger. This is a regression
// test for a segfault seen in early versions of the RPC code,
// in which shutting down the acceptor would trigger an assert,
// making our tests flaky.
TEST_F(TestRpc, TestAcceptorPoolStartStop) {
  int n_iters = AllowSlowTests() ? 100 : 5;
  for (int i = 0; i < n_iters; i++) {
    std::unique_ptr<Messenger> messenger = CreateMessenger("TestAcceptorPoolStartStop");
    Endpoint bound_endpoint;
    ASSERT_OK(messenger->ListenAddress(
        CreateConnectionContextFactory<YBInboundConnectionContext>(),
        Endpoint(), &bound_endpoint));
    ASSERT_OK(messenger->StartAcceptor());
    ASSERT_NE(0, bound_endpoint.port());
    messenger->Shutdown();
  }
}

// Test making successful RPC calls.
TEST_F(TestRpc, TestCall) {
  // Set up server.
  HostPort server_addr;
  StartTestServer(&server_addr);

  // Set up client.
  LOG(INFO) << "Connecting to " << server_addr;
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  for (int i = 0; i < 10; i++) {
    ASSERT_OK(DoTestSyncCall(&p, CalculatorServiceMethods::AddMethod()));
  }
}

TEST_F(TestRpc, BigTimeout) {
  // Set up server.
  TestServerOptions options;
  options.messenger_options.keep_alive_timeout = 60s;
  HostPort server_addr;
  StartTestServer(&server_addr, options);

  // Set up client.
  LOG(INFO) << "Connecting to " << server_addr;
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  for (int i = 0; i < 10; i++) {
    ASSERT_OK(DoTestSyncCall(&p, CalculatorServiceMethods::AddMethod()));
  }

  LOG(INFO) << "Calls OK";

  auto call_consumption = MemTracker::GetRootTracker()->FindChild("Call")->consumption();
  ASSERT_EQ(call_consumption, 0);
}

// Test that connecting to an invalid server properly throws an error.
TEST_F(TestRpc, TestCallToBadServer) {
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  HostPort addr;
  Proxy p(client_messenger.get(), addr);

  // Loop a few calls to make sure that we properly set up and tear down
  // the connections.
  for (int i = 0; i < 5; i++) {
    Status s = DoTestSyncCall(&p, CalculatorServiceMethods::AddMethod());
    LOG(INFO) << "Status: " << s.ToString();
    ASSERT_TRUE(s.IsRemoteError()) << "unexpected status: " << s.ToString();
  }
}

// Test that RPC calls can be failed with an error status on the server.
TEST_F(TestRpc, TestInvalidMethodCall) {
  // Set up server.
  HostPort server_addr;
  StartTestServer(&server_addr);

  // Set up client.
  LOG(INFO) << "Connecting to " << server_addr;
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  // Call the method which fails.
  static RemoteMethod method(
      rpc_test::CalculatorServiceIf::static_service_name(), "ThisMethodDoesNotExist");
  Status s = DoTestSyncCall(&p, &method);
  ASSERT_TRUE(s.IsRemoteError()) << "unexpected status: " << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "bad method");
}

// Test that the error message returned when connecting to the wrong service
// is reasonable.
TEST_F(TestRpc, TestWrongService) {
  // Set up server.
  HostPort server_addr;
  StartTestServer(&server_addr);

  // Set up client with the wrong service name.
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  // Call the method which fails.
  static RemoteMethod method("WrongServiceName", "ThisMethodDoesNotExist");
  Status s = DoTestSyncCall(&p, &method);
  auto message = s.ToString();
  ASSERT_TRUE(s.IsRemoteError()) << "unexpected status: " << message;
  // Remote errors always contain file name and line number.
  ASSERT_STR_CONTAINS(message, "Remote error (");
  ASSERT_STR_CONTAINS(message, "): Service unavailable (");
  ASSERT_STR_CONTAINS(message, "): Service WrongServiceName not registered on TestServer");
}

namespace {

uint64_t GetOpenFileLimit() {
  struct rlimit limit;
  PCHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0);
  return limit.rlim_cur;
}

} // anonymous namespace

// Test that we can still make RPC connections even if many fds are in use.
// This is a regression test for KUDU-650.
TEST_F(TestRpc, TestHighFDs) {
  // This test can only run if ulimit is set high.
  const uint64_t kNumFakeFiles = 3500;
  const uint64_t kMinUlimit = kNumFakeFiles + 100;
  if (GetOpenFileLimit() < kMinUlimit) {
    LOG(INFO) << "Test skipped: must increase ulimit -n to at least " << kMinUlimit;
    return;
  }

  // Open a bunch of fds just to increase our fd count.
  std::vector<std::unique_ptr<RandomAccessFile>> fake_files;
  for (uint64_t i = 0; i < kNumFakeFiles; i++) {
    std::unique_ptr<RandomAccessFile> f;
    CHECK_OK(Env::Default()->NewRandomAccessFile("/dev/zero", &f));
    fake_files.emplace_back(f.release());
  }

  // Set up server and client, and verify we can make a successful call.
  HostPort server_addr;
  StartTestServer(&server_addr);
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);
  ASSERT_OK(DoTestSyncCall(&p, CalculatorServiceMethods::AddMethod()));
}

// Test that connections are kept alive by ScanIdleConnections between calls.
TEST_F(TestRpc, TestConnectionKeepalive) {
  google::FlagSaver saver;

  // Only run one reactor per messenger, so we can grab the metrics from that
  // one without having to check all.
  const auto kGcTimeout = 300ms;
  MessengerOptions messenger_options = { 1, kGcTimeout };
  TestServerOptions options;
  options.messenger_options = messenger_options;
  // RPC heartbeats shouldn't prevent idle connections from being GCed. To test that we set
  // rpc_connection_timeout less than kGcTimeout.
  FLAGS_rpc_connection_timeout_ms = MonoDelta(kGcTimeout).ToMilliseconds() / 2;
  FLAGS_enable_rpc_keepalive = true;

  // Set up server.
  HostPort server_addr;
  StartTestServer(&server_addr, options);
  for (int i = 0; i < FLAGS_rpc_test_connection_keepalive_num_iterations; ++i) {
    // Set up client.
    LOG(INFO) << "Connecting to " << server_addr;
    auto client_messenger = CreateAutoShutdownMessengerHolder("Client", messenger_options);
    Proxy p(client_messenger.get(), server_addr);

    ASSERT_OK(DoTestSyncCall(&p, CalculatorServiceMethods::AddMethod()));
    ASSERT_NO_FATALS(CheckServerMessengerConnections(1));
    ASSERT_NO_FATALS(CheckClientMessengerConnections(client_messenger.get(), 1));
    LOG(INFO) << "Connections are up";

    SleepFor(kGcTimeout / 2);

    LOG(INFO) << "Checking connections";
    ASSERT_NO_FATALS(CheckServerMessengerConnections(1));
    ASSERT_NO_FATALS(CheckClientMessengerConnections(client_messenger.get(), 1));

    SleepFor(kGcTimeout * 2);

    // After sleeping, the keepalive timer should have closed both sides of the connection.
    ASSERT_NO_FATALS(CheckServerMessengerConnections(0));
    ASSERT_NO_FATALS(CheckClientMessengerConnections(client_messenger.get(), 0));
  }
}

// Test that a call which takes longer than the keepalive time
// succeeds -- i.e that we don't consider a connection to be "idle" on the
// server if there is a call outstanding on it.
TEST_F(TestRpc, TestCallLongerThanKeepalive) {
  TestServerOptions options;
  // set very short keepalive
  options.messenger_options.keep_alive_timeout = 100ms;

  // Set up server.
  HostPort server_addr;
  StartTestServer(&server_addr, options);

  // Set up client.
  auto client_options = kDefaultClientMessengerOptions;
  client_options.keep_alive_timeout = 100ms;
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client", client_options);
  Proxy p(client_messenger.get(), server_addr);

  // Make a call which sleeps longer than the keepalive.
  RpcController controller;
  rpc_test::SleepRequestPB req;
  req.set_sleep_micros(200 * 1000);
  req.set_deferred(true);
  rpc_test::SleepResponsePB resp;
  ASSERT_OK(p.SyncRequest(CalculatorServiceMethods::SleepMethod(), req, &resp, &controller));
}

// Test that connections are kept alive by heartbeats between calls.
TEST_F(TestRpc, TestConnectionHeartbeating) {
  google::FlagSaver saver;

  const auto kTestTimeout = 300ms;

  // Only run one reactor per messenger, so we can grab the metrics from that
  // one without having to check all. Set ScanIdleConnections keep alive to huge value in order
  // to not affect heartbeats testing.
  MessengerOptions messenger_options = { 1, kTestTimeout * 100 };
  TestServerOptions options;
  options.messenger_options = messenger_options;
  FLAGS_num_connections_to_server = 1;
  FLAGS_rpc_connection_timeout_ms = MonoDelta(kTestTimeout).ToMilliseconds();

  // Set up server.
  HostPort server_addr;
  StartTestServer(&server_addr, options);

  for (int i = 0; i < FLAGS_rpc_test_connection_keepalive_num_iterations; ++i) {
    // Set up client.
    LOG(INFO) << "Connecting to " << server_addr;
    auto client_messenger = CreateAutoShutdownMessengerHolder("Client", messenger_options);
    Proxy p(client_messenger.get(), server_addr);

    ASSERT_OK(DoTestSyncCall(&p, CalculatorServiceMethods::AddMethod()));

    SleepFor(kTestTimeout * 3);
    // Both client and server connections should survive when there is no application traffic.
    ASSERT_NO_FATALS(CheckServerMessengerConnections(1));
    ASSERT_NO_FATALS(CheckClientMessengerConnections(client_messenger.get(), 1));
  }
}

// Test that the RpcSidecar transfers the expected messages.
TEST_F(TestRpc, TestRpcSidecar) {
  // Set up server.
  HostPort server_addr;
  StartTestServer(&server_addr);

  // Set up client.
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  // Test some small sidecars
  DoTestSidecar(&p, {123, 456});

  // Test some larger sidecars to verify that we properly handle the case where
  // we can't write the whole response to the socket in a single call.
  DoTestSidecar(&p, {3_MB, 2_MB, 240_MB});

  std::vector<size_t> sizes(20);
  std::fill(sizes.begin(), sizes.end(), 123);
  DoTestSidecar(&p, sizes);
}

// Test that timeouts are properly handled.
TEST_F(TestRpc, TestCallTimeout) {
  HostPort server_addr;
  StartTestServer(&server_addr);
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  uint64_t delay_ns = 1;

  // Test a very short timeout - we expect this will time out while the
  // call is still trying to connect, or in the send queue. This was triggering ASAN failures
  // before.

  while (delay_ns < 100ul * 1000 * 1000) {
    ASSERT_NO_FATALS(DoTestExpectTimeout(&p, MonoDelta::FromNanoseconds(delay_ns)));
    delay_ns *= 2;
  }
}

static void AcceptAndReadForever(Socket* listen_sock) {
  // Accept the TCP connection.
  Socket server_sock;
  Endpoint remote;
  CHECK_OK(listen_sock->Accept(&server_sock, &remote, 0));

  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(MonoDelta::FromSeconds(10));

  size_t nread;
  uint8_t buf[1024];
  while (server_sock.BlockingRecv(buf, sizeof(buf), &nread, deadline).ok()) {
  }
}

// Starts a fake listening socket which never actually negotiates.
// Ensures that the client gets a reasonable status code in this case.
TEST_F(TestRpc, TestNegotiationTimeout) {
  // Set up a simple socket server which accepts a connection.
  HostPort server_addr;
  Socket listen_sock;
  ASSERT_OK(StartFakeServer(&listen_sock, &server_addr));

  // Create another thread to accept the connection on the fake server.
  scoped_refptr<Thread> acceptor_thread;
  ASSERT_OK(Thread::Create("test", "acceptor",
                                  AcceptAndReadForever, &listen_sock,
                                  &acceptor_thread));

  // Set up client.
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  ASSERT_NO_FATALS(DoTestExpectTimeout(&p, MonoDelta::FromMilliseconds(100)));

  acceptor_thread->Join();
}

// Test that client calls get failed properly when the server they're connected to
// shuts down.
TEST_F(TestRpc, TestServerShutsDown) {
  // Set up a simple socket server which accepts a connection.
  HostPort server_addr;
  Socket listen_sock;
  ASSERT_OK(StartFakeServer(&listen_sock, &server_addr));

  // Set up client.
  LOG(INFO) << "Connecting to " << server_addr;
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  // Send a call.
  rpc_test::AddRequestPB req;
  unsigned int seed = SeedRandom();
  req.set_x(rand_r(&seed));
  req.set_y(rand_r(&seed));
  rpc_test::AddResponsePB resp;

  boost::ptr_vector<RpcController> controllers;

  // We'll send several calls async, and ensure that they all
  // get the error status when the connection drops.
  int n_calls = 5;

  CountDownLatch latch(n_calls);
  for (int i = 0; i < n_calls; i++) {
    auto controller = new RpcController();
    controllers.push_back(controller);
    p.AsyncRequest(CalculatorServiceMethods::AddMethod(), req, &resp, controller, [&latch]() {
      latch.CountDown();
    });
  }

  // Accept the TCP connection.
  Socket server_sock;
  Endpoint remote;
  ASSERT_OK(listen_sock.Accept(&server_sock, &remote, 0));

  // The call is still in progress at this point.
  for (const RpcController &controller : controllers) {
    ASSERT_FALSE(controller.finished());
  }

  // Shut down the socket.
  ASSERT_OK(listen_sock.Close());
  ASSERT_OK(server_sock.Close());

  // Wait for the call to be marked finished.
  latch.Wait();

  // Should get the appropriate error on the client for all calls;
  for (const RpcController &controller : controllers) {
    ASSERT_TRUE(controller.finished());
    Status s = controller.status();
    ASSERT_TRUE(s.IsNetworkError()) <<
      "Unexpected status: " << s.ToString();

    // Any of these errors could happen, depending on whether we were
    // in the middle of sending a call while the connection died, or
    // if we were already waiting for responses.
    //
    // ECONNREFUSED is possible because the sending of the calls is async.
    // For example, the following interleaving:
    // - Enqueue 3 calls
    // - Reactor wakes up, creates connection, starts writing calls
    // - Enqueue 2 more calls
    // - Shut down socket
    // - Reactor wakes up, tries to write more of the first 3 calls, gets error
    // - Reactor shuts down connection
    // - Reactor sees the 2 remaining calls, makes a new connection
    // - Because the socket is shut down, gets ECONNREFUSED.
    //
    // EINVAL is possible if the controller socket had already disconnected by
    // the time it tries to set the SO_SNDTIMEO socket option as part of the
    // normal blocking SASL handshake.
    //
    // EPROTOTYPE sometimes happens on Mac OS X.
    // TODO: figure out why.
    Errno err(s);
    ASSERT_TRUE(err == EPIPE ||
                err == ECONNRESET ||
                err == ESHUTDOWN ||
                err == ECONNREFUSED ||
                err == EINVAL
#if defined(__APPLE__)
                || err == EPROTOTYPE
#endif
               )
      << "Unexpected status: " << s.ToString();
  }
}

// Test handler latency metric.
TEST_F(TestRpc, TestRpcHandlerLatencyMetric) {

  const uint64_t sleep_micros = 20 * 1000;

  // Set up server.
  HostPort server_addr;
  StartTestServerWithGeneratedCode(&server_addr);

  // Set up client.

  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  RpcController controller;
  rpc_test::SleepRequestPB req;
  req.set_sleep_micros(sleep_micros);
  req.set_deferred(true);
  rpc_test::SleepResponsePB resp;
  ASSERT_OK(p.SyncRequest(CalculatorServiceMethods::SleepMethod(), req, &resp, &controller));

  const unordered_map<const MetricPrototype*, scoped_refptr<Metric> > metric_map =
    server_messenger()->metric_entity()->UnsafeMetricsMapForTests();

  scoped_refptr<Histogram> latency_histogram = down_cast<Histogram *>(
      FindOrDie(metric_map,
                &METRIC_handler_latency_yb_rpc_test_CalculatorService_Sleep).get());

  LOG(INFO) << "Sleep() min lat: " << latency_histogram->MinValueForTests();
  LOG(INFO) << "Sleep() mean lat: " << latency_histogram->MeanValueForTests();
  LOG(INFO) << "Sleep() max lat: " << latency_histogram->MaxValueForTests();
  LOG(INFO) << "Sleep() #calls: " << latency_histogram->TotalCount();

  ASSERT_EQ(1, latency_histogram->TotalCount());
  ASSERT_GE(latency_histogram->MaxValueForTests(), sleep_micros);
  ASSERT_TRUE(latency_histogram->MinValueForTests() == latency_histogram->MaxValueForTests());

  // TODO: Implement an incoming queue latency test.
  // For now we just assert that the metric exists.
  YB_ASSERT_TRUE(FindOrDie(metric_map, &METRIC_rpc_incoming_queue_time));
}

TEST_F(TestRpc, TestRpcCallbackDestroysMessenger) {
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  HostPort bad_addr;
  CountDownLatch latch(1);

  rpc_test::AddRequestPB req;
  unsigned int seed = SeedRandom();
  req.set_x(rand_r(&seed));
  req.set_y(rand_r(&seed));
  rpc_test::AddResponsePB resp;
  RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(1));
  {
    Proxy p(client_messenger.get(), bad_addr);
    static RemoteMethod method(
        rpc_test::CalculatorServiceIf::static_service_name(), "my-fake-method");
    p.AsyncRequest(&method, req, &resp, &controller, [&latch]() { latch.CountDown(); });
  }
  latch.Wait();
}

// Test that setting the client timeout / deadline gets propagated to RPC
// services.
TEST_F(TestRpc, TestRpcContextClientDeadline) {
  const uint64_t sleep_micros = 20 * 1000;

  // Set up server.
  HostPort server_addr;
  StartTestServerWithGeneratedCode(&server_addr);

  // Set up client.
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  rpc_test::SleepRequestPB req;
  req.set_sleep_micros(sleep_micros);
  req.set_client_timeout_defined(true);
  rpc_test::SleepResponsePB resp;
  RpcController controller;
  const auto* method = CalculatorServiceMethods::SleepMethod();
  Status s = p.SyncRequest(method, req, &resp, &controller);
  ASSERT_TRUE(s.IsRemoteError());
  ASSERT_STR_CONTAINS(s.ToString(), "Missing required timeout");

  controller.Reset();
  controller.set_timeout(MonoDelta::FromMilliseconds(1000));
  ASSERT_OK(p.SyncRequest(method, req, &resp, &controller));
}

// Send multiple long running calls to a single worker thread. All of them except the first one,
// should time out early w/o starting processing them.
TEST_F(TestRpc, QueueTimeout) {
  const MonoDelta kSleep = 1s;
  constexpr auto kCalls = 10;

  // Set up server.
  TestServerOptions options;
  options.n_worker_threads = 1;
  HostPort server_addr;
  StartTestServerWithGeneratedCode(&server_addr, options);

  // Set up client.
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(client_messenger.get(), server_addr);

  const auto* method = CalculatorServiceMethods::SleepMethod();

  CountDownLatch latch(kCalls);

  struct Call {
    rpc_test::SleepRequestPB req;
    rpc_test::SleepResponsePB resp;
    RpcController controller;
  };
  std::vector<Call> calls(kCalls);

  for (int i = 0; i != kCalls; ++i) {
    auto& call = calls[i];
    auto& req = call.req;
    req.set_sleep_micros(kSleep.ToMicroseconds());
    req.set_client_timeout_defined(true);
    call.controller.set_timeout(kSleep / 2);
    p.AsyncRequest(method, req, &call.resp, &call.controller,
        [&latch, &call] {
      latch.CountDown();
      ASSERT_TRUE(call.controller.status().IsTimedOut()) << call.controller.status();
    });
  }

  latch.Wait();

  // Give some time for algorithm to work.
  std::this_thread::sleep_for((kSleep / 2).ToSteadyDuration());

  Counter* counter = nullptr;
  std::string metric_name("rpcs_timed_out_early_in_queue");
  const auto& map = metric_entity()->UnsafeMetricsMapForTests();
  for (const auto& p : map) {
    if (p.first->name() == metric_name) {
      counter = down_cast<Counter*>(p.second.get());
      break;
    }
  }

  ASSERT_NE(counter, nullptr);
  // First call should succeed, other should timeout.
  ASSERT_EQ(counter->value(), kCalls - 1);
}

struct DisconnectShare {
  Proxy proxy;
  size_t left;
  std::mutex mutex;
  std::condition_variable cond;
  std::unordered_map<std::string, size_t> counts;
};

class DisconnectTask {
 public:
  explicit DisconnectTask(DisconnectShare* share) : share_(share) {
  }

  void Launch() {
    controller_.set_timeout(MonoDelta::FromSeconds(1));
    share_->proxy.AsyncRequest(CalculatorServiceMethods::DisconnectMethod(),
                               rpc_test::DisconnectRequestPB(),
                               &response_,
                               &controller_,
                               [this]() { this->Done(); });
  }
 private:
  void Done() {
    bool notify;
    {
      std::lock_guard<std::mutex> lock(share_->mutex);
      ++share_->counts[controller_.status().ToString()];
      notify = 0 == --share_->left;
    }
    if (notify)
      share_->cond.notify_one();
  }

  DisconnectShare* share_;
  rpc_test::DisconnectResponsePB response_;
  RpcController controller_;
};

TEST_F(TestRpc, TestDisconnect) {
  // Set up server.
  HostPort server_addr;
  StartTestServerWithGeneratedCode(&server_addr);

  // Set up client.
  auto client_messenger = CreateAutoShutdownMessengerHolder("Client");

  constexpr size_t kRequests = 10000;
  DisconnectShare share = { { client_messenger.get(), server_addr }, kRequests };

  std::vector<DisconnectTask> tasks;
  for (size_t i = 0; i != kRequests; ++i) {
    tasks.emplace_back(&share);
  }
  for (size_t i = 0; i != kRequests; ++i) {
    tasks[i].Launch();
  }
  {
    std::unique_lock<std::mutex> lock(share.mutex);
    share.cond.wait(lock, [&share]() { return !share.left; });
  }

  size_t total = 0;
  for (const auto& pair : share.counts) {
    ASSERT_NE(pair.first, "OK");
    total += pair.second;
    LOG(INFO) << pair.first << ": " << pair.second;
  }
  ASSERT_EQ(kRequests, total);
}

// Check that we could perform DumpRunningRpcs while timed out calls are in queue.
//
// Start listenting socket, that will accept one connection and does not read it.
// Send big RPC request, that does not fit into socket buffer, so it will be sending forever.
// Wait until this call is timed out.
// Check that we could invoke DumpRunningRpcs after it.
TEST_F(TestRpc, DumpTimedOutCall) {
  // Set up a simple socket server which accepts a connection.
  HostPort server_addr;
  Socket listen_sock;
  ASSERT_OK(StartFakeServer(&listen_sock, &server_addr));

  std::atomic<bool> stop(false);

  std::thread thread([&listen_sock, &stop] {
    Socket socket;
    Endpoint remote;
    ASSERT_OK(listen_sock.Accept(&socket, &remote, 0));
    while (!stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(100ms);
    }
  });

  auto messenger = CreateAutoShutdownMessengerHolder("Client");
  Proxy p(messenger.get(), server_addr);

  {
    rpc_test::EchoRequestPB req;
    req.set_data(std::string(1_MB, 'X'));
    rpc_test::EchoResponsePB resp;
    std::aligned_storage<sizeof(RpcController), alignof(RpcController)>::type storage;
    auto controller = new (&storage) RpcController;
    controller->set_timeout(100ms);
    auto status = p.SyncRequest(CalculatorServiceMethods::EchoMethod(), req, &resp, controller);
    ASSERT_TRUE(status.IsTimedOut()) << status;
    controller->~RpcController();
    memset(&storage, 0xff, sizeof(storage));
  }

  DumpRunningRpcsRequestPB dump_req;
  DumpRunningRpcsResponsePB dump_resp;
  ASSERT_OK(messenger->DumpRunningRpcs(dump_req, &dump_resp));

  stop.store(true, std::memory_order_release);
  thread.join();
}

class TestRpcSecure : public RpcTestBase {
 public:
  void SetUp() override {
    RpcTestBase::SetUp();
    secure_context_ = std::make_unique<SecureContext>();
    EXPECT_OK(secure_context_->TEST_GenerateKeys(512, "127.0.0.1"));
    TestServerOptions options;
    StartTestServerWithGeneratedCode(
        CreateSecureMessenger("TestServer"), &server_hostport_, options);
    client_messenger_ = CreateSecureMessenger("Client");
    proxy_cache_ = std::make_unique<ProxyCache>(client_messenger_.get());
  }

  void TearDown() override {
    client_messenger_->Shutdown();
    RpcTestBase::TearDown();
  }

 protected:
  std::unique_ptr<Messenger> CreateSecureMessenger(const std::string& name) {
    auto builder = CreateMessengerBuilder(name);
    builder.SetListenProtocol(SecureStreamProtocol());
    builder.AddStreamFactory(
        SecureStreamProtocol(),
        SecureStreamFactory(TcpStream::Factory(), MemTracker::GetRootTracker(),
                            secure_context_.get()));
    return EXPECT_RESULT(builder.Build());
  }

  HostPort server_hostport_;
  std::unique_ptr<SecureContext> secure_context_;
  std::unique_ptr<Messenger> client_messenger_;
  std::unique_ptr<ProxyCache> proxy_cache_;
};

TEST_F(TestRpcSecure, TLS) {
  rpc_test::CalculatorServiceProxy p(
      proxy_cache_.get(), server_hostport_, SecureStreamProtocol());

  RpcController controller;
  controller.set_timeout(5s);
  rpc_test::AddRequestPB req;
  req.set_x(10);
  req.set_y(20);
  rpc_test::AddResponsePB resp;
  ASSERT_OK(p.Add(req, &resp, &controller));
  ASSERT_EQ(30, resp.result());
}

} // namespace rpc
} // namespace yb
