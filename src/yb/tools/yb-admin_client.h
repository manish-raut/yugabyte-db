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
#ifndef YB_TOOLS_YB_ADMIN_CLIENT_H
#define YB_TOOLS_YB_ADMIN_CLIENT_H

#include <boost/optional.hpp>

#include "yb/client/client.h"
#include "yb/client/yb_table_name.h"
#include "yb/util/status.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/common/entity_ids.h"
#include "yb/tools/yb-admin_cli.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"
#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/messenger.h"

namespace yb {

namespace consensus {
class ConsensusServiceProxy;
}

namespace client {
class YBClient;
}

namespace tools {

class ClusterAdminClient {
 public:
  enum PeerMode {
    LEADER = 1,
    FOLLOWER
  };

  // Creates an admin client for host/port combination e.g.,
  // "localhost" or "127.0.0.1:7050" with the given timeout.
  // If certs_dir is non-empty, caller will init the yb_client_.
  ClusterAdminClient(std::string addrs, int64_t timeout_millis, string certs_dir);

  virtual ~ClusterAdminClient();

  // Initialized the client and connects to the specified tablet server.
  virtual CHECKED_STATUS Init();

  // Parse the user-specified change type to consensus change type
  CHECKED_STATUS ParseChangeType(
      const std::string& change_type,
      consensus::ChangeConfigType* cc_type);

  // Change the configuration of the specified tablet.
  CHECKED_STATUS ChangeConfig(
      const TabletId& tablet_id,
      const std::string& change_type,
      const PeerId& peer_uuid,
      const boost::optional<std::string>& member_type);

  // Change the configuration of the master tablet.
  CHECKED_STATUS ChangeMasterConfig(
      const std::string& change_type,
      const std::string& peer_host,
      int16 peer_port,
      bool use_hostport);

  CHECKED_STATUS DumpMasterState();

  // List all the tables.
  CHECKED_STATUS ListTables(bool include_db_type);

  // List all tablets of this table
  CHECKED_STATUS ListTablets(const client::YBTableName& table_name, int max_tablets);

  // Per Tablet list of all tablet servers
  CHECKED_STATUS ListPerTabletTabletServers(const PeerId& tablet_id);

  // Delete a single table by name.
  CHECKED_STATUS DeleteTable(const client::YBTableName& table_name);

  // List all tablet servers known to master
  CHECKED_STATUS ListAllTabletServers();

  // List all masters
  CHECKED_STATUS ListAllMasters();

  // List the log locations of all tablet servers, by uuid
  CHECKED_STATUS ListTabletServersLogLocations();

  // List all the tablets a certain tablet server is serving
  CHECKED_STATUS ListTabletsForTabletServer(const PeerId& ts_uuid);

  CHECKED_STATUS SetLoadBalancerEnabled(bool is_enabled);

  CHECKED_STATUS GetLoadMoveCompletion();

  CHECKED_STATUS GetLeaderBlacklistCompletion();

  CHECKED_STATUS GetIsLoadBalancerIdle();

  CHECKED_STATUS ListLeaderCounts(const client::YBTableName& table_name);

  CHECKED_STATUS SetupRedisTable();

  CHECKED_STATUS DropRedisTable();

  CHECKED_STATUS FlushTable(const client::YBTableName& table_name,
                            int timeout_secs,
                            bool is_compaction);

  CHECKED_STATUS ModifyPlacementInfo(std::string placement_infos,
                                     int replication_factor,
                                     const std::string& optional_uuid);

  CHECKED_STATUS AddReadReplicaPlacementInfo(const std::string& placement_info,
                                             int replication_factor,
                                             const std::string& optional_uuid);

  CHECKED_STATUS ModifyReadReplicaPlacementInfo(const std::string& placement_uuid,
                                                const std::string& placement_info,
                                                int replication_factor);

  CHECKED_STATUS DeleteReadReplicaPlacementInfo();

  CHECKED_STATUS GetUniverseConfig();

  CHECKED_STATUS ChangeBlacklist(const std::vector<HostPort>& servers, bool add,
      bool blacklist_leader);

  Result<const master::NamespaceIdentifierPB&> GetNamespaceInfo(YQLDatabase db_type,
                                                                const std::string& namespace_name);

 protected:
  // Fetch the locations of the replicas for a given tablet from the Master.
  CHECKED_STATUS GetTabletLocations(const TabletId& tablet_id,
                                    master::TabletLocationsPB* locations);

  // Fetch information about the location of a tablet peer from the leader master.
  CHECKED_STATUS GetTabletPeer(
      const TabletId& tablet_id,
      PeerMode mode,
      master::TSInfoPB* ts_info);

  // Set the uuid and the socket information for a peer of this tablet.
  CHECKED_STATUS SetTabletPeerInfo(
      const TabletId& tablet_id,
      PeerMode mode,
      PeerId* peer_uuid,
      HostPort* peer_addr);

  // Fetch the latest list of tablet servers from the Master.
  CHECKED_STATUS ListTabletServers(
      google::protobuf::RepeatedPtrField<master::ListTabletServersResponsePB::Entry>* servers);

  // Look up the RPC address of the server with the specified UUID from the Master.
  Result<HostPort> GetFirstRpcAddressForTS(const std::string& uuid);

  CHECKED_STATUS LeaderStepDown(
      const PeerId& leader_uuid,
      const std::string& tablet_id,
      std::unique_ptr<consensus::ConsensusServiceProxy>* leader_proxy);

  CHECKED_STATUS StartElection(const std::string& tablet_id);

  CHECKED_STATUS MasterLeaderStepDown(const std::string& leader_uuid);
  CHECKED_STATUS GetMasterLeaderInfo(std::string* leader_uuid);
  CHECKED_STATUS WaitUntilMasterLeaderReady();

  const std::string master_addr_list_;
  const MonoDelta timeout_;
  HostPort leader_addr_;
  std::unique_ptr<rpc::Messenger> messenger_;
  std::unique_ptr<rpc::ProxyCache> proxy_cache_;
  std::unique_ptr<master::MasterServiceProxy> master_proxy_;
  // Skip yb_client_ and related fields' initialization.
  bool client_init_ = true;
  std::unique_ptr<client::YBClient> yb_client_;
  bool initted_ = false;

 private:
  CHECKED_STATUS FillPlacementInfo(
      master::PlacementInfoPB* placement_info_pb, const std::string& placement_str);

  Result<int> GetReadReplicaConfigFromPlacementUuid(
      master::ReplicationInfoPB* replication_info, const std::string& placement_uuid);


  Result<master::GetMasterClusterConfigResponsePB> GetMasterClusterConfig();

  // Perform RPC call without checking Response structure for error
  template<class Response, class Request, class Object>
  Result<Response> InvokeRpcNoResponseCheck(
      Status (Object::*func)(const Request&, Response*, rpc::RpcController*),
      Object* obj, const Request& req, const char* error_message = nullptr);

  // Perform RPC call by calling InvokeRpcNoResponseCheck
  // and check Response structure for error by using its has_error method (if any)
  template<class Response, class Request, class Object>
  Result<Response> InvokeRpc(
      Status (Object::*func)(const Request&, Response*, rpc::RpcController*),
      Object* obj, const Request& req, const char* error_message = nullptr);

 private:
  using NamespaceMap = std::unordered_map<NamespaceId, master::NamespaceIdentifierPB>;
  Result<const NamespaceMap&> GetNamespaceMap();

  NamespaceMap namespace_map_;

  DISALLOW_COPY_AND_ASSIGN(ClusterAdminClient);
};

static constexpr const char* kColumnSep = " \t";

std::string RightPadToUuidWidth(const std::string &s);

struct TypedNamespaceName {
  YQLDatabase db_type;
  std::string name;
};

Result<TypedNamespaceName> ParseNamespaceName(const std::string& full_namespace_name);

}  // namespace tools
}  // namespace yb

#endif // YB_TOOLS_YB_ADMIN_CLIENT_H
