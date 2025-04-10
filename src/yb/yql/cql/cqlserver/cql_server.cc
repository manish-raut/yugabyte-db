// Copyright (c) YugaByte, Inc.
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

#include <yb/rpc/rpc_introspection.pb.h>
#include "yb/yql/cql/cqlserver/cql_server.h"

#include "yb/util/flag_tags.h"
#include "yb/util/size_literals.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/yql/cql/cqlserver/cql_service.h"
#include "yb/rpc/messenger.h"

using yb::rpc::ServiceIf;
using namespace yb::size_literals;  // NOLINT.

DEFINE_int32(cql_service_queue_length, 10000,
             "RPC queue length for CQL service");
TAG_FLAG(cql_service_queue_length, advanced);

DEFINE_int32(cql_nodelist_refresh_interval_secs, 300,
             "Interval after which a node list refresh event should be sent to all CQL clients.");
TAG_FLAG(cql_nodelist_refresh_interval_secs, runtime);
TAG_FLAG(cql_nodelist_refresh_interval_secs, advanced);

DEFINE_int64(cql_rpc_memory_limit, 0, "CQL RPC memory limit");

using namespace std::placeholders;

namespace yb {
namespace cqlserver {

namespace {

boost::posix_time::time_duration refresh_interval() {
  return boost::posix_time::seconds(FLAGS_cql_nodelist_refresh_interval_secs);
}

}

CQLServer::CQLServer(const CQLServerOptions& opts,
                     boost::asio::io_service* io,
                     tserver::TabletServer* tserver)
    : RpcAndWebServerBase(
          "CQLServer", opts, "yb.cqlserver",
          MemTracker::CreateTracker(
              "CQL", tserver ? tserver->mem_tracker() : MemTracker::GetRootTracker(),
              AddToParent::kTrue, CreateMetrics::kFalse)),
      opts_(opts),
      timer_(*io, refresh_interval()),
      tserver_(tserver) {
  SetConnectionContextFactory(rpc::CreateConnectionContextFactory<CQLConnectionContext>(
      FLAGS_cql_rpc_memory_limit, mem_tracker()->parent()));
}

Status CQLServer::Start() {
  RETURN_NOT_OK(server::RpcAndWebServerBase::Init());

  auto cql_service = std::make_shared<CQLServiceImpl>(
      this, opts_, std::bind(&tserver::TabletServerIf::TransactionPool, tserver_));
  cql_service->CompleteInit();

  RETURN_NOT_OK(RegisterService(FLAGS_cql_service_queue_length, std::move(cql_service)));

  RETURN_NOT_OK(server::RpcAndWebServerBase::Start());

  // Start the CQL node list refresh timer.
  timer_.async_wait(boost::bind(&CQLServer::CQLNodeListRefresh, this,
                                boost::asio::placeholders::error));
  return Status::OK();
}

void CQLServer::Shutdown() {
  boost::system::error_code ec;
  timer_.cancel(ec);
  if (ec) {
    LOG(WARNING) << "Failed to cancel timer: " << ec;
  }
  server::RpcAndWebServerBase::Shutdown();
}

void CQLServer::RescheduleTimer() {
  // Reschedule the timer.
  boost::system::error_code ec;
  auto new_expires = timer_.expires_at() + refresh_interval();
  timer_.expires_at(new_expires, ec);
  if (ec) {
    LOG(WARNING) << "Failed to reschedule timer: " << ec;
  }
  timer_.async_wait(boost::bind(&CQLServer::CQLNodeListRefresh, this,
                                boost::asio::placeholders::error));
}

std::unique_ptr<CQLServerEvent> CQLServer::BuildTopologyChangeEvent(
    const std::string& event_type, const Endpoint& addr) {
  std::unique_ptr<EventResponse> event_response(new TopologyChangeEventResponse(event_type, addr));
  std::unique_ptr<CQLServerEvent> cql_server_event(new CQLServerEvent(std::move(event_response)));
  return cql_server_event;
}

void CQLServer::CQLNodeListRefresh(const boost::system::error_code &e) {
  if (!e) {
    auto cqlserver_event_list = std::make_shared<CQLServerEventList>();
    if (tserver_ != nullptr) {
      // Get all live tservers.
      std::vector<master::TSInformationPB> live_tservers;
      Status s = tserver_->GetLiveTServers(&live_tservers);
      if (!s.ok()) {
        LOG (WARNING) << s.ToString();
        RescheduleTimer();
        return;
      }

      // Queue NEW_NODE event for all the live tservers.
      for (const master::TSInformationPB& ts_info : live_tservers) {
        const auto& hostport_pb = DesiredHostPort(ts_info.registration().common(), CloudInfoPB());
        if (hostport_pb.host().empty()) {
          LOG (WARNING) << "Skipping TS since it doesn't have any rpc address: "
                        << ts_info.DebugString();
          continue;
        }

        // Use only the first rpc address.
        InetAddress addr;
        if (PREDICT_FALSE(!addr.FromString(hostport_pb.host()).ok())) {
          LOG(WARNING) << strings::Substitute("Couldn't parse host $0", hostport_pb.host());
          continue;
        }

        // We need the CQL port not the tserver port so use the rpc port from the local CQL server.
        // Note: this relies on the fact that all tservers must use the same CQL port which is not
        // currently enforced on YB side, but is practically required by the drivers.
        const auto cql_port = first_rpc_address().port();

        // Queue event for all clients to add a node.
        cqlserver_event_list->AddEvent(
            BuildTopologyChangeEvent(TopologyChangeEventResponse::kNewNode,
                                     Endpoint(addr.address(), cql_port)));
      }
    }

    // Queue node refresh event, to remove any nodes that are down. Note that the 'MOVED_NODE'
    // event forces the client to refresh its entire cluster topology. The RPC address associated
    // with the event doesn't have much significance.
    cqlserver_event_list->AddEvent(
        BuildTopologyChangeEvent(TopologyChangeEventResponse::kMovedNode, first_rpc_address()));

    Status s = messenger_->QueueEventOnAllReactors(cqlserver_event_list, SOURCE_LOCATION());
    if (!s.ok()) {
      LOG (WARNING) << strings::Substitute("Failed to push events: [$0], due to: $1",
                                           cqlserver_event_list->ToString(), s.ToString());
    }

    RescheduleTimer();
  }
}

}  // namespace cqlserver
}  // namespace yb
