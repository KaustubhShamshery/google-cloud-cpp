// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/bigtable/table.h"
#include "google/cloud/bigtable/internal/async_retry_unary_rpc.h"
#include "google/cloud/bigtable/internal/bulk_mutator.h"
#include "google/cloud/bigtable/internal/grpc_error_delegate.h"
#include "google/cloud/bigtable/internal/unary_client_utils.h"
#include <thread>
#include <type_traits>

namespace btproto = ::google::bigtable::v2;
namespace google {
namespace cloud {
namespace bigtable {
inline namespace BIGTABLE_CLIENT_NS {
using ClientUtils = bigtable::internal::noex::UnaryClientUtils<DataClient>;
static_assert(std::is_copy_assignable<bigtable::Table>::value,
              "bigtable::Table must be CopyAssignable");

Status Table::Apply(SingleRowMutation mut) {
  // Copy the policies in effect for this operation.  Many policy classes change
  // their state as the operation makes progress (or fails to make progress), so
  // we need fresh instances.
  auto rpc_policy = impl_.rpc_retry_policy_->clone();
  auto backoff_policy = impl_.rpc_backoff_policy_->clone();
  auto idempotent_policy = impl_.idempotent_mutation_policy_->clone();

  // Build the RPC request, try to minimize copying.
  btproto::MutateRowRequest request;
  bigtable::internal::SetCommonTableOperationRequest<btproto::MutateRowRequest>(
      request, impl_.app_profile_id_.get(), impl_.table_name_.get());
  mut.MoveTo(request);

  bool const is_idempotent =
      std::all_of(request.mutations().begin(), request.mutations().end(),
                  [&idempotent_policy](btproto::Mutation const& m) {
                    return idempotent_policy->is_idempotent(m);
                  });

  btproto::MutateRowResponse response;
  grpc::Status status;
  while (true) {
    grpc::ClientContext client_context;
    rpc_policy->Setup(client_context);
    backoff_policy->Setup(client_context);
    impl_.metadata_update_policy_.Setup(client_context);
    status = impl_.client_->MutateRow(&client_context, request, &response);

    if (status.ok()) {
      return google::cloud::Status{};
    }
    // It is up to the policy to terminate this loop, it could run
    // forever, but that would be a bad policy (pun intended).
    if (!rpc_policy->OnFailure(status) || !is_idempotent) {
      return bigtable::internal::MakeStatusFromRpcError(
          status.error_code(),
          "Permanent (or too many transient) errors in Table::Apply()");
    }
    auto delay = backoff_policy->OnCompletion(status);
    std::this_thread::sleep_for(delay);
  }
}

future<Status> Table::AsyncApply(SingleRowMutation mut, CompletionQueue& cq) {
  google::bigtable::v2::MutateRowRequest request;
  internal::SetCommonTableOperationRequest<
      google::bigtable::v2::MutateRowRequest>(
      request, impl_.app_profile_id_.get(), impl_.table_name_.get());
  mut.MoveTo(request);
  auto context = google::cloud::internal::make_unique<grpc::ClientContext>();

  // Determine if all the mutations are idempotent. The idempotency of the
  // mutations won't change as the retry loop executes, so we can just compute
  // it once and use a constant value for the loop.
  auto idempotent_mutation_policy = clone_idempotent_mutation_policy();
  bool const is_idempotent = std::all_of(
      request.mutations().begin(), request.mutations().end(),
      [&idempotent_mutation_policy](google::bigtable::v2::Mutation const& m) {
        return idempotent_mutation_policy->is_idempotent(m);
      });

  auto client = impl_.client_;
  return internal::StartRetryAsyncUnaryRpc(
             __func__, clone_rpc_retry_policy(), clone_rpc_backoff_policy(),
             internal::ConstantIdempotencyPolicy(is_idempotent),
             clone_metadata_update_policy(),
             [client](grpc::ClientContext* context,
                      google::bigtable::v2::MutateRowRequest const& request,
                      grpc::CompletionQueue* cq) {
               return client->AsyncMutateRow(context, request, cq);
             },
             std::move(request), cq)
      .then([](future<StatusOr<google::bigtable::v2::MutateRowResponse>> r) {
        return r.get().status();
      });
}

std::vector<FailedMutation> Table::BulkApply(BulkMutation mut) {
  grpc::Status status;

  // Copy the policies in effect for this operation.  Many policy classes change
  // their state as the operation makes progress (or fails to make progress), so
  // we need fresh instances.
  auto backoff_policy = impl_.rpc_backoff_policy_->clone();
  auto retry_policy = impl_.rpc_retry_policy_->clone();
  auto idemponent_policy = impl_.idempotent_mutation_policy_->clone();

  bigtable::internal::BulkMutator mutator(impl_.app_profile_id_,
                                          impl_.table_name_, *idemponent_policy,
                                          std::move(mut));
  while (mutator.HasPendingMutations()) {
    grpc::ClientContext client_context;
    backoff_policy->Setup(client_context);
    retry_policy->Setup(client_context);
    impl_.metadata_update_policy_.Setup(client_context);
    status = mutator.MakeOneRequest(*impl_.client_, client_context);
    if (!status.ok() && !retry_policy->OnFailure(status)) {
      break;
    }
    auto delay = backoff_policy->OnCompletion(status);
    std::this_thread::sleep_for(delay);
  }
  return std::move(mutator).OnRetryDone();
}

future<std::vector<FailedMutation>> Table::AsyncBulkApply(BulkMutation mut,
                                                          CompletionQueue& cq) {
  auto mutation_policy = clone_idempotent_mutation_policy();
  return internal::AsyncRetryBulkApply::Create(
      cq, clone_rpc_retry_policy(), clone_rpc_backoff_policy(),
      *mutation_policy, clone_metadata_update_policy(), impl_.client_,
      impl_.app_profile_id_, bigtable::TableId(impl_.table_name()),
      std::move(mut));
}

RowReader Table::ReadRows(RowSet row_set, Filter filter) {
  return RowReader(impl_.client_, impl_.app_profile_id_, impl_.table_name_,
                   std::move(row_set), RowReader::NO_ROWS_LIMIT,
                   std::move(filter), impl_.rpc_retry_policy_->clone(),
                   impl_.rpc_backoff_policy_->clone(),
                   impl_.metadata_update_policy_,
                   google::cloud::internal::make_unique<
                       bigtable::internal::ReadRowsParserFactory>());
}

RowReader Table::ReadRows(RowSet row_set, std::int64_t rows_limit,
                          Filter filter) {
  return RowReader(impl_.client_, impl_.app_profile_id_, impl_.table_name_,
                   std::move(row_set), rows_limit, std::move(filter),
                   impl_.rpc_retry_policy_->clone(),
                   impl_.rpc_backoff_policy_->clone(),
                   impl_.metadata_update_policy_,
                   google::cloud::internal::make_unique<
                       bigtable::internal::ReadRowsParserFactory>());
}

StatusOr<std::pair<bool, Row>> Table::ReadRow(std::string row_key,
                                              Filter filter) {
  RowSet row_set(std::move(row_key));
  std::int64_t const rows_limit = 1;
  RowReader reader =
      ReadRows(std::move(row_set), rows_limit, std::move(filter));

  auto it = reader.begin();
  if (it == reader.end()) {
    return std::make_pair(false, Row("", {}));
  }
  if (!*it) {
    return it->status();
  }
  auto result = std::make_pair(true, std::move(**it));
  if (++it != reader.end()) {
    return Status(StatusCode::kInternal,
                  "internal error - RowReader returned 2 rows in ReadRow()");
  }
  return result;
}

StatusOr<bool> Table::CheckAndMutateRow(std::string row_key, Filter filter,
                                        std::vector<Mutation> true_mutations,
                                        std::vector<Mutation> false_mutations) {
  grpc::Status status;
  btproto::CheckAndMutateRowRequest request;
  request.set_row_key(std::move(row_key));
  bigtable::internal::SetCommonTableOperationRequest<
      btproto::CheckAndMutateRowRequest>(request, impl_.app_profile_id_.get(),
                                         impl_.table_name_.get());
  *request.mutable_predicate_filter() = std::move(filter).as_proto();
  for (auto& m : true_mutations) {
    *request.add_true_mutations() = std::move(m.op);
  }
  for (auto& m : false_mutations) {
    *request.add_false_mutations() = std::move(m.op);
  }
  bool const is_idempotent =
      impl_.idempotent_mutation_policy_->is_idempotent(request);
  auto response = ClientUtils::MakeCall(
      *impl_.client_, impl_.rpc_retry_policy_->clone(),
      impl_.rpc_backoff_policy_->clone(), impl_.metadata_update_policy_,
      &DataClient::CheckAndMutateRow, request, "Table::CheckAndMutateRow",
      status, is_idempotent);

  if (!status.ok()) {
    return bigtable::internal::MakeStatusFromRpcError(status);
  }
  return response.predicate_matched();
}

future<StatusOr<bool>> Table::AsyncCheckAndMutateRow(
    std::string row_key, Filter filter, std::vector<Mutation> true_mutations,
    std::vector<Mutation> false_mutations, CompletionQueue& cq) {
  btproto::CheckAndMutateRowRequest request;
  request.set_row_key(std::move(row_key));
  bigtable::internal::SetCommonTableOperationRequest<
      btproto::CheckAndMutateRowRequest>(request, impl_.app_profile_id_.get(),
                                         impl_.table_name_.get());
  *request.mutable_predicate_filter() = std::move(filter).as_proto();
  for (auto& m : true_mutations) {
    *request.add_true_mutations() = std::move(m.op);
  }
  for (auto& m : false_mutations) {
    *request.add_false_mutations() = std::move(m.op);
  }
  bool const is_idempotent =
      impl_.idempotent_mutation_policy_->is_idempotent(request);

  auto client = impl_.client_;
  return internal::StartRetryAsyncUnaryRpc(
             __func__, clone_rpc_retry_policy(), clone_rpc_backoff_policy(),
             internal::ConstantIdempotencyPolicy(is_idempotent),
             clone_metadata_update_policy(),
             [client](grpc::ClientContext* context,
                      btproto::CheckAndMutateRowRequest const& request,
                      grpc::CompletionQueue* cq) {
               return client->AsyncCheckAndMutateRow(context, request, cq);
             },
             std::move(request), cq)
      .then([](future<StatusOr<btproto::CheckAndMutateRowResponse>> f)
                -> StatusOr<bool> {
        auto response = f.get();
        if (!response) {
          return response.status();
        }
        return response->predicate_matched();
      });
}

// Call the `google.bigtable.v2.Bigtable.SampleRowKeys` RPC until
// successful. When RPC is finished, this function returns the SampleRowKeys
// as a std::vector<>. If the RPC fails, it will keep retrying until the
// policies in effect tell us to stop. Note that each retry must clear the
// samples otherwise the result is an inconsistent set of samples row keys.
StatusOr<std::vector<bigtable::RowKeySample>> Table::SampleRows() {
  // Copy the policies in effect for this operation.
  auto backoff_policy = clone_rpc_backoff_policy();
  auto retry_policy = clone_rpc_retry_policy();
  std::vector<bigtable::RowKeySample> samples;

  // Build the RPC request for SampleRowKeys
  btproto::SampleRowKeysRequest request;
  btproto::SampleRowKeysResponse response;
  bigtable::internal::SetCommonTableOperationRequest<
      btproto::SampleRowKeysRequest>(request, impl_.app_profile_id_.get(),
                                     impl_.table_name_.get());

  while (true) {
    grpc::ClientContext client_context;
    backoff_policy->Setup(client_context);
    retry_policy->Setup(client_context);
    clone_metadata_update_policy().Setup(client_context);

    auto stream = impl_.client_->SampleRowKeys(&client_context, request);
    while (stream->Read(&response)) {
      bigtable::RowKeySample row_sample;
      row_sample.offset_bytes = response.offset_bytes();
      row_sample.row_key = std::move(*response.mutable_row_key());
      samples.emplace_back(std::move(row_sample));
    }
    auto status = stream->Finish();
    if (status.ok()) {
      break;
    }
    if (!retry_policy->OnFailure(status)) {
      return internal::MakeStatusFromRpcError(
          status.error_code(),
          "Retry policy exhausted: " + status.error_message());
    }
    samples.clear();
    auto delay = backoff_policy->OnCompletion(status);
    std::this_thread::sleep_for(delay);
  }
  return samples;
}

StatusOr<Row> Table::ReadModifyWriteRowImpl(
    btproto::ReadModifyWriteRowRequest request) {
  bigtable::internal::SetCommonTableOperationRequest<
      ::google::bigtable::v2::ReadModifyWriteRowRequest>(
      request, impl_.app_profile_id_.get(), impl_.table_name_.get());

  grpc::Status status;
  auto response = ClientUtils::MakeNonIdemponentCall(
      *(impl_.client_), clone_rpc_retry_policy(),
      clone_metadata_update_policy(), &DataClient::ReadModifyWriteRow, request,
      "ReadModifyWriteRowRequest", status);
  if (!status.ok()) {
    return internal::MakeStatusFromRpcError(status);
  }
  return internal::TransformReadModifyWriteRowResponse<
      btproto::ReadModifyWriteRowResponse>(response);
}

future<StatusOr<Row>> Table::AsyncReadModifyWriteRowImpl(
    CompletionQueue& cq,
    ::google::bigtable::v2::ReadModifyWriteRowRequest request) {
  bigtable::internal::SetCommonTableOperationRequest<
      ::google::bigtable::v2::ReadModifyWriteRowRequest>(
      request, impl_.app_profile_id_.get(), impl_.table_name_.get());

  auto client = impl_.client_;
  return internal::StartRetryAsyncUnaryRpc(
             __func__, clone_rpc_retry_policy(), clone_rpc_backoff_policy(),
             internal::ConstantIdempotencyPolicy(false),
             clone_metadata_update_policy(),
             [client](grpc::ClientContext* context,
                      btproto::ReadModifyWriteRowRequest const& request,
                      grpc::CompletionQueue* cq) {
               return client->AsyncReadModifyWriteRow(context, request, cq);
             },
             std::move(request), cq)
      .then([](future<StatusOr<btproto::ReadModifyWriteRowResponse>> fut)
                -> StatusOr<Row> {
        auto result = fut.get();
        if (!result) {
          return result.status();
        }
        return internal::TransformReadModifyWriteRowResponse<
            btproto::ReadModifyWriteRowResponse>(*result);
      });
}

}  // namespace BIGTABLE_CLIENT_NS
}  // namespace bigtable
}  // namespace cloud
}  // namespace google
