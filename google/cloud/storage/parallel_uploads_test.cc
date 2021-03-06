// Copyright 2019 Google LLC
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

#include "google/cloud/storage/oauth2/google_credentials.h"
#include "google/cloud/storage/parallel_upload.h"
#include "google/cloud/storage/retry_policy.h"
#include "google/cloud/storage/testing/canonical_errors.h"
#include "google/cloud/storage/testing/mock_client.h"
#include "google/cloud/storage/testing/random_names.h"
#include "google/cloud/storage/testing/retry_tests.h"
#include "google/cloud/storage/testing/temp_file.h"
#include "google/cloud/testing_util/assert_ok.h"
#include "google/cloud/testing_util/chrono_literals.h"
#include <gmock/gmock.h>
#include <cstdio>
#include <stack>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif  // __linux__

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {
namespace {

using namespace google::cloud::testing_util::chrono_literals;
using ::google::cloud::storage::testing::canonical_errors::PermanentError;
using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::ReturnRef;

std::string const kBucketName = "test-bucket";
std::string const kDestObjectName = "final-object";
int const kDestGeneration = 123;
int const kUploadMarkerGeneration = 234;
int const kComposeMarkerGeneration = 345;
int const kPersistentStateGeneration = 456;
std::string const kPrefix = "some-prefix";
std::string const kPersistentStateName = kPrefix + ".upload_state";
std::string const kParallelResumableId =
    "ParUpl:" + kPersistentStateName + ":" +
    std::to_string(kPersistentStateGeneration);
std::string const kIndividualSessionId = "some_session_id";

std::string ObjectId(std::string const& bucket, std::string const& object,
                     int generation) {
  return bucket + "/" + object + "/" + std::to_string(generation);
}

ObjectMetadata MockObject(std::string const& object_name, int generation) {
  auto metadata = internal::ObjectMetadataParser::FromJson(internal::nl::json{
      {"contentDisposition", "a-disposition"},
      {"contentLanguage", "a-language"},
      {"contentType", "application/octet-stream"},
      {"crc32c", "d1e2f3"},
      {"etag", "XYZ="},
      {"kind", "storage#object"},
      {"md5Hash", "xa1b2c3=="},
      {"mediaLink", "https://storage.googleapis.com/download/storage/v1/b/" +
                        kBucketName + "/o/" + object_name + "?generation=" +
                        std::to_string(generation) + "&alt=media"},
      {"metageneration", 4},
      {"selfLink", "https://storage.googleapis.com/storage/v1/b/" +
                       kBucketName + "/o/" + object_name},
      {"size", 1024},
      {"storageClass", "STANDARD"},
      {"timeCreated", "2018-05-19T19:31:14Z"},
      {"timeDeleted", "2018-05-19T19:32:24Z"},
      {"timeStorageClassUpdated", "2018-05-19T19:31:34Z"},
      {"updated", "2018-05-19T19:31:24Z"},
      {"bucket", kBucketName},
      {"generation", generation},
      {"id", ObjectId(kBucketName, object_name, generation)},
      {"name", object_name}});
  EXPECT_STATUS_OK(metadata);
  return *metadata;
}

class ExpectedDeletions {
 public:
  ExpectedDeletions(std::map<std::pair<std::string, int>, Status> expectations)
      : deletions_(std::move(expectations)) {}

  ~ExpectedDeletions() {
    std::lock_guard<std::mutex> lk(mu_);
    std::stringstream unsatisfied_stream;
    for (auto kv : deletions_) {
      unsatisfied_stream << " object_name=" << kv.first.first
                         << " gen=" << kv.first.second;
    }
    EXPECT_TRUE(deletions_.empty())
        << "Some expected deletions were not performed:"
        << unsatisfied_stream.str();
  }

  StatusOr<internal::EmptyResponse> operator()(
      internal::DeleteObjectRequest const& r) {
    EXPECT_TRUE(r.HasOption<IfGenerationMatch>());
    auto if_gen_match = r.GetOption<IfGenerationMatch>();
    auto status = RemoveExpectation(r.object_name(), if_gen_match.value());
    EXPECT_EQ(kBucketName, r.bucket_name());
    if (status.ok()) {
      return internal::EmptyResponse();
    }
    return status;
  };

 private:
  Status RemoveExpectation(std::string const& object_name, int generation) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = deletions_.find(std::make_pair(object_name, generation));
    if (it == deletions_.end()) {
      EXPECT_TRUE(false);
      return Status(StatusCode::kFailedPrecondition,
                    "Unexpected deletion. object=" + object_name +
                        " generation=" + std::to_string(generation));
    }
    auto res = std::move(it->second);
    deletions_.erase(it);
    return res;
  }
  std::mutex mu_;
  std::map<std::pair<std::string, int>, Status> deletions_;
};

class ParallelUploadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    raw_client_mock = std::make_shared<testing::MockClient>();
    EXPECT_CALL(*raw_client_mock, client_options())
        .WillRepeatedly(ReturnRef(client_options));
    client.reset(new Client{
        std::shared_ptr<internal::RawClient>(raw_client_mock),
        ExponentialBackoffPolicy(std::chrono::milliseconds(1),
                                 std::chrono::milliseconds(1), 2.0)});
  }
  void TearDown() override {
    client.reset();
    raw_client_mock.reset();
  }

  void ExpectCreateSessionFailure(
      std::string const& object_name, Status status,
      optional<std::string> const& resumable_session_id =
          optional<std::string>()) {
    EXPECT_FALSE(status.ok())
        << "Expect either a failure or an actual MockResumableUploadSession";
    session_mocks.emplace(std::move(status));
    AddNewExpectation(object_name, resumable_session_id);
  }

  testing::MockResumableUploadSession& ExpectCreateFailingSession(
      std::string const& object_name, Status status,
      optional<std::string> const& resumable_session_id =
          optional<std::string>()) {
    auto session = google::cloud::internal::make_unique<
        testing::MockResumableUploadSession>();
    auto& res = *session;
    session_mocks.emplace(std::move(session));
    using internal::ResumableUploadResponse;

    EXPECT_CALL(res, done()).WillRepeatedly(Return(false));
    static std::string session_id(kIndividualSessionId);
    EXPECT_CALL(res, session_id()).WillRepeatedly(ReturnRef(session_id));
    EXPECT_CALL(res, next_expected_byte()).WillRepeatedly(Return(0));
    EXPECT_CALL(res, UploadChunk(_))
        .WillRepeatedly(Return(make_status_or(ResumableUploadResponse{
            "fake-url", 0, {}, ResumableUploadResponse::kInProgress, {}})));
    EXPECT_CALL(res, UploadFinalChunk(_, _))
        .WillRepeatedly(Return(std::move(status)));
    AddNewExpectation(object_name, resumable_session_id);

    return res;
  }

  testing::MockResumableUploadSession& ExpectCreateSession(
      std::string const& object_name, int generation,
      optional<std::string> expected_content = optional<std::string>(),
      optional<std::string> const& resumable_session_id =
          optional<std::string>()) {
    auto session = google::cloud::internal::make_unique<
        testing::MockResumableUploadSession>();
    auto& res = *session;
    session_mocks.emplace(std::move(session));
    using internal::ResumableUploadResponse;

    EXPECT_CALL(res, done()).WillRepeatedly(Return(false));
    static std::string session_id(kIndividualSessionId);
    EXPECT_CALL(res, session_id()).WillRepeatedly(ReturnRef(session_id));
    EXPECT_CALL(res, next_expected_byte()).WillRepeatedly(Return(0));
    if (expected_content) {
      EXPECT_CALL(res, UploadFinalChunk(_, _))
          .WillOnce(
              Invoke([expected_content, object_name, generation](
                         std::string const& content, std::uint64_t /*size*/) {
                EXPECT_EQ(*expected_content, content);
                EXPECT_EQ(expected_content->size(), content.size());
                return make_status_or(
                    ResumableUploadResponse{"fake-url",
                                            0,
                                            MockObject(object_name, generation),
                                            ResumableUploadResponse::kDone,
                                            {}});
              }));
    } else {
      EXPECT_CALL(res, UploadFinalChunk(_, _))
          .WillOnce(Return(make_status_or(
              ResumableUploadResponse{"fake-url",
                                      0,
                                      MockObject(object_name, generation),
                                      ResumableUploadResponse::kDone,
                                      {}})));
    }
    AddNewExpectation(object_name, resumable_session_id);

    return res;
  }

  testing::MockResumableUploadSession& ExpectCreateSessionToSuspend(
      std::string const& object_name,
      optional<std::string> const& resumable_session_id =
          optional<std::string>()) {
    auto session = google::cloud::internal::make_unique<
        testing::MockResumableUploadSession>();
    auto& res = *session;
    session_mocks.emplace(std::move(session));
    using internal::ResumableUploadResponse;

    EXPECT_CALL(res, done()).WillRepeatedly(Return(false));
    static std::string session_id(kIndividualSessionId);
    EXPECT_CALL(res, session_id()).WillRepeatedly(ReturnRef(session_id));
    EXPECT_CALL(res, next_expected_byte()).WillRepeatedly(Return(0));
    AddNewExpectation(object_name, resumable_session_id);

    return res;
  }

  std::shared_ptr<testing::MockClient> raw_client_mock;
  std::unique_ptr<Client> client;
  std::stack<StatusOr<std::unique_ptr<internal::ResumableUploadSession>>>
      session_mocks;
  ClientOptions client_options =
      ClientOptions(oauth2::CreateAnonymousCredentials());

 private:
  void AddNewExpectation(std::string const& object_name,
                         optional<std::string> const& resumable_session_id =
                             optional<std::string>()) {
    EXPECT_CALL(*raw_client_mock, CreateResumableSession(_))
        .WillOnce(Invoke(
            [this, object_name, resumable_session_id](
                internal::ResumableUploadRequest const& request)
                -> StatusOr<std::unique_ptr<internal::ResumableUploadSession>> {
              EXPECT_EQ(object_name, request.object_name());
              EXPECT_EQ(kBucketName, request.bucket_name());

              if (resumable_session_id) {
                EXPECT_TRUE(request.HasOption<UseResumableUploadSession>());
                auto actual_resumable_session_id =
                    request.GetOption<UseResumableUploadSession>();
                EXPECT_EQ(*resumable_session_id,
                          actual_resumable_session_id.value());
              }

              auto res = std::move(session_mocks.top());
              session_mocks.pop();
              return res;
            }))
        .RetiresOnSaturation();
  }
};

auto create_composition_check =
    [](std::vector<std::pair<std::string, int>> source_objects,
       std::string const& dest_obj, StatusOr<ObjectMetadata> res,
       optional<std::int64_t> expected_if_gen_match =
           optional<std::int64_t>()) {
      internal::nl::json json_source_objects;
      for (auto& obj : source_objects) {
        json_source_objects.emplace_back(internal::nl::json{
            {"name", std::move(obj.first)}, {"generation", obj.second}});
      }

      internal::nl::json expected_payload = {
          {"kind", "storage#composeRequest"},
          {"sourceObjects", json_source_objects}};
      return [dest_obj, res, expected_payload,
              expected_if_gen_match](internal::ComposeObjectRequest const& r) {
        EXPECT_EQ(kBucketName, r.bucket_name());
        EXPECT_EQ(dest_obj, r.object_name());
        internal::nl::json actual_payload =
            internal::nl::json::parse(r.JsonPayload());
        EXPECT_EQ(expected_payload, actual_payload);
        if (expected_if_gen_match) {
          EXPECT_TRUE(r.HasOption<IfGenerationMatch>());
          auto if_gen_match = r.GetOption<IfGenerationMatch>();
          EXPECT_EQ(*expected_if_gen_match, if_gen_match.value());
        }
        return res;
      };
    };

template <typename T>
bool Unsatisfied(future<T> const& fut) {
  return std::future_status::timeout == fut.wait_for(1_ms);
}

auto expect_deletion = [](std::string const& name, int generation) {
  return [name, generation](internal::DeleteObjectRequest const& r) {
    EXPECT_EQ(kBucketName, r.bucket_name());
    EXPECT_EQ(name, r.object_name());
    EXPECT_TRUE(r.HasOption<IfGenerationMatch>());
    EXPECT_EQ(generation, r.GetOption<IfGenerationMatch>().value());
    return make_status_or(internal::EmptyResponse{});
  };
};

auto expect_new_object = [](std::string const& object_name, int generation) {
  return [object_name,
          generation](internal::InsertObjectMediaRequest const& request) {
    EXPECT_EQ(kBucketName, request.bucket_name());
    EXPECT_EQ(object_name, request.object_name());
    EXPECT_EQ("", request.contents());
    return make_status_or(MockObject(object_name, generation));
  };
};

auto expect_persistent_state = [](std::string const& state_name, int generation,
                                  internal::nl::json const& state) {
  return [state_name, generation,
          state](internal::InsertObjectMediaRequest const& request) {
    EXPECT_EQ(kBucketName, request.bucket_name());
    EXPECT_EQ(state_name, request.object_name());
    EXPECT_EQ(state.dump(), request.contents());
    return make_status_or(MockObject(state_name, generation));
  };
};

auto create_state_read_expectation = [](std::string const state_object,
                                        std::int64_t generation,
                                        internal::nl::json const& state) {
  return [state_object, generation, state](ReadObjectRangeRequest req) {
    EXPECT_EQ(state_object, req.object_name());
    EXPECT_TRUE(req.HasOption<IfGenerationMatch>());
    auto if_gen_match = req.GetOption<IfGenerationMatch>();
    EXPECT_EQ(generation, if_gen_match.value());
    auto res =
        google::cloud::internal::make_unique<testing::MockObjectReadSource>();
    EXPECT_CALL(*res, Read(_, _))
        .WillOnce(Invoke([state](char* buf, std::size_t n) {
          auto state_str = state.dump();
          EXPECT_GE(n, state_str.size());
          memcpy(buf, state_str.c_str(), state_str.size());
          return ReadSourceResult{static_cast<std::size_t>(state_str.size()),
                                  HttpResponse{200, "", {}}};
        }))
        .WillOnce(Return(ReadSourceResult{0, HttpResponse{200, "", {}}}));
    EXPECT_CALL(*res, IsOpen()).WillRepeatedly(Return(true));
    EXPECT_CALL(*res, Close())
        .WillRepeatedly(Return(HttpResponse{200, "", {}}));
    return StatusOr<std::unique_ptr<ObjectReadSource>>(std::move(res));
  };
};

TEST_F(ParallelUploadTest, Success) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333);
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222);
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222},
           {kPrefix + ".upload_shard_2", 333}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto state = PrepareParallelUpload(*client, kBucketName, kDestObjectName,
                                     num_shards, kPrefix);
  EXPECT_STATUS_OK(state);
  auto res_future = state->WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  auto cleanup_too_early = state->EagerCleanup();
  ASSERT_FALSE(cleanup_too_early.ok());
  EXPECT_EQ(StatusCode::kFailedPrecondition, cleanup_too_early.code());
  EXPECT_THAT(cleanup_too_early.message(), HasSubstr("still in progress"));

  state->shards().clear();
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);

  EXPECT_STATUS_OK(state->EagerCleanup());
  EXPECT_STATUS_OK(state->EagerCleanup());
}

TEST_F(ParallelUploadTest, OneStreamFailsUponCration) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSessionFailure(kPrefix + ".upload_shard_1", PermanentError());
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto state = PrepareParallelUpload(*client, kBucketName, kDestObjectName,
                                     num_shards, kPrefix);
  EXPECT_FALSE(state);
  EXPECT_EQ(PermanentError().code(), state.status().code());
}

TEST_F(ParallelUploadTest, CleanupFailsEager) {
  int const num_shards = 1;
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));

  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111}}, kDestObjectName,
          MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions(
      {{{kPrefix + ".upload_shard_0", 111}, PermanentError()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }));

  auto state = PrepareParallelUpload(*client, kBucketName, kDestObjectName,
                                     num_shards, kPrefix);
  EXPECT_STATUS_OK(state);

  auto cleanup_too_early = state->EagerCleanup();
  ASSERT_FALSE(cleanup_too_early.ok());
  EXPECT_EQ(StatusCode::kFailedPrecondition, cleanup_too_early.code());
  EXPECT_THAT(cleanup_too_early.message(), HasSubstr("still in progress"));

  state->shards().clear();
  auto res = state->WaitForCompletion().get();
  EXPECT_STATUS_OK(res);

  auto cleanup_status = state->EagerCleanup();
  EXPECT_FALSE(cleanup_status.ok());
  EXPECT_EQ(PermanentError().code(), cleanup_status.code());
  EXPECT_EQ(cleanup_status, state->EagerCleanup());
  EXPECT_EQ(cleanup_status, state->EagerCleanup());
}

TEST_F(ParallelUploadTest, CleanupFailsInDtor) {
  int const num_shards = 1;
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));

  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111}}, kDestObjectName,
          MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions(
      {{{kPrefix + ".upload_shard_0", 111}, PermanentError()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }));

  auto state = PrepareParallelUpload(*client, kBucketName, kDestObjectName,
                                     num_shards, kPrefix);
  EXPECT_STATUS_OK(state);
}

TEST_F(ParallelUploadTest, BrokenStream) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333);
  ExpectCreateFailingSession(kPrefix + ".upload_shard_1", PermanentError());
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto state = PrepareParallelUpload(*client, kBucketName, kDestObjectName,
                                     num_shards, kPrefix);
  EXPECT_STATUS_OK(state);

  state->shards().clear();
  auto res = state->WaitForCompletion().get();
  EXPECT_FALSE(res);
  EXPECT_EQ(PermanentError().code(), res.status().code());
}

TEST(FirstOccurenceTest, Basic) {
  EXPECT_EQ(optional<std::string>(),
            ExtractFirstOccurenceOfType<std::string>(std::tuple<>()));
  EXPECT_EQ(optional<std::string>(),
            ExtractFirstOccurenceOfType<std::string>(std::make_tuple(5, 5.5)));
  EXPECT_EQ(optional<std::string>("foo"),
            ExtractFirstOccurenceOfType<std::string>(
                std::make_tuple(std::string("foo"), std::string("bar"))));
  EXPECT_EQ(optional<std::string>("foo"),
            ExtractFirstOccurenceOfType<std::string>(
                std::make_tuple(5, 6, std::string("foo"), std::string("bar"))));
}

TEST_F(ParallelUploadTest, FileSuccessWithMaxStreamsNotReached) {
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333, "c");
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "b");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "a");

  testing::TempFile temp_file("abc");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222},
           {kPrefix + ".upload_shard_2", 333}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto uploaders =
      CreateUploadShards(*client, temp_file.name(), kBucketName,
                         kDestObjectName, kPrefix, MinStreamSize(1));
  EXPECT_STATUS_OK(uploaders);

  ASSERT_EQ(3U, uploaders->size());

  auto res_future = (*uploaders)[0].WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  for (auto& shard : *uploaders) {
    EXPECT_STATUS_OK(shard.Upload());
  }
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);
  EXPECT_EQ(kDestObjectName, res->name());
  EXPECT_EQ(kBucketName, res->bucket());
}

TEST_F(ParallelUploadTest, FileSuccessWithMaxStreamsReached) {
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "c");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "ab");

  testing::TempFile temp_file("abc");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));

  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto uploaders = CreateUploadShards(*client, temp_file.name(), kBucketName,
                                      kDestObjectName, kPrefix,
                                      MinStreamSize(1), MaxStreams(2));
  EXPECT_STATUS_OK(uploaders);

  ASSERT_EQ(2U, uploaders->size());

  auto res_future = (*uploaders)[0].WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  for (auto& shard : *uploaders) {
    EXPECT_STATUS_OK(shard.Upload());
  }
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);
  EXPECT_EQ(kDestObjectName, res->name());
  EXPECT_EQ(kBucketName, res->bucket());
}

TEST_F(ParallelUploadTest, FileSuccessWithEmptyFile) {
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "");

  testing::TempFile temp_file("");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));

  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111}}, kDestObjectName,
          MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto uploaders = CreateUploadShards(*client, temp_file.name(), kBucketName,
                                      kDestObjectName, kPrefix,
                                      MinStreamSize(100), MaxStreams(200));
  EXPECT_STATUS_OK(uploaders);

  ASSERT_EQ(1U, uploaders->size());

  auto res_future = (*uploaders)[0].WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  for (auto& shard : *uploaders) {
    EXPECT_STATUS_OK(shard.Upload());
  }
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);
  EXPECT_EQ(kDestObjectName, res->name());
  EXPECT_EQ(kBucketName, res->bucket());
}

TEST_F(ParallelUploadTest, NonExistentFile) {
  // The expectations need to be reversed.
  auto uploaders =
      CreateUploadShards(*client, "nonexistent", kBucketName, kDestObjectName,
                         kPrefix, MinStreamSize(100), MaxStreams(200));
  EXPECT_FALSE(uploaders);
  EXPECT_EQ(StatusCode::kNotFound, uploaders.status().code());
}

TEST_F(ParallelUploadTest, UnreadableFile) {
#ifdef __linux__
  // The expectations need to be reversed.
  testing::TempFile temp_file("whatever");
  ASSERT_EQ(0, ::chmod(temp_file.name().c_str(), 0));
  if (std::ifstream(temp_file.name()).good()) {
    // On some versions of the standard library it succeeds. We're trying to
    // test the scenario when it fails, so ignore this test otherwise.
    return;
  }
  ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_0");
  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));
  auto uploaders = CreateUploadShards(*client, temp_file.name(), kBucketName,
                                      kDestObjectName, kPrefix,
                                      MinStreamSize(100), MaxStreams(200));
  EXPECT_STATUS_OK(uploaders);
  ASSERT_EQ(1U, uploaders->size());

  EXPECT_EQ(StatusCode::kNotFound, (*uploaders)[0].Upload().code());
  auto res = (*uploaders)[0].WaitForCompletion().get();
  ASSERT_FALSE(res);
  EXPECT_EQ(StatusCode::kNotFound, res.status().code());
#endif  // __linux__
}

TEST_F(ParallelUploadTest, FileOneStreamFailsUponCration) {
  // The expectations need to be reversed.
  ExpectCreateSessionFailure(kPrefix + ".upload_shard_1", PermanentError());
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  testing::TempFile temp_file("whatever");
  auto uploaders = CreateUploadShards(*client, temp_file.name(), kBucketName,
                                      kDestObjectName, kPrefix,
                                      MinStreamSize(1), MaxStreams(2));
  EXPECT_FALSE(uploaders);
  EXPECT_EQ(PermanentError().code(), uploaders.status().code());
}

TEST_F(ParallelUploadTest, FileBrokenStream) {
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333);
  ExpectCreateFailingSession(kPrefix + ".upload_shard_1", PermanentError());
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  testing::TempFile temp_file("abc");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto uploaders =
      CreateUploadShards(*client, temp_file.name(), kBucketName,
                         kDestObjectName, kPrefix, MinStreamSize(1));
  EXPECT_STATUS_OK(uploaders);

  EXPECT_STATUS_OK((*uploaders)[0].Upload());
  EXPECT_EQ(PermanentError().code(), (*uploaders)[1].Upload().code());
  EXPECT_STATUS_OK((*uploaders)[2].Upload());

  auto res = (*uploaders)[0].WaitForCompletion().get();
  EXPECT_FALSE(res);
  EXPECT_EQ(PermanentError().code(), res.status().code());
}

TEST_F(ParallelUploadTest, FileFailsToReadAfterCreation) {
#ifdef __linux__
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333);
  ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_1");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  testing::TempFile temp_file("abc");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto uploaders =
      CreateUploadShards(*client, temp_file.name(), kBucketName,
                         kDestObjectName, kPrefix, MinStreamSize(1));
  EXPECT_STATUS_OK(uploaders);

  EXPECT_STATUS_OK((*uploaders)[0].Upload());

  ASSERT_EQ(0, ::truncate(temp_file.name().c_str(), 0));
  EXPECT_EQ(StatusCode::kInternal, (*uploaders)[1].Upload().code());
  std::ofstream f(temp_file.name(), std::ios::binary | std::ios::trunc);
  f.write("abc", 3);
  f.close();
  ASSERT_TRUE(f.good());

  EXPECT_STATUS_OK((*uploaders)[2].Upload());

  auto res = (*uploaders)[0].WaitForCompletion().get();
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
#endif  // __linux__
}

TEST_F(ParallelUploadTest, ShardDestroyedTooEarly) {
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333);
  ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_1");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  testing::TempFile temp_file("abc");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto uploaders =
      CreateUploadShards(*client, temp_file.name(), kBucketName,
                         kDestObjectName, kPrefix, MinStreamSize(1));
  EXPECT_STATUS_OK(uploaders);

  EXPECT_STATUS_OK((*uploaders)[0].Upload());
  EXPECT_STATUS_OK((*uploaders)[2].Upload());
  { auto to_destroy = std::move((*uploaders)[1]); }

  auto res = (*uploaders)[0].WaitForCompletion().get();
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kCancelled, res.status().code());
}

TEST_F(ParallelUploadTest, FileSuccessBasic) {
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333, "c");
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "b");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "a");

  testing::TempFile temp_file("abc");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222},
           {kPrefix + ".upload_shard_2", 333}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(expect_deletion(kPrefix, kUploadMarkerGeneration)));

  auto res =
      ParallelUploadFile(*client, temp_file.name(), kBucketName,
                         kDestObjectName, kPrefix, false, MinStreamSize(1));
  EXPECT_STATUS_OK(res);
  EXPECT_EQ(kDestObjectName, res->name());
  EXPECT_EQ(kBucketName, res->bucket());
}

TEST_F(ParallelUploadTest, UploadNonExistentFile) {
  auto res =
      ParallelUploadFile(*client, "nonexistent", kBucketName, kDestObjectName,
                         kPrefix, false, MinStreamSize(1));
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kNotFound, res.status().code());
}

TEST_F(ParallelUploadTest, CleanupFailureIsIgnored) {
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));

  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111}}, kDestObjectName,
          MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions(
      {{{kPrefix + ".upload_shard_0", 111}, PermanentError()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }));

  testing::TempFile temp_file("abc");

  auto object_metadata = ParallelUploadFile(
      *client, temp_file.name(), kBucketName, kDestObjectName, kPrefix, true);
  ASSERT_STATUS_OK(object_metadata);
}

TEST_F(ParallelUploadTest, CleanupFailureIsNotIgnored) {
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix, kUploadMarkerGeneration)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));

  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111}}, kDestObjectName,
          MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions(
      {{{kPrefix + ".upload_shard_0", 111}, PermanentError()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }));

  testing::TempFile temp_file("abc");

  auto object_metadata = ParallelUploadFile(
      *client, temp_file.name(), kBucketName, kDestObjectName, kPrefix, false);
  ASSERT_FALSE(object_metadata);
}

TEST(ParallelUploadPersistentState, NotJson) {
  auto res = ParallelUploadPersistentState::FromString("blah");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("not a valid JSON"));
}

TEST(ParallelUploadPersistentState, RootNotOject) {
  auto res = ParallelUploadPersistentState::FromString("\"blah\"");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("not a JSON object"));
}

TEST(ParallelUploadPersistentState, NoDestination) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"a", "b"}}.dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("doesn't contain a 'destination'"));
}

TEST(ParallelUploadPersistentState, DestinationNotAString) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"destination", 2}}.dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("'destination' is not a string"));
}

TEST(ParallelUploadPersistentState, NoGeneration) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"destination", "b"}}.dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("doesn't contain a 'expected_generation'"));
}

TEST(ParallelUploadPersistentState, GenerationNotAString) {
  auto res = ParallelUploadPersistentState::FromString(internal::nl::json{
      {"destination", "dest"}, {"expected_generation", "blah"}}
                                                           .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("'expected_generation' is not a number"));
}

TEST(ParallelUploadPersistentState, CustomDataNotAString) {
  auto res = ParallelUploadPersistentState::FromString(internal::nl::json{
      {"destination", "dest"}, {"expected_generation", 1}, {"custom_data", 123}}
                                                           .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("'custom_data' is not a string"));
}

TEST(ParallelUploadPersistentState, NoStreams) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"destination", "dest"}, {"expected_generation", 1}}
          .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("doesn't contain 'streams'"));
}

TEST(ParallelUploadPersistentState, StreamsNotArray) {
  auto res = ParallelUploadPersistentState::FromString(internal::nl::json{
      {"destination", "dest"}, {"expected_generation", 1}, {"streams", 5}}
                                                           .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("is not an array"));
}

TEST(ParallelUploadPersistentState, StreamNotObject) {
  auto res = ParallelUploadPersistentState::FromString(internal::nl::json{
      {"destination", "dest"}, {"expected_generation", 1}, {"streams", {5}}}
                                                           .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("'stream' is not an object"));
}

TEST(ParallelUploadPersistentState, StreamHasNoName) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"destination", "dest"},
                         {"expected_generation", 1},
                         {"streams", {internal::nl::json::object()}}}
          .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("stream doesn't contain a 'name'"));
}

TEST(ParallelUploadPersistentState, StreamNameNotString) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"destination", "dest"},
                         {"expected_generation", 1},
                         {"streams", {{{"name", 1}}}}}
          .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("stream 'name' is not a string"));
}

TEST(ParallelUploadPersistentState, StreamHasNoSessionId) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"destination", "dest"},
                         {"expected_generation", 1},
                         {"streams", {{{"name", "abc"}}}}}
          .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("stream doesn't contain a 'resumable_session_id'"));
}

TEST(ParallelUploadPersistentState, StreamSessionIdNotString) {
  auto res = ParallelUploadPersistentState::FromString(internal::nl::json{
      {"destination", "dest"},
      {"expected_generation", 1},
      {"streams", {{{"name", "abc"}, {"resumable_session_id", 123}}}}}
                                                           .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(),
              HasSubstr("'resumable_session_id' is not a string"));
}

TEST(ParallelUploadPersistentState, StreamsEmpty) {
  auto res = ParallelUploadPersistentState::FromString(
      internal::nl::json{{"destination", "dest"},
                         {"expected_generation", 1},
                         {"streams", internal::nl::json::array()}}
          .dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("doesn't contain any streams"));
}

TEST(ParseResumableSessionId, InvalidPrefix) {
  auto res = ParseResumableSessionId("blahblah");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  res = ParseResumableSessionId("b");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
}

TEST(ParseResumableSessionId, NoSecondColon) {
  auto res = ParseResumableSessionId("ParUpl:");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  res = ParseResumableSessionId("ParUpl:blahblah");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
}

TEST(ParseResumableSessionId, GenerationNotANumber) {
  auto res = ParseResumableSessionId("ParUpl:");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  res = ParseResumableSessionId("ParUpl:some_name:some_gen");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
}

TEST(ParallelFileUploadSplitPointsToStringTest, Simple) {
  std::vector<std::uintmax_t> test_vec{1, 2};
  EXPECT_EQ("[1,2]", ParallelFileUploadSplitPointsToString(test_vec));
}

TEST(ParallelFileUploadSplitPointsFromStringTest, NotJson) {
  auto res = ParallelFileUploadSplitPointsFromString("blah");
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("not a valid JSON"));
}

TEST(ParallelFileUploadSplitPointsFromStringTest, NotArray) {
  auto res = ParallelFileUploadSplitPointsFromString(
      internal::nl::json{{"a", "b"}, {"b", "c"}}.dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("not an array"));
}

TEST(ParallelFileUploadSplitPointsFromStringTest, NotNumber) {
  auto res = ParallelFileUploadSplitPointsFromString(
      internal::nl::json{1, "a", 2}.dump());
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("not a number"));
}

TEST_F(ParallelUploadTest, ResumableSuccess) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333, "", "");
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "", "");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "", "");
  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_persistent_state(
          kPersistentStateName, kPersistentStateGeneration, expected_state)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(Status(StatusCode::kNotFound, "")));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222},
           {kPrefix + ".upload_shard_2", 333}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(
          expect_deletion(kPersistentStateName, kPersistentStateGeneration)));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_STATUS_OK(state);
  auto res_future = state->WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  auto cleanup_too_early = state->EagerCleanup();
  ASSERT_FALSE(cleanup_too_early.ok());
  EXPECT_EQ(StatusCode::kFailedPrecondition, cleanup_too_early.code());
  EXPECT_THAT(cleanup_too_early.message(), HasSubstr("still in progress"));

  state->shards().clear();
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);

  EXPECT_STATUS_OK(state->EagerCleanup());
  EXPECT_STATUS_OK(state->EagerCleanup());
}

TEST_F(ParallelUploadTest, Suspend) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_2", "");
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "", "");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "", "");
  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_persistent_state(
          kPersistentStateName, kPersistentStateGeneration, expected_state)));
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(Status(StatusCode::kNotFound, "")));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_STATUS_OK(state);
  EXPECT_EQ(kParallelResumableId, state->resumable_session_id());
  auto res_future = state->WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  auto cleanup_too_early = state->EagerCleanup();
  ASSERT_FALSE(cleanup_too_early.ok());
  EXPECT_EQ(StatusCode::kFailedPrecondition, cleanup_too_early.code());
  EXPECT_THAT(cleanup_too_early.message(), HasSubstr("still in progress"));

  std::move(state->shards()[2]).Suspend();
  state->shards().clear();
  auto res = res_future.get();
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kCancelled, res.status().code());
}

TEST_F(ParallelUploadTest, Resume) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333, "",
                      kIndividualSessionId);
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "",
                      kIndividualSessionId);
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "",
                      kIndividualSessionId);
  internal::nl::json state_json{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222},
           {kPrefix + ".upload_shard_2", 333}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));
  EXPECT_CALL(*raw_client_mock, ReadObject(_))
      .WillOnce(Invoke(create_state_read_expectation(
          kPersistentStateName, kPersistentStateGeneration, state_json)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(
          expect_deletion(kPersistentStateName, kPersistentStateGeneration)));

  auto state = PrepareParallelUpload(
      *client, kBucketName, kDestObjectName, num_shards, kPrefix,
      UseResumableUploadSession(kParallelResumableId));
  EXPECT_STATUS_OK(state);
  auto res_future = state->WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  auto cleanup_too_early = state->EagerCleanup();
  ASSERT_FALSE(cleanup_too_early.ok());
  EXPECT_EQ(StatusCode::kFailedPrecondition, cleanup_too_early.code());
  EXPECT_THAT(cleanup_too_early.message(), HasSubstr("still in progress"));

  state->shards().clear();
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);

  EXPECT_STATUS_OK(state->EagerCleanup());
  EXPECT_STATUS_OK(state->EagerCleanup());
}

TEST_F(ParallelUploadTest, ResumableOneStreamFailsUponCration) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSessionFailure(kPrefix + ".upload_shard_1", PermanentError());
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(Status(StatusCode::kNotFound, "")));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_FALSE(state);
  EXPECT_EQ(PermanentError().code(), state.status().code());
}

TEST_F(ParallelUploadTest, BrokenResumableStream) {
  int const num_shards = 3;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333);
  ExpectCreateFailingSession(kPrefix + ".upload_shard_1", PermanentError());
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111);

  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_persistent_state(
          kPersistentStateName, kPersistentStateGeneration, expected_state)));
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(Status(StatusCode::kNotFound, "")));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_STATUS_OK(state);

  state->shards().clear();
  auto res = state->WaitForCompletion().get();
  EXPECT_FALSE(res);
  EXPECT_EQ(PermanentError().code(), res.status().code());
}

TEST_F(ParallelUploadTest, ResumableSuccessDestinationExists) {
  int const num_shards = 1;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "", "");
  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 42},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_persistent_state(
          kPersistentStateName, kPersistentStateGeneration, expected_state)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(MockObject(kDestObjectName, 42)));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111}}, kDestObjectName,
          MockObject(kDestObjectName, kDestGeneration), 42)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(
          expect_deletion(kPersistentStateName, kPersistentStateGeneration)));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_STATUS_OK(state);
  auto res_future = state->WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  auto cleanup_too_early = state->EagerCleanup();
  ASSERT_FALSE(cleanup_too_early.ok());
  EXPECT_EQ(StatusCode::kFailedPrecondition, cleanup_too_early.code());
  EXPECT_THAT(cleanup_too_early.message(), HasSubstr("still in progress"));

  state->shards().clear();
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);

  EXPECT_STATUS_OK(state->EagerCleanup());
  EXPECT_STATUS_OK(state->EagerCleanup());
}

TEST_F(ParallelUploadTest, ResumableSuccessDestinationChangedUnderhandedly) {
  int const num_shards = 1;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "", "");
  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 42},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_persistent_state(
          kPersistentStateName, kPersistentStateGeneration, expected_state)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(MockObject(kDestObjectName, 42)))
      .WillOnce(Return(MockObject(kDestObjectName, kDestGeneration)));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Return(Status(StatusCode::kFailedPrecondition, "")));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(
          expect_deletion(kPersistentStateName, kPersistentStateGeneration)));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_STATUS_OK(state);
  auto res_future = state->WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  auto cleanup_too_early = state->EagerCleanup();
  ASSERT_FALSE(cleanup_too_early.ok());
  EXPECT_EQ(StatusCode::kFailedPrecondition, cleanup_too_early.code());
  EXPECT_THAT(cleanup_too_early.message(), HasSubstr("still in progress"));

  state->shards().clear();
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);

  EXPECT_STATUS_OK(state->EagerCleanup());
  EXPECT_STATUS_OK(state->EagerCleanup());
}

TEST_F(ParallelUploadTest, ResumableInitialGetMetadataFails) {
  int const num_shards = 1;
  // The expectations need to be reversed.
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(Status(StatusCode::kPermissionDenied, "")));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_FALSE(state);
  EXPECT_EQ(StatusCode::kPermissionDenied, state.status().code());
}

TEST_F(ParallelUploadTest, StoringPersistentStateFails) {
  int const num_shards = 1;
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "", "");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Return(Status(StatusCode::kPermissionDenied, "")));

  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(MockObject(kDestObjectName, 42)));

  auto state =
      PrepareParallelUpload(*client, kBucketName, kDestObjectName, num_shards,
                            kPrefix, UseResumableUploadSession(""));
  EXPECT_FALSE(state);
  EXPECT_EQ(StatusCode::kPermissionDenied, state.status().code());
}

TEST_F(ParallelUploadTest, ResumeFailsOnBadState) {
  int const num_shards = 1;
  internal::nl::json state_json{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, ReadObject(_))
      .WillOnce(Invoke(create_state_read_expectation(
          kPersistentStateName, kPersistentStateGeneration,
          internal::nl::json{{"not", "valid"}})));

  auto state = PrepareParallelUpload(
      *client, kBucketName, kDestObjectName, num_shards, kPrefix,
      UseResumableUploadSession(kParallelResumableId));
  EXPECT_FALSE(state);
  EXPECT_EQ(StatusCode::kInternal, state.status().code());
}

TEST_F(ParallelUploadTest, ResumableOneStreamFailsUponCrationOnResume) {
  int const num_shards = 1;
  // The expectations need to be reversed.
  ExpectCreateSessionFailure(kPrefix + ".upload_shard_0", PermanentError());

  internal::nl::json state_json{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}}}}};
  EXPECT_CALL(*raw_client_mock, ReadObject(_))
      .WillOnce(Invoke(create_state_read_expectation(
          kPersistentStateName, kPersistentStateGeneration, state_json)));

  auto state = PrepareParallelUpload(
      *client, kBucketName, kDestObjectName, num_shards, kPrefix,
      UseResumableUploadSession(kParallelResumableId));
  EXPECT_FALSE(state);
  EXPECT_EQ(PermanentError().code(), state.status().code());
}

TEST_F(ParallelUploadTest, ResumableBadSessionId) {
  int const num_shards = 1;

  auto state = PrepareParallelUpload(
      *client, kBucketName, kDestObjectName, num_shards, kPrefix,
      UseResumableUploadSession("bad session id"));
  EXPECT_FALSE(state);
  EXPECT_EQ(StatusCode::kInternal, state.status().code());
}

TEST_F(ParallelUploadTest, ResumeBadNumShards) {
  int const num_shards = 2;
  // The expectations need to be reversed.
  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 42},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, ReadObject(_))
      .WillOnce(Invoke(create_state_read_expectation(
          kPersistentStateName, kPersistentStateGeneration, expected_state)));

  auto state = PrepareParallelUpload(
      *client, kBucketName, kDestObjectName, num_shards, kPrefix,
      UseResumableUploadSession(kParallelResumableId));
  EXPECT_FALSE(state);
  EXPECT_EQ(StatusCode::kInternal, state.status().code());
  EXPECT_THAT(state.status().message(),
              HasSubstr("previously specified number of shards"));
}

TEST_F(ParallelUploadTest, ResumeDifferentDest) {
  int const num_shards = 1;
  // The expectations need to be reversed.
  internal::nl::json expected_state{
      {"destination", "some-different-object"},
      {"expected_generation", 42},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, ReadObject(_))
      .WillOnce(Invoke(create_state_read_expectation(
          kPersistentStateName, kPersistentStateGeneration, expected_state)));

  auto state = PrepareParallelUpload(
      *client, kBucketName, kDestObjectName, num_shards, kPrefix,
      UseResumableUploadSession(kParallelResumableId));
  EXPECT_FALSE(state);
  EXPECT_EQ(StatusCode::kInternal, state.status().code());
  EXPECT_THAT(state.status().message(),
              HasSubstr("resumable session ID is doesn't match"));
}

TEST_F(ParallelUploadTest, ResumableUploadFileShards) {
  // The expectations need to be reversed.
  ExpectCreateSession(kPrefix + ".upload_shard_2", 333, "c", "");
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "b", "");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "a", "");
  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"custom_data", internal::nl::json{1, 2}.dump()},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  testing::TempFile temp_file("abc");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_persistent_state(
          kPersistentStateName, kPersistentStateGeneration, expected_state)))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(Status(StatusCode::kNotFound, "")));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222},
           {kPrefix + ".upload_shard_2", 333}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(
          expect_deletion(kPersistentStateName, kPersistentStateGeneration)));

  auto uploaders = CreateUploadShards(
      *client, temp_file.name(), kBucketName, kDestObjectName, kPrefix,
      MinStreamSize(1), UseResumableUploadSession(""));
  EXPECT_STATUS_OK(uploaders);

  ASSERT_EQ(3U, uploaders->size());

  auto res_future = (*uploaders)[0].WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  for (auto& shard : *uploaders) {
    EXPECT_STATUS_OK(shard.Upload());
  }
  auto res = res_future.get();
  EXPECT_STATUS_OK(res);
  EXPECT_EQ(kDestObjectName, res->name());
  EXPECT_EQ(kBucketName, res->bucket());
}

TEST_F(ParallelUploadTest, SuspendUploadFileShards) {
  // The expectations need to be reversed.
  ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_2", "");
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "def", "");
  ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "abc", "");
  internal::nl::json expected_state{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"custom_data", internal::nl::json{3, 6}.dump()},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  testing::TempFile temp_file("abcdefghi");

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_persistent_state(
          kPersistentStateName, kPersistentStateGeneration, expected_state)));
  EXPECT_CALL(*raw_client_mock, GetObjectMetadata(_))
      .WillOnce(Return(Status(StatusCode::kNotFound, "")));

  auto uploaders = CreateUploadShards(
      *client, temp_file.name(), kBucketName, kDestObjectName, kPrefix,
      MinStreamSize(3), UseResumableUploadSession(""));
  EXPECT_STATUS_OK(uploaders);

  ASSERT_EQ(3U, uploaders->size());

  auto res_future = (*uploaders)[0].WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  // don't upload the last shard
  (*uploaders)[0].Upload();
  (*uploaders)[1].Upload();
  uploaders->clear();
  auto res = res_future.get();
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kCancelled, res.status().code());
}

TEST_F(ParallelUploadTest, SuspendUploadFileResume) {
  // The expectations need to be reversed.
  auto& session3 = ExpectCreateSession(kPrefix + ".upload_shard_2", 333, "hi",
                                       kIndividualSessionId);
  ExpectCreateSession(kPrefix + ".upload_shard_1", 222, "def",
                      kIndividualSessionId);
  auto& session1 = ExpectCreateSession(kPrefix + ".upload_shard_0", 111, "");
  // last stream has one byte uploaded
  EXPECT_CALL(session3, next_expected_byte()).WillRepeatedly(Return(1));
  // first stream is fully uploaded
  EXPECT_CALL(session1, next_expected_byte()).WillRepeatedly(Return(3));
  internal::nl::json state_json{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"custom_data", internal::nl::json{3, 6}.dump()},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, InsertObjectMedia(_))
      .WillOnce(Invoke(expect_new_object(kPrefix + ".compose_many",
                                         kComposeMarkerGeneration)));
  EXPECT_CALL(*raw_client_mock, ComposeObject(_))
      .WillOnce(Invoke(create_composition_check(
          {{kPrefix + ".upload_shard_0", 111},
           {kPrefix + ".upload_shard_1", 222},
           {kPrefix + ".upload_shard_2", 333}},
          kDestObjectName, MockObject(kDestObjectName, kDestGeneration))));
  EXPECT_CALL(*raw_client_mock, ReadObject(_))
      .WillOnce(Invoke(create_state_read_expectation(
          kPersistentStateName, kPersistentStateGeneration, state_json)));

  ExpectedDeletions deletions({{{kPrefix + ".upload_shard_0", 111}, Status()},
                               {{kPrefix + ".upload_shard_1", 222}, Status()},
                               {{kPrefix + ".upload_shard_2", 333}, Status()}});
  EXPECT_CALL(*raw_client_mock, DeleteObject(_))
      .WillOnce(Invoke(
          expect_deletion(kPrefix + ".compose_many", kComposeMarkerGeneration)))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke([&deletions](internal::DeleteObjectRequest const& r) {
        return deletions(r);
      }))
      .WillOnce(Invoke(
          expect_deletion(kPersistentStateName, kPersistentStateGeneration)));

  testing::TempFile temp_file("abcdefghi");

  auto uploaders = CreateUploadShards(
      *client, temp_file.name(), kBucketName, kDestObjectName, kPrefix,
      MinStreamSize(3), UseResumableUploadSession(kParallelResumableId));

  EXPECT_STATUS_OK(uploaders);
  ASSERT_EQ(3U, uploaders->size());

  auto res_future = (*uploaders)[0].WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  for (auto& uploader : *uploaders) {
    uploader.Upload();
  }

  auto res = res_future.get();
  EXPECT_STATUS_OK(res);
}

TEST_F(ParallelUploadTest, SuspendUploadFileResumeBadOffset) {
  // The expectations need to be reversed.
  ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_2",
                               kIndividualSessionId);
  ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_1",
                               kIndividualSessionId);
  auto& session1 = ExpectCreateSessionToSuspend(kPrefix + ".upload_shard_0",
                                                kIndividualSessionId);
  EXPECT_CALL(session1, next_expected_byte()).WillRepeatedly(Return(7));
  internal::nl::json state_json{
      {"destination", "final-object"},
      {"expected_generation", 0},
      {"custom_data", internal::nl::json{3, 6}.dump()},
      {"streams",
       {{{"name", "some-prefix.upload_shard_0"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_1"},
         {"resumable_session_id", kIndividualSessionId}},
        {{"name", "some-prefix.upload_shard_2"},
         {"resumable_session_id", kIndividualSessionId}}}}};

  EXPECT_CALL(*raw_client_mock, ReadObject(_))
      .WillOnce(Invoke(create_state_read_expectation(
          kPersistentStateName, kPersistentStateGeneration, state_json)));

  testing::TempFile temp_file("abcdefghi");

  auto uploaders = CreateUploadShards(
      *client, temp_file.name(), kBucketName, kDestObjectName, kPrefix,
      MinStreamSize(3), UseResumableUploadSession(kParallelResumableId));

  EXPECT_STATUS_OK(uploaders);
  ASSERT_EQ(3U, uploaders->size());

  auto res_future = (*uploaders)[0].WaitForCompletion();
  EXPECT_TRUE(Unsatisfied(res_future));

  (*uploaders)[0].Upload();
  uploaders->clear();

  auto res = res_future.get();
  EXPECT_FALSE(res);
  EXPECT_EQ(StatusCode::kInternal, res.status().code());
  EXPECT_THAT(res.status().message(), HasSubstr("Corrupted upload state"));
}

}  // namespace
}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
