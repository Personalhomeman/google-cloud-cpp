#!/usr/bin/env bash
#
# Copyright 2018 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -eu

echo "================================================================"
echo "Running Bazel build with integration tests against production $(date)."
echo "================================================================"

echo "Running build and tests"
cd "$(dirname "$0")/../../.."
readonly PROJECT_ROOT="${PWD}"

echo "================================================================"
echo "Update or Install Bazel $(date)."
echo "================================================================"
"${PROJECT_ROOT}/ci/install-bazel.sh"

echo "================================================================"
echo "Compiling and running unit tests $(date)"
echo "================================================================"
readonly BAZEL_BIN="$HOME/bin/bazel"
echo "Using Bazel in ${BAZEL_BIN}"
"${BAZEL_BIN}" version

# Kokoro does guarantee that g++-4.9 will be installed, but the default compiler
# might be g++-4.8. Set the compiler version explicitly:
export CC=/usr/bin/gcc-4.9
export CXX=/usr/bin/g++-4.9

echo "================================================================"
echo "Compiling and running unit tests $(date)"
echo "================================================================"
"${BAZEL_BIN}" test \
    --test_output=errors \
    --verbose_failures=true \
    --keep_going \
    -- //google/cloud/...:all

echo "================================================================"
echo "Compiling all the code, including integration tests $(date)"
echo "================================================================"
# Then build everything else (integration tests, examples, etc). So we can run
# them next.
"${BAZEL_BIN}" build \
    --test_output=errors \
    --verbose_failures=true \
    --keep_going \
    -- //google/cloud/...:all

# The integration tests need further configuration and tools.
echo "================================================================"
echo "Download dependencies for integration tests $(date)."
echo "================================================================"

# Download the gRPC `roots.pem` file. Somewhere inside the bowels of Bazel, this
# file might exist, but my attempts at using it have failed.
echo "    Getting roots.pem for gRPC."
wget -q https://raw.githubusercontent.com/grpc/grpc/master/etc/roots.pem
export GRPC_DEFAULT_SSL_ROOTS_FILE_PATH="$PWD/roots.pem"
# If this file does not exist gRPC blocks trying to connect, so it is better
# to break the build early (the ls command breaks and the build stops) if that
# is the case.
echo "GRPC_DEFAULT_SSL_ROOTS_FILE_PATH = ${GRPC_DEFAULT_SSL_ROOTS_FILE_PATH}"
ls -l "$(dirname "${GRPC_DEFAULT_SSL_ROOTS_FILE_PATH}")"
ls -l "${GRPC_DEFAULT_SSL_ROOTS_FILE_PATH}"

echo "    Getting cbt tool"
wget -q https://dl.google.com/dl/cloudsdk/channels/rapid/downloads/google-cloud-sdk-282.0.0-linux-x86_64.tar.gz
sha256sum google-cloud-sdk-282.0.0-linux-x86_64.tar.gz | \
    grep -q '^db2fd176a998381ef937bd9f9e83b16eadff864111255d771976d654c961fc95 '
tar x -C "${HOME}" -f google-cloud-sdk-282.0.0-linux-x86_64.tar.gz
"${HOME}/google-cloud-sdk/bin/gcloud" --quiet components install cbt
export CBT="${HOME}/google-cloud-sdk/bin/cbt"

echo "================================================================"
echo "Setup environment for integration tests $(date)"
export TEST_KEY_FILE_JSON="${KOKORO_GFILE_DIR}/service-account.json"
export TEST_KEY_FILE_P12="${KOKORO_GFILE_DIR}/service-account.p12"
export GOOGLE_APPLICATION_CREDENTIALS="${KOKORO_GFILE_DIR}/service-account.json"

# Activate the account so we can create a token using `gcloud`, note that this account
# is also used further down.
gcloud auth activate-service-account --key-file \
    "${KOKORO_GFILE_DIR}/service-account.json"

ACCESS_TOKEN="$(gcloud auth application-default print-access-token)"
export ACCESS_TOKEN

echo "Reading CI secret configuration parameters."
source "${KOKORO_GFILE_DIR}/test-configuration.sh"

BAZEL_BIN_DIR="$("${BAZEL_BIN}" info bazel-bin)"
readonly BAZEL_BIN_DIR

if [[ "${ENABLE_BIGTABLE_ADMIN_INTEGRATION_TESTS:-}" = "yes" ]]; then
  echo
  echo "================================================================"
  echo "Running Google Cloud Bigtable Integration Tests $(date)"
  echo "================================================================"
  (cd "${BAZEL_BIN_DIR}/google/cloud/bigtable/tests" && \
     "${PROJECT_ROOT}/google/cloud/bigtable/tests/run_admin_integration_tests_production.sh")
  (cd "${BAZEL_BIN_DIR}/google/cloud/bigtable/examples" && \
     "${PROJECT_ROOT}/google/cloud/bigtable/examples/run_admin_examples_production.sh")
fi

echo
echo "================================================================"
echo "Running Google Cloud Bigtable Integration Tests $(date)"
echo "================================================================"
(cd "${BAZEL_BIN_DIR}/google/cloud/bigtable/tests" && \
   "${PROJECT_ROOT}/google/cloud/bigtable/tests/run_integration_tests_production.sh")
(cd "${BAZEL_BIN_DIR}/google/cloud/bigtable/examples" && \
   "${PROJECT_ROOT}/google/cloud/bigtable/examples/run_examples_production.sh")
(cd "${BAZEL_BIN_DIR}/google/cloud/bigtable/examples" && \
   "${PROJECT_ROOT}/google/cloud/bigtable/examples/run_grpc_credential_examples_production.sh")

echo
echo "================================================================"
echo "Running Google Cloud Storage Integration Tests $(date)"
echo "================================================================"
# Recall that each evaluation of ${RANDOM} produces a different value.
HMAC_SERVICE_ACCOUNT_NAME="hmac-sa-$(date +%s)-${RANDOM}"
HMAC_SERVICE_ACCOUNT="${HMAC_SERVICE_ACCOUNT_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"
export HMAC_SERVICE_ACCOUNT

gcloud iam service-accounts create "--project=${PROJECT_ID}" \
    "${HMAC_SERVICE_ACCOUNT_NAME}"
gcloud projects add-iam-policy-binding "${PROJECT_ID}" \
    --member "serviceAccount:${HMAC_SERVICE_ACCOUNT}" \
    --role roles/iam.serviceAccountTokenCreator

echo "Create service account to run the tests."
set +e
(cd "${BAZEL_BIN_DIR}/google/cloud/storage/tests" && \
    "${PROJECT_ROOT}/google/cloud/storage/tests/run_integration_tests_production.sh")
storage_integration_test_status=$?
echo "Running Google Cloud Storage Examples"
(cd "${BAZEL_BIN_DIR}/google/cloud/storage/examples" && \
    "${PROJECT_ROOT}/google/cloud/storage/examples/run_examples_production.sh")
storage_examples_status=$?
set -e

gcloud projects remove-iam-policy-binding "${PROJECT_ID}" \
    --member "serviceAccount:${HMAC_SERVICE_ACCOUNT}" \
    --role roles/iam.serviceAccountTokenCreator
gcloud iam service-accounts delete --quiet "${HMAC_SERVICE_ACCOUNT}"

if [[ "${storage_integration_test_status}" != 0 ]]; then
  echo "Error in integration tests."
  exit 1
fi

if [[ "${storage_examples_status}" != 0 ]]; then
  echo "Error in storage examples."
  exit 1
fi

echo "================================================================"
echo "Build completed $(date)"
echo "================================================================"
