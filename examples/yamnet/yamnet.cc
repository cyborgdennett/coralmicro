// Copyright 2022 Google LLC
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

#include "libs/tensorflow/yamnet.h"

#include "libs/audio/audio_service.h"
#include "libs/base/filesystem.h"
#include "libs/base/timer.h"
#include "libs/tensorflow/utils.h"
#include "libs/tpu/edgetpu_manager.h"
#include "libs/tpu/edgetpu_op.h"
#include "third_party/tflite-micro/tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_interpreter.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_mutable_op_resolver.h"

namespace {
constexpr int kTensorArenaSize = 1 * 1024 * 1024;
STATIC_TENSOR_ARENA_IN_SDRAM(tensor_arena, kTensorArenaSize);
constexpr int kNumDmaBuffers = 2;
constexpr int kDmaBufferSizeMs = 50;
constexpr int kDmaBufferSize = kNumDmaBuffers *
                               coralmicro::tensorflow::kYamnetSampleRateMs *
                               kDmaBufferSizeMs;
constexpr int kAudioServicePriority = 4;
constexpr int kDropFirstSamplesMs = 150;

coralmicro::AudioDriverBuffers<kNumDmaBuffers, kDmaBufferSize> audio_buffers;
coralmicro::AudioDriver audio_driver(audio_buffers);

constexpr int kAudioBufferSizeMs = coralmicro::tensorflow::kYamnetDurationMs;
constexpr int kAudioBufferSize =
    kAudioBufferSizeMs * coralmicro::tensorflow::kYamnetSampleRateMs;

constexpr float kThreshold = 0.3;
constexpr int kTopK = 5;

#ifdef YAMNET_CPU
constexpr char kModelName[] = "/models/yamnet.tflite";
constexpr bool kUseTpu = false;
#else
constexpr char kModelName[] = "/models/yamnet_edgetpu.tflite";
constexpr bool kUseTpu = true;
#endif

// Run invoke and get the results after the interpreter have already been
// populated with raw audio input.
void run(tflite::MicroInterpreter* interpreter, FrontendState* frontend_state) {
  auto input_tensor = interpreter->input_tensor(0);
  auto preprocess_start = coralmicro::TimerMillis();
  coralmicro::tensorflow::YamNetPreprocessInput(input_tensor, frontend_state);
  // Reset frontend state.
  FrontendReset(frontend_state);
  auto preprocess_end = coralmicro::TimerMillis();
  if (interpreter->Invoke() != kTfLiteOk) {
    printf("Failed to invoke on test input\r\n");
    vTaskSuspend(nullptr);
  }
  auto current_time = coralmicro::TimerMillis();
  printf(
      "Yamnet preprocess time: %lums, invoke time: %lums, total: "
      "%lums\r\n",
      static_cast<uint32_t>(preprocess_end - preprocess_start),
      static_cast<uint32_t>(current_time - preprocess_end),
      static_cast<uint32_t>(current_time - preprocess_start));
  auto results = coralmicro::tensorflow::GetClassificationResults(
      interpreter, kThreshold, kTopK);
  printf("%s\r\n",
         coralmicro::tensorflow::FormatClassificationOutput(results).c_str());
}

}  // namespace

extern "C" [[noreturn]] void app_main(void* param) {
  printf("YAMNet!!!\r\n");
  std::vector<uint8_t> yamnet_tflite;
  if (!coralmicro::LfsReadFile(kModelName, &yamnet_tflite)) {
    printf("Failed to load model\r\n");
    vTaskSuspend(nullptr);
  }

  const auto* model = tflite::GetModel(yamnet_tflite.data());
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    printf("Model schema version is %lu, supported is %d", model->version(),
           TFLITE_SCHEMA_VERSION);
    vTaskSuspend(nullptr);
  }

#ifndef YAMNET_CPU
  auto edgetpu_context =
      coralmicro::EdgeTpuManager::GetSingleton()->OpenDevice();
  if (!edgetpu_context) {
    printf("Failed to get TPU context\r\n");
    vTaskSuspend(nullptr);
  }
#endif

  tflite::MicroErrorReporter error_reporter;
  auto yamnet_resolver =
      coralmicro::tensorflow::SetupYamNetResolver</*tForTpu=*/kUseTpu>();

  tflite::MicroInterpreter interpreter{model, yamnet_resolver, tensor_arena,
                                       kTensorArenaSize, &error_reporter};
  if (interpreter.AllocateTensors() != kTfLiteOk) {
    printf("AllocateTensors failed.\r\n");
    vTaskSuspend(nullptr);
  }

  FrontendState frontend_state{};
  if (!coralmicro::tensorflow::YamNetPrepareFrontEnd(&frontend_state)) {
    printf("coralmicro::tensorflow::YamNetPrepareFrontEnd() failed.\r\n");
    vTaskSuspend(nullptr);
  }

  // Run tensorflow on test input file.
  std::vector<uint8_t> yamnet_test_input_bin;
  if (!coralmicro::LfsReadFile("/models/yamnet_test_audio.bin",
                               &yamnet_test_input_bin)) {
    printf("Failed to load test input!\r\n");
    vTaskSuspend(nullptr);
  }
  if (yamnet_test_input_bin.size() !=
      coralmicro::tensorflow::kYamnetAudioSize * sizeof(int16_t)) {
    printf("Input audio size doesn't match expected\r\n");
    vTaskSuspend(nullptr);
  }
  auto input_tensor = interpreter.input_tensor(0);
  std::memcpy(tflite::GetTensorData<uint8_t>(input_tensor),
              yamnet_test_input_bin.data(), yamnet_test_input_bin.size());
  run(&interpreter, &frontend_state);

  // Setup audio
  coralmicro::AudioDriverConfig audio_config{
      coralmicro::AudioSampleRate::k16000_Hz, kNumDmaBuffers, kDmaBufferSizeMs};
  coralmicro::AudioService audio_service(
      &audio_driver, audio_config, kAudioServicePriority, kDropFirstSamplesMs);
  coralmicro::LatestSamples audio_latest(
      coralmicro::MsToSamples(coralmicro::AudioSampleRate::k16000_Hz,
                              coralmicro::tensorflow::kYamnetDurationMs));
  audio_service.AddCallback(
      &audio_latest,
      +[](void* ctx, const int32_t* samples, size_t num_samples) {
        static_cast<coralmicro::LatestSamples*>(ctx)->Append(samples,
                                                             num_samples);
        return true;
      });

  // Delay for the first buffers to fill.
  vTaskDelay(pdMS_TO_TICKS(coralmicro::tensorflow::kYamnetDurationMs));
  auto audio_input = tflite::GetTensorData<int16_t>(input_tensor);
  while (true) {
    audio_latest.AccessLatestSamples(
        [&audio_input](const std::vector<int32_t>& samples,
                       size_t start_index) {
          size_t i, j = 0;
          // Starting with start_index, grab until the end of the buffer.
          for (i = 0; i < samples.size() - start_index; ++i) {
            audio_input[i] = samples[i + start_index] >> 16;
          }
          // Now fill the rest of the data with the beginning of the
          // buffer.
          for (j = 0; j < samples.size() - i; ++j) {
            audio_input[i + j] = samples[j] >> 16;
          }
        });
    run(&interpreter, &frontend_state);
#ifndef YAMNET_CPU
    // Delay 975 ms to rate limit the TPU version.
    vTaskDelay(pdMS_TO_TICKS(coralmicro::tensorflow::kYamnetDurationMs));
#endif
  }
}
