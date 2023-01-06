/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/blocking_validator_runner.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "flatbuffers/flatbuffer_builder.h"  // from @flatbuffers
#include "tensorflow/lite/experimental/acceleration/configuration/configuration_generated.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/status_codes.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/validator.h"
#include "tensorflow/lite/logger.h"
#include "tensorflow/lite/minimal_logging.h"

namespace tflite {
namespace acceleration {

using ::flatbuffers::FlatBufferBuilder;
using ::flatbuffers::GetRoot;

// Wait time between each query to the test result file, defined in
// microseconds.
constexpr absl::Duration kWaitBetweenRefresh = absl::Milliseconds(20);

BlockingValidatorRunner::BlockingValidatorRunner(
    const ValidatorRunnerOptions& options)
    : per_test_timeout_ms_(options.per_test_timeout_ms),
      storage_path_(options.storage_path) {
  std::string model_path;
  if (!options.model_path.empty()) {
    model_path = options.model_path;
  } else if (options.model_fd >= 0) {
    model_path = absl::StrCat("fd:", options.model_fd, ":",
                              options.model_offset, ":", options.model_size);
  }

  validator_runner_impl_ = std::make_unique<ValidatorRunnerImpl>(
      model_path, options.storage_path, options.data_directory_path,
      options.per_test_timeout_ms,
      options.custom_input_data.empty()
          ? nullptr
          : std::make_unique<CustomValidationEmbedder>(
                options.custom_input_batch_size, options.custom_input_data,
                options.error_reporter),
      options.error_reporter, options.nnapi_sl,
      options.validation_entrypoint_name, options.benchmark_result_evaluator);
}

MinibenchmarkStatus BlockingValidatorRunner::Init() {
  return validator_runner_impl_->Init();
}

std::vector<FlatBufferBuilder> BlockingValidatorRunner::TriggerValidation(
    const std::vector<const TFLiteSettings*>& for_settings) {
  if (for_settings.empty()) {
    return {};
  }

  // Delete storage_file before running the tests, so that each run is
  // independent from each other.
  (void)unlink(storage_path_.c_str());
  auto to_be_run =
      std::make_unique<std::vector<flatbuffers::FlatBufferBuilder>>();
  std::vector<TFLiteSettingsT> for_settings_obj;
  for_settings_obj.reserve(for_settings.size());
  for (auto settings : for_settings) {
    TFLiteSettingsT tflite_settings;
    settings->UnPackTo(&tflite_settings);
    flatbuffers::FlatBufferBuilder copy;
    copy.Finish(CreateTFLiteSettings(copy, &tflite_settings));
    to_be_run->emplace_back(std::move(copy));
    for_settings_obj.emplace_back(tflite_settings);
  }
  validator_runner_impl_->TriggerValidationAsync(std::move(to_be_run));

  // The underlying process runner should ensure each test finishes on time or
  // timed out. deadline_us is added here as an extra safety guard.
  int64_t total_timeout_ms = per_test_timeout_ms_ * (1 + for_settings.size());
  int64_t deadline_us = Validator::BootTimeMicros() + total_timeout_ms * 1000;

  bool within_timeout = true;
  // TODO(b/249274787): GetNumCompletedResults() loads the file from disk each
  // time when called. We should find a way to optimize the FlatbufferStorage to
  // reduce the I/O and remove the sleep().
  while ((validator_runner_impl_->GetNumCompletedResults()) <
             for_settings.size() &&
         (within_timeout = Validator::BootTimeMicros() < deadline_us)) {
    usleep(absl::ToInt64Microseconds(kWaitBetweenRefresh));
  }

  std::vector<FlatBufferBuilder> results =
      validator_runner_impl_->GetCompletedResults();
  if (!within_timeout) {
    TFLITE_LOG_PROD(
        TFLITE_LOG_WARNING,
        "Validation timed out after %ld ms. Return before all tests finished.",
        total_timeout_ms);
  } else if (for_settings.size() != results.size()) {
    TFLITE_LOG_PROD(TFLITE_LOG_WARNING,
                    "Validation completed.Started benchmarking for %d "
                    "TFLiteSettings, received %d results.",
                    for_settings.size(), results.size());
  }

  // If there are any for_settings missing from results, add an error event.
  std::vector<TFLiteSettingsT> result_settings;
  result_settings.reserve(results.size());
  for (auto& result : results) {
    const BenchmarkEvent* event =
        GetRoot<BenchmarkEvent>(result.GetBufferPointer());
    TFLiteSettingsT event_settings;
    event->tflite_settings()->UnPackTo(&event_settings);
    result_settings.emplace_back(std::move(event_settings));
  }
  for (auto& settings_obj : for_settings_obj) {
    auto result_it =
        std::find(result_settings.begin(), result_settings.end(), settings_obj);
    if (result_it == result_settings.end()) {
      FlatBufferBuilder fbb;
      fbb.Finish(CreateBenchmarkEvent(
          fbb, CreateTFLiteSettings(fbb, &settings_obj),
          BenchmarkEventType_ERROR, /* result */ 0,
          CreateBenchmarkError(fbb, BenchmarkStage_UNKNOWN,
                               kMinibenchmarkCompletionEventMissing),
          Validator::BootTimeMicros(), Validator::WallTimeMicros()));
      results.emplace_back(std::move(fbb));
    }
  }
  return results;
}

}  // namespace acceleration
}  // namespace tflite
