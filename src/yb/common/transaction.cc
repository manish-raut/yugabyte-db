//
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
//

#include "yb/common/transaction.h"

#include "yb/util/tsan_util.h"

using namespace std::literals;

DEFINE_int64(transaction_rpc_timeout_ms, 5000 * yb::kTimeMultiplier,
             "Timeout used by transaction related RPCs in milliseconds.");

namespace yb {

const std::string kTransactionsTableName = "transactions";
const std::string kMetricsSnapshotsTableName = "metrics";

namespace {

// Makes transaction id from its binary representation.
// If check_exact_size is true, checks that slice contains only TransactionId.
Result<TransactionId> DoDecodeTransactionId(const Slice &slice, const bool check_exact_size) {
  if (check_exact_size ? slice.size() != TransactionId::static_size()
                       : slice.size() < TransactionId::static_size()) {
    return STATUS_FORMAT(
        Corruption, "Invalid length of binary data with transaction id '$0': $1 (expected $2$3)",
        slice.ToDebugHexString(), slice.size(), check_exact_size ? "" : "at least ",
        TransactionId::static_size());
  }
  TransactionId id;
  memcpy(id.data, slice.data(), TransactionId::static_size());
  return id;
}

} // namespace

TransactionStatusResult::TransactionStatusResult(TransactionStatus status_, HybridTime status_time_)
    : status(status_), status_time(status_time_) {
  DCHECK(status == TransactionStatus::ABORTED || status_time.is_valid())
      << "Status: " << status << ", status_time: " << status_time;
}

Result<TransactionId> FullyDecodeTransactionId(const Slice& slice) {
  return DoDecodeTransactionId(slice, true);
}

Result<TransactionId> DecodeTransactionId(Slice* slice) {
  Result<TransactionId> id = DoDecodeTransactionId(*slice, false);
  RETURN_NOT_OK(id);
  slice->remove_prefix(TransactionId::static_size());
  return id;
}

Result<TransactionMetadata> TransactionMetadata::FromPB(const TransactionMetadataPB& source) {
  TransactionMetadata result;
  auto id = FullyDecodeTransactionId(source.transaction_id());
  RETURN_NOT_OK(id);
  result.transaction_id = *id;
  if (source.has_isolation()) {
    result.isolation = source.isolation();
    result.status_tablet = source.status_tablet();
    result.priority = source.priority();
    result.DEPRECATED_start_time = HybridTime(source.deprecated_start_hybrid_time());
  }
  return result;
}

void TransactionMetadata::ToPB(TransactionMetadataPB* dest) const {
  if (isolation != IsolationLevel::NON_TRANSACTIONAL) {
    ForceToPB(dest);
  } else {
    dest->set_transaction_id(transaction_id.data, transaction_id.size());
  }
}

void TransactionMetadata::ForceToPB(TransactionMetadataPB* dest) const {
  dest->set_transaction_id(transaction_id.data, transaction_id.size());
  dest->set_isolation(isolation);
  dest->set_status_tablet(status_tablet);
  dest->set_priority(priority);
  dest->set_deprecated_start_hybrid_time(DEPRECATED_start_time.ToUint64());
}

bool operator==(const TransactionMetadata& lhs, const TransactionMetadata& rhs) {
  return lhs.transaction_id == rhs.transaction_id &&
         lhs.isolation == rhs.isolation &&
         lhs.status_tablet == rhs.status_tablet &&
         lhs.priority == rhs.priority &&
         lhs.DEPRECATED_start_time == rhs.DEPRECATED_start_time;
}

std::ostream& operator<<(std::ostream& out, const TransactionMetadata& metadata) {
  return out << metadata.ToString();
}

MonoDelta TransactionRpcTimeout() {
  return FLAGS_transaction_rpc_timeout_ms * 1ms;
}

// TODO(dtxn) correct deadline should be calculated and propagated.
CoarseTimePoint TransactionRpcDeadline() {
  return CoarseMonoClock::Now() + TransactionRpcTimeout();
}

bool TransactionOperationContext::transactional() const {
  return !transaction_id.is_nil();
}

} // namespace yb
