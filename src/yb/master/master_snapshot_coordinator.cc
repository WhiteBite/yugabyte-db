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

#include "yb/master/master_snapshot_coordinator.h"

#include <unordered_map>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

#include "yb/common/snapshot.h"
#include "yb/common/transaction_error.h"

#include "yb/docdb/doc_key.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/master_error.h"
#include "yb/master/sys_catalog_writer.h"

#include "yb/rpc/poller.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/operations/snapshot_operation.h"
#include "yb/tablet/operations/write_operation.h"

#include "yb/util/pb_util.h"

using namespace std::literals;
using namespace std::placeholders;

DECLARE_int32(sys_catalog_write_timeout_ms);

DEFINE_uint64(snapshot_coordinator_poll_interval_ms, 5000,
              "Poll interval for snapshot coordinator in milliseconds.");

namespace yb {
namespace master {

namespace {

YB_STRONGLY_TYPED_BOOL(ForClient);

const std::initializer_list<std::pair<SysSnapshotEntryPB::State, SysSnapshotEntryPB::State>>
    kStateTransitions = {
  { SysSnapshotEntryPB::CREATING, SysSnapshotEntryPB::COMPLETE },
  { SysSnapshotEntryPB::DELETING, SysSnapshotEntryPB::DELETED },
  { SysSnapshotEntryPB::RESTORING, SysSnapshotEntryPB::RESTORED },
};

SysSnapshotEntryPB::State InitialStateToTerminalState(SysSnapshotEntryPB::State state) {
  for (const auto& initial_and_terminal_states : kStateTransitions) {
    if (state == initial_and_terminal_states.first) {
      return initial_and_terminal_states.second;
    }
  }

  FATAL_INVALID_PB_ENUM_VALUE(SysSnapshotEntryPB::State, state);
}

struct TabletSnapshotOperation {
  TabletId tablet_id;
  TxnSnapshotId snapshot_id;
  SysSnapshotEntryPB::State state;
  HybridTime snapshot_hybrid_time;

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(tablet_id, snapshot_id, state, snapshot_hybrid_time);
  }
};

using TabletSnapshotOperations = std::vector<TabletSnapshotOperation>;

class StateWithTablets {
 public:
  StateWithTablets(
      SnapshotCoordinatorContext* context, SysSnapshotEntryPB::State initial_state)
      : context_(*context), initial_state_(initial_state) {
  }

  virtual ~StateWithTablets() = default;

  template <class TabletIds>
  void InitTabletIds(const TabletIds& tablet_ids, SysSnapshotEntryPB::State state) {
    for (const auto& id : tablet_ids) {
      tablets_.emplace(id, state);
    }
    num_tablets_in_initial_state_ = state == initial_state_ ? tablet_ids.size() : 0;
  }

  // Initialize tablet states using tablet ids, i.e. put all tablets in initial state.
  template <class TabletIds>
  void InitTabletIds(const TabletIds& tablet_ids) {
    InitTabletIds(tablet_ids, initial_state_);
  }

  // Initialize tablet states from serialized data.
  void InitTablets(
      const google::protobuf::RepeatedPtrField<SysSnapshotEntryPB::TabletSnapshotPB>& tablets) {
    for (const auto& tablet : tablets) {
      tablets_.emplace(tablet.id(), tablet.state());
      if (tablet.state() == initial_state_) {
        ++num_tablets_in_initial_state_;
      }
    }
  }

  StateWithTablets(const StateWithTablets&) = delete;
  void operator=(const StateWithTablets&) = delete;

  // If any of tablets failed returns this failure.
  // Otherwise if any of tablets is in initial state returns initial state.
  // Otherwise all tablets should be in the same state, which is returned.
  Result<SysSnapshotEntryPB::State> AggregatedState() {
    SysSnapshotEntryPB::State result = initial_state_;
    bool has_initial = false;
    for (const auto& tablet : tablets_) {
      if (tablet.state == SysSnapshotEntryPB::FAILED) {
        return SysSnapshotEntryPB::FAILED;
      } else if (tablet.state == initial_state_) {
        has_initial = true;
      } else if (result == initial_state_) {
        result = tablet.state;
      } else if (tablet.state != result) {
        // Should not happen.
        return STATUS_FORMAT(IllegalState, "Tablets in different terminal states: $0 and $1",
                             SysSnapshotEntryPB::State_Name(result),
                             SysSnapshotEntryPB::State_Name(tablet.state));
      }
    }
    return has_initial ? initial_state_ : result;
  }

  Result<bool> Complete() {
    return VERIFY_RESULT(AggregatedState()) != initial_state_;
  }

  bool AllTabletsDone() {
    return num_tablets_in_initial_state_ == 0;
  }

  std::vector<TabletId> TabletIdsInState(SysSnapshotEntryPB::State state) {
    std::vector<TabletId> result;
    result.reserve(tablets_.size());
    for (const auto& tablet : tablets_) {
      if (tablet.state == state) {
        result.push_back(tablet.id);
      }
    }
    return result;
  }

  // Invoking callback for all operations that are not running and are still in the initial state.
  // Marking such operations as running.
  template <class Functor>
  void DoPrepareOperations(const Functor& functor) {
    auto& running_index = tablets_.get<RunningTag>();
    for (auto it = running_index.begin(); it != running_index.end();) {
      if (it->running) {
        // Could exit here, because we have already iterated over all non-running operations.
        break;
      }
      bool should_run = it->state == initial_state_;
      if (should_run) {
        functor(*it);

        // Here we modify indexed value, so iterator could be advanced to the next element.
        // Taking next before modify.
        auto new_it = it;
        ++new_it;
        running_index.modify(it, [](TabletData& data) { data.running = true; });
        it = new_it;
      } else {
        ++it;
      }
    }
  }

  void TabletsToPB(google::protobuf::RepeatedPtrField<SysSnapshotEntryPB::TabletSnapshotPB>* out) {
    out->Reserve(tablets_.size());
    for (const auto& tablet : tablets_) {
      auto* tablet_state = out->Add();
      tablet_state->set_id(tablet.id);
      tablet_state->set_state(tablet.state);
    }
  }

  void Done(const TabletId& tablet_id, const Status& status) {
    VLOG(4) << __func__ << "(" << tablet_id << ", " << status << ")";

    auto it = tablets_.find(tablet_id);
    if (it == tablets_.end()) {
      LOG(DFATAL) << "Finished " << InitialStateName() <<  " snapshot at unknown tablet "
                  << tablet_id << ": " << status;
      return;
    }
    if (!it->running) {
      LOG(DFATAL) << "Finished " << InitialStateName() <<  " snapshot at " << tablet_id
                  << " that is not running and in state "
                  << SysSnapshotEntryPB::State_Name(it->state) << ": " << status;
      return;
    }
    tablets_.modify(it, [](TabletData& data) { data.running = false; });
    const auto& state = it->state;
    if (state == initial_state_) {
      if (status.ok()) {
        tablets_.modify(
            it, [terminal_state = InitialStateToTerminalState(initial_state_)](TabletData& data) {
          data.state = terminal_state;
        });
        LOG(INFO) << "Finished " << InitialStateName() << " snapshot at " << tablet_id;
      } else {
        auto full_status = status.CloneAndPrepend(
            Format("Failed to $0 snapshot at $1", InitialStateName(), tablet_id));
        LOG(WARNING) << full_status;
        bool terminal = IsTerminalFailure(status);
        tablets_.modify(it, [&full_status, terminal](TabletData& data) {
          if (terminal) {
            data.state = SysSnapshotEntryPB::FAILED;
          }
          data.last_error = full_status;
        });
        if (!terminal) {
          return;
        }
      }
      --num_tablets_in_initial_state_;
    } else {
      LOG(DFATAL) << "Finished " << InitialStateName() << " snapshot at tablet " << tablet_id
                  << " in a wrong state " << state << ": " << status;
    }
  }

  SnapshotCoordinatorContext& context() const {
    return context_;
  }

  bool AllInState(SysSnapshotEntryPB::State state) {
    for (const auto& tablet : tablets_) {
      if (tablet.state != state) {
        return false;
      }
    }

    return true;
  }

  bool HasInState(SysSnapshotEntryPB::State state) {
    for (const auto& tablet : tablets_) {
      if (tablet.state == state) {
        return true;
      }
    }

    return false;
  }

  void SetInitialTabletsState(SysSnapshotEntryPB::State state) {
    initial_state_ = state;
    for (auto it = tablets_.begin(); it != tablets_.end(); ++it) {
      tablets_.modify(it, [state](TabletData& data) {
        data.state = state;
      });
    }
    num_tablets_in_initial_state_ = tablets_.size();
  }

  virtual bool IsTerminalFailure(const Status& status) = 0;

  SysSnapshotEntryPB::State initial_state() const {
    return initial_state_;
  }

 protected:
  struct TabletData {
    TabletId id;
    SysSnapshotEntryPB::State state;
    Status last_error;
    bool running = false;

    TabletData(const TabletId& id_, SysSnapshotEntryPB::State state_)
        : id(id_), state(state_) {
    }
  };

 private:
  const std::string& InitialStateName() const {
    return SysSnapshotEntryPB::State_Name(initial_state_);
  }

  SnapshotCoordinatorContext& context_;
  SysSnapshotEntryPB::State initial_state_;

  class RunningTag;

  typedef boost::multi_index_container<
    TabletData,
    boost::multi_index::indexed_by<
      boost::multi_index::hashed_unique<
        boost::multi_index::member<TabletData, TabletId, &TabletData::id>
      >,
      boost::multi_index::ordered_non_unique<
        boost::multi_index::tag<RunningTag>,
        boost::multi_index::member<TabletData, bool, &TabletData::running>
      >
    >
  > Tablets;

  Tablets tablets_;

  size_t num_tablets_in_initial_state_ = 0;
};

class SnapshotState : public StateWithTablets {
 public:
  SnapshotState(
      SnapshotCoordinatorContext* context, const TxnSnapshotId& id,
      const tserver::TabletSnapshotOpRequestPB& request)
      : StateWithTablets(context, SysSnapshotEntryPB::CREATING),
        id_(id), snapshot_hybrid_time_(request.snapshot_hybrid_time()) {
    InitTabletIds(request.tablet_id(),
                  request.imported() ? SysSnapshotEntryPB::COMPLETE : SysSnapshotEntryPB::CREATING);
    request.extra_data().UnpackTo(&entries_);
  }

  SnapshotState(
      SnapshotCoordinatorContext* context, const TxnSnapshotId& id,
      const SysSnapshotEntryPB& entry)
      : StateWithTablets(context, entry.state()),
        id_(id), snapshot_hybrid_time_(entry.snapshot_hybrid_time()) {
    InitTablets(entry.tablet_snapshots());
    *entries_.mutable_entries() = entry.entries();
  }

  const TxnSnapshotId& id() const {
    return id_;
  }

  HybridTime snapshot_hybrid_time() const {
    return snapshot_hybrid_time_;
  }

  CHECKED_STATUS ToPB(SnapshotInfoPB* out) {
    out->set_id(id_.data(), id_.size());
    return ToEntryPB(out->mutable_entry(), ForClient::kTrue);
  }

  CHECKED_STATUS ToEntryPB(SysSnapshotEntryPB* out, ForClient for_client) {
    out->set_state(for_client ? VERIFY_RESULT(AggregatedState()) : initial_state());
    out->set_snapshot_hybrid_time(snapshot_hybrid_time_.ToUint64());

    TabletsToPB(out->mutable_tablet_snapshots());

    *out->mutable_entries() = entries_.entries();

    return Status::OK();
  }

  CHECKED_STATUS StoreToWriteBatch(docdb::KeyValueWriteBatchPB* out) {
    docdb::DocKey doc_key({ docdb::PrimitiveValue::Int32(SysRowEntry::SNAPSHOT),
                            docdb::PrimitiveValue(id_.AsSlice().ToBuffer()) });
    docdb::SubDocKey sub_doc_key(
        doc_key, docdb::PrimitiveValue(VERIFY_RESULT(context().MetadataColumnId())));
    auto encoded_key = sub_doc_key.Encode();
    auto pair = out->add_write_pairs();
    pair->set_key(encoded_key.data());
    faststring value;
    value.push_back(docdb::ValueTypeAsChar::kString);
    SysSnapshotEntryPB entry;
    RETURN_NOT_OK(ToEntryPB(&entry, ForClient::kFalse));
    pb_util::AppendToString(entry, &value);
    pair->set_value(value.data(), value.size());
    return Status::OK();
  }

  CHECKED_STATUS CheckCanDelete() {
    if (AllInState(SysSnapshotEntryPB::DELETED)) {
      return STATUS(NotFound, "The snapshot was deleted", id_.ToString(),
                    MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
    }
    if (HasInState(SysSnapshotEntryPB::DELETING)) {
      return STATUS(NotFound, "The snapshot is being deleted", id_.ToString(),
                    MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
    }

    return Status::OK();
  }

  void PrepareOperations(TabletSnapshotOperations* out) {
    DoPrepareOperations([this, out](const TabletData& tablet) {
      out->push_back(TabletSnapshotOperation {
        .tablet_id = tablet.id,
        .snapshot_id = id_,
        .state = initial_state(),
        .snapshot_hybrid_time = snapshot_hybrid_time_,
      });
    });
  }

 private:
  bool IsTerminalFailure(const Status& status) override {
    return TransactionError(status) == TransactionErrorCode::kSnapshotTooOld;
  }

  TxnSnapshotId id_;
  HybridTime snapshot_hybrid_time_;
  SysRowEntries entries_;
};

class RestorationState : public StateWithTablets {
 public:
  RestorationState(
      SnapshotCoordinatorContext* context, const TxnSnapshotRestorationId& restoration_id,
      SnapshotState* snapshot)
      : StateWithTablets(context, SysSnapshotEntryPB::RESTORING),
        restoration_id_(restoration_id), snapshot_id_(snapshot->id()) {
    InitTabletIds(snapshot->TabletIdsInState(SysSnapshotEntryPB::COMPLETE));
  }

  const TxnSnapshotRestorationId& restoration_id() const {
    return restoration_id_;
  }

  const TxnSnapshotId& snapshot_id() const {
    return snapshot_id_;
  }

  CHECKED_STATUS ToPB(SnapshotInfoPB* out) {
    out->set_id(restoration_id_.data(), restoration_id_.size());
    auto& entry = *out->mutable_entry();

    entry.set_state(VERIFY_RESULT(AggregatedState()));

    TabletsToPB(entry.mutable_tablet_snapshots());

    return Status::OK();
  }

  TabletInfos PrepareOperations() {
    std::vector<TabletId> tablet_ids;
    DoPrepareOperations([&tablet_ids](const TabletData& data) {
      tablet_ids.push_back(data.id);
    });
    return context().GetTabletInfos(tablet_ids);
  }

 private:
  bool IsTerminalFailure(const Status& status) override {
    return false;
  }

  TxnSnapshotRestorationId restoration_id_;
  TxnSnapshotId snapshot_id_;
};

struct NoOp {
  template <class... Args>
  void operator()(Args&&... args) const {}
};

// Utility to create callback that is invoked when operation done.
// Finds appropriate entry in passed collection and invokes Done on it.
template <class Collection, class PostProcess = NoOp>
auto MakeDoneCallback(
    std::mutex* mutex, const Collection& collection, const typename Collection::key_type& key,
    const TabletId& tablet_id, const PostProcess& post_process = PostProcess()) {
  struct DoneFunctor {
    std::mutex& mutex;
    const Collection& collection;
    typename Collection::key_type key;
    TabletId tablet_id;
    PostProcess post_process;

    void operator()(Result<const tserver::TabletSnapshotOpResponsePB&> resp) const {
      std::unique_lock<std::mutex> lock(mutex);
      auto it = collection.find(key);
      if (it == collection.end()) {
        LOG(DFATAL) << "Received reply for unknown " << key;
        return;
      }

      it->second->Done(tablet_id, ResultToStatus(resp));
      post_process(it->second.get(), &lock);
    }
  };

  return DoneFunctor {
    .mutex = *mutex,
    .collection = collection,
    .key = key,
    .tablet_id = tablet_id,
    .post_process = post_process,
  };
}

auto SnapshotUpdater(SnapshotCoordinatorContext* context) {
  struct UpdateFunctor {
    SnapshotCoordinatorContext& context;

    void operator()(SnapshotState* snapshot, std::unique_lock<std::mutex>* lock) const {
      if (!snapshot->AllTabletsDone()) {
        return;
      }
      docdb::KeyValueWriteBatchPB write_batch;
      auto status = snapshot->StoreToWriteBatch(&write_batch);
      if (!status.ok()) {
        LOG(DFATAL) << "Failed to prepare write batch for snapshot: " << status;
        return;
      }
      lock->unlock();

      tserver::WriteRequestPB empty_write_request;
      auto state = std::make_unique<tablet::WriteOperationState>(
          /* tablet= */ nullptr, &empty_write_request);
      auto& request = *state->mutable_request();
      *request.mutable_write_batch() = write_batch;
      auto operation = std::make_unique<tablet::WriteOperation>(
          std::move(state), yb::OpId::kUnknownTerm, ScopedOperation(),
          CoarseMonoClock::now() + FLAGS_sys_catalog_write_timeout_ms * 1ms, /* context */ nullptr);
      context.Submit(std::move(operation));
    }
  };

  return UpdateFunctor {
    .context = *context
  };
}

} // namespace

class MasterSnapshotCoordinator::Impl {
 public:
  explicit Impl(SnapshotCoordinatorContext* context)
      : context_(*context), poller_(std::bind(&Impl::Poll, this)) {}

  Result<TxnSnapshotId> Create(
      const SysRowEntries& entries, bool imported, HybridTime snapshot_hybrid_time,
      CoarseTimePoint deadline) {
    auto synchronizer = std::make_shared<Synchronizer>();
    auto operation_state = std::make_unique<tablet::SnapshotOperationState>(/* tablet= */ nullptr);
    auto request = operation_state->AllocateRequest();

    for (const auto& entry : entries.entries()) {
      if (entry.type() == SysRowEntry::TABLET) {
        request->add_tablet_id(entry.id());
      }
    }

    request->set_snapshot_hybrid_time(snapshot_hybrid_time.ToUint64());
    request->set_operation(tserver::TabletSnapshotOpRequestPB::CREATE_ON_MASTER);
    auto snapshot_id = TxnSnapshotId::GenerateRandom();
    request->set_snapshot_id(snapshot_id.data(), snapshot_id.size());
    request->set_imported(imported);

    request->mutable_extra_data()->PackFrom(entries);

    operation_state->set_completion_callback(std::make_unique<
        tablet::WeakSynchronizerOperationCompletionCallback>(synchronizer));
    auto operation = std::make_unique<tablet::SnapshotOperation>(std::move(operation_state));

    context_.Submit(std::move(operation));
    RETURN_NOT_OK(synchronizer->WaitUntil(ToSteady(deadline)));

    return snapshot_id;
  }

  CHECKED_STATUS CreateReplicated(
      int64_t leader_term, const tablet::SnapshotOperationState& state) {
    // TODO(txn_backup) retain logs with this operation while doing snapshot
    auto id = VERIFY_RESULT(FullyDecodeTxnSnapshotId(state.request()->snapshot_id()));

    VLOG(1) << __func__ << "(" << id << ", " << state.ToString() << ")";

    auto snapshot = std::make_unique<SnapshotState>(&context_, id, *state.request());

    TabletSnapshotOperations operations;
    docdb::KeyValueWriteBatchPB write_batch;
    RETURN_NOT_OK(snapshot->StoreToWriteBatch(&write_batch));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto emplace_result = snapshots_.emplace(id, std::move(snapshot));
      if (!emplace_result.second) {
        return STATUS_FORMAT(IllegalState, "Duplicate snapshot id: $0", id);
      }

      if (leader_term >= 0) {
        emplace_result.first->second->PrepareOperations(&operations);
      }
    }

    RETURN_NOT_OK(state.tablet()->ApplyOperationState(state, /* batch_idx= */ -1, write_batch));

    ExecuteOperations(operations);

    return Status::OK();
  }

  CHECKED_STATUS Load(const TxnSnapshotId& snapshot_id, const SysSnapshotEntryPB& data) {
    VLOG(1) << __func__ << "(" << snapshot_id << ", " << data.ShortDebugString() << ")";

    auto snapshot = std::make_unique<SnapshotState>(&context_, snapshot_id, data);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = snapshots_.find(snapshot_id);
    if (it == snapshots_.end()) {
      snapshots_.emplace(snapshot_id, std::move(snapshot));
    } else {
      // During sys catalog bootstrap we replay WAL, that could contain "create snapshot" operation.
      // After that we load sys catalog, that contain those snapshots operations.
      // So we could see that same snapshot operation from 2 sources.
      //   1) Raft logs
      //   2) Stored in system catalog
      //
      // We should use latter, because they could contain updates.
      it->second = std::move(snapshot);
    }

    return Status::OK();
  }

  CHECKED_STATUS ListSnapshots(const TxnSnapshotId& snapshot_id, ListSnapshotsResponsePB* resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_id.IsNil()) {
      for (const auto& p : snapshots_) {
        // Do not list deleted snapshots.
        auto aggreaged_state = p.second->AggregatedState();
        if (!aggreaged_state.ok() || *aggreaged_state != SysSnapshotEntryPB::DELETED) {
          RETURN_NOT_OK(p.second->ToPB(resp->add_snapshots()));
        }
      }
      return Status::OK();
    }

    SnapshotState& snapshot = VERIFY_RESULT(FindSnapshot(snapshot_id));
    return snapshot.ToPB(resp->add_snapshots());
  }

  CHECKED_STATUS Delete(const TxnSnapshotId& snapshot_id, CoarseTimePoint deadline) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      SnapshotState& snapshot = VERIFY_RESULT(FindSnapshot(snapshot_id));
      RETURN_NOT_OK(snapshot.CheckCanDelete());
    }

    auto synchronizer = std::make_shared<Synchronizer>();
    auto operation_state = std::make_unique<tablet::SnapshotOperationState>(nullptr);
    auto request = operation_state->AllocateRequest();

    request->set_operation(tserver::TabletSnapshotOpRequestPB::DELETE_ON_MASTER);
    request->set_snapshot_id(snapshot_id.data(), snapshot_id.size());

    operation_state->set_completion_callback(std::make_unique<
        tablet::WeakSynchronizerOperationCompletionCallback>(synchronizer));
    auto operation = std::make_unique<tablet::SnapshotOperation>(std::move(operation_state));

    context_.Submit(std::move(operation));
    RETURN_NOT_OK(synchronizer->WaitUntil(ToSteady(deadline)));
    return Status::OK();
  }

  CHECKED_STATUS DeleteReplicated(
      int64_t leader_term, const tablet::SnapshotOperationState& state) {
    auto snapshot_id = VERIFY_RESULT(FullyDecodeTxnSnapshotId(state.request()->snapshot_id()));
    docdb::KeyValueWriteBatchPB write_batch;
    TabletSnapshotOperations operations;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      SnapshotState& snapshot = VERIFY_RESULT(FindSnapshot(snapshot_id));
      snapshot.SetInitialTabletsState(SysSnapshotEntryPB::DELETING);
      RETURN_NOT_OK(snapshot.StoreToWriteBatch(&write_batch));
      if (leader_term >= 0) {
        snapshot.PrepareOperations(&operations);
      }
    }

    RETURN_NOT_OK(state.tablet()->ApplyOperationState(state, /* batch_idx= */ -1, write_batch));

    ExecuteOperations(operations);

    return Status::OK();
  }

  CHECKED_STATUS ListRestorations(
      const TxnSnapshotRestorationId& restoration_id, const TxnSnapshotId& snapshot_id,
      ListSnapshotRestorationsResponsePB* resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!restoration_id) {
      for (const auto& p : restorations_) {
        if (!snapshot_id || p.second->snapshot_id() == snapshot_id) {
          RETURN_NOT_OK(p.second->ToPB(resp->add_restorations()));
        }
      }
      return Status::OK();
    }

    RestorationState& restoration = VERIFY_RESULT(FindRestoration(restoration_id));
    return restoration.ToPB(resp->add_restorations());
  }

  Result<TxnSnapshotRestorationId> Restore(const TxnSnapshotId& snapshot_id) {
    auto restoration_id = TxnSnapshotRestorationId::GenerateRandom();
    TabletInfos tablet_infos;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      SnapshotState& snapshot = VERIFY_RESULT(FindSnapshot(snapshot_id));
      if (!VERIFY_RESULT(snapshot.Complete())) {
        return STATUS(IllegalState, "The snapshot state is not complete", snapshot_id.ToString(),
                      MasterError(MasterErrorPB::SNAPSHOT_IS_NOT_READY));
      }

      auto restoration = std::make_unique<RestorationState>(&context_, restoration_id, &snapshot);
      tablet_infos = restoration->PrepareOperations();
      restorations_.emplace(restoration_id, std::move(restoration));
    }

    auto snapshot_id_str = snapshot_id.AsSlice().ToBuffer();
    for (const auto& tablet : tablet_infos) {
      context_.SendRestoreTabletSnapshotRequest(
          tablet, snapshot_id_str,
          MakeDoneCallback(&mutex_, restorations_, restoration_id, tablet->tablet_id()));
    }

    return restoration_id;
  }

  void Start() {
    poller_.Start(&context_.Scheduler(), FLAGS_snapshot_coordinator_poll_interval_ms * 1ms);
  }

  void Shutdown() {
    poller_.Shutdown();
  }

 private:
  Result<SnapshotState&> FindSnapshot(const TxnSnapshotId& snapshot_id) REQUIRES(mutex_) {
    auto it = snapshots_.find(snapshot_id);
    if (it == snapshots_.end()) {
      return STATUS(NotFound, "Could not find snapshot", snapshot_id.ToString(),
                    MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
    }
    return *it->second;
  }

  Result<RestorationState&> FindRestoration(
      const TxnSnapshotRestorationId& restoration_id) REQUIRES(mutex_) {
    auto it = restorations_.find(restoration_id);
    if (it == restorations_.end()) {
      return STATUS(NotFound, "Could not find restoration", restoration_id.ToString(),
                    MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
    return *it->second;
  }

  void ExecuteOperations(const TabletSnapshotOperations& operations) {
    if (operations.empty()) {
      return;
    }
    VLOG(4) << __func__ << "(" << AsString(operations) << ")";

    size_t num_operations = operations.size();
    std::vector<TabletId> tablet_ids;
    tablet_ids.reserve(num_operations);
    for (const auto& operation : operations) {
      tablet_ids.push_back(operation.tablet_id);
    }
    auto tablet_infos = context_.GetTabletInfos(tablet_ids);
    for (size_t i = 0; i != num_operations; ++i) {
      ExecuteOperation(operations[i], tablet_infos[i]);
    }
  }

  void ExecuteOperation(
      const TabletSnapshotOperation& operation, const TabletInfoPtr& tablet_info) {
    auto callback = MakeDoneCallback(
        &mutex_, snapshots_, operation.snapshot_id, operation.tablet_id,
        SnapshotUpdater(&context_));
    auto snapshot_id_str = operation.snapshot_id.AsSlice().ToBuffer();

    if (operation.state == SysSnapshotEntryPB::DELETING) {
      context_.SendDeleteTabletSnapshotRequest(
          tablet_info, snapshot_id_str, callback);
    } else if (operation.state == SysSnapshotEntryPB::CREATING) {
      context_.SendCreateTabletSnapshotRequest(
          tablet_info, snapshot_id_str, operation.snapshot_hybrid_time, callback);
    } else {
      LOG(DFATAL) << "Unsupported snapshot operation: " << operation.ToString();
    }
  }

  void Poll() {
    if (!context_.IsLeader()) {
      return;
    }
    VLOG(4) << __func__ << "()";
    TabletSnapshotOperations operations;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& p : snapshots_) {
        p.second->PrepareOperations(&operations);
      }
    }
    ExecuteOperations(operations);
  }

  SnapshotCoordinatorContext& context_;
  std::mutex mutex_;
  std::unordered_map<TxnSnapshotId, std::unique_ptr<SnapshotState>,
                     TxnSnapshotIdHash> snapshots_ GUARDED_BY(mutex_);
  std::unordered_map<TxnSnapshotRestorationId, std::unique_ptr<RestorationState>,
                     TxnSnapshotRestorationIdHash> restorations_ GUARDED_BY(mutex_);
  rpc::Poller poller_;
};

MasterSnapshotCoordinator::MasterSnapshotCoordinator(SnapshotCoordinatorContext* context)
    : impl_(new Impl(context)) {}

MasterSnapshotCoordinator::~MasterSnapshotCoordinator() {}

Result<TxnSnapshotId> MasterSnapshotCoordinator::Create(
    const SysRowEntries& entries, bool imported, HybridTime snapshot_hybrid_time,
    CoarseTimePoint deadline) {
  return impl_->Create(entries, imported, snapshot_hybrid_time, deadline);
}

Status MasterSnapshotCoordinator::CreateReplicated(
    int64_t leader_term, const tablet::SnapshotOperationState& state) {
  return impl_->CreateReplicated(leader_term, state);
}

Status MasterSnapshotCoordinator::DeleteReplicated(
    int64_t leader_term, const tablet::SnapshotOperationState& state) {
  return impl_->DeleteReplicated(leader_term, state);
}

Status MasterSnapshotCoordinator::ListSnapshots(
    const TxnSnapshotId& snapshot_id, ListSnapshotsResponsePB* resp) {
  return impl_->ListSnapshots(snapshot_id, resp);
}

Status MasterSnapshotCoordinator::Delete(
    const TxnSnapshotId& snapshot_id, CoarseTimePoint deadline) {
  return impl_->Delete(snapshot_id, deadline);
}

Result<TxnSnapshotRestorationId> MasterSnapshotCoordinator::Restore(
    const TxnSnapshotId& snapshot_id) {
  return impl_->Restore(snapshot_id);
}

Status MasterSnapshotCoordinator::ListRestorations(
    const TxnSnapshotRestorationId& restoration_id, const TxnSnapshotId& snapshot_id,
    ListSnapshotRestorationsResponsePB* resp) {
  return impl_->ListRestorations(restoration_id, snapshot_id, resp);
}

Status MasterSnapshotCoordinator::Load(const TxnSnapshotId& id, const SysSnapshotEntryPB& data) {
  return impl_->Load(id, data);
}

void MasterSnapshotCoordinator::Start() {
  impl_->Start();
}

void MasterSnapshotCoordinator::Shutdown() {
  impl_->Shutdown();
}

} // namespace master
} // namespace yb
