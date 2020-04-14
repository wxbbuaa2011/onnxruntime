// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "testPch.h"

#include <functional>

#include "lib/Api.Ort/pch.h"

#include "AdapterSessionTest.h"
#include "ILotusValueProviderPrivate.h"
#include "onnxruntime_c_api.h"
#include "OnnxruntimeEngine.h"
#include "OnnxruntimeErrors.h"
#include "OnnxruntimeModel.h"
#include "core/common/logging/isink.h"
#include "core/common/logging/logging.h"
#include "core/session/abi_session_options_impl.h"

using namespace winrt;
using namespace winrt::Windows::AI::MachineLearning;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace {
WinML::OnnxruntimeEngineFactory *engine_factory;
const OrtApi *ort_api;
const WinmlAdapterApi *winml_adapter_api;
OrtEnv* ort_env;

UniqueOrtSessionOptions CreateUniqueOrtSessionOptions() {
  OrtSessionOptions *options;
  THROW_IF_NOT_OK_MSG(ort_api->CreateSessionOptions(&options), ort_api);
  return UniqueOrtSessionOptions(options, ort_api->ReleaseSessionOptions);
}

winml::ILearningModelFeatureDescriptor FindValidBinding(
    wfc::IIterable<ILearningModelFeatureDescriptor> descriptors,
    const std::wstring& name) {
  for (auto descriptor : descriptors) {
    auto descriptor_native = descriptor.as<ILearningModelFeatureDescriptorNative>();
    WINML_EXPECT_NOT_EQUAL(nullptr, descriptor_native);

    const wchar_t* feature_name;
    uint32_t size;
    WINML_THROW_IF_FAILED(descriptor_native->GetName(&feature_name, &size));

    if (_wcsicmp(feature_name, name.c_str()) == 0) {
      return descriptor;
    }
  }
  throw std::runtime_error("Binding not found");
}

winml::ILearningModelFeatureDescriptor FindValidInputBinding(
    winml::LearningModel& model,
    const std::wstring& name) {
  return FindValidBinding(model.InputFeatures(), name);
}

winml::ILearningModelFeatureDescriptor FindValidOutputBinding(
    winml::LearningModel& model,
    const std::wstring& name) {
  return FindValidBinding(model.OutputFeatures(), name);
}

WinML::IValue* LoadImageValue(WinML::BindingContext& context, const std::wstring& image_path) {
  const auto software_bitmap = FileHelpers::GetSoftwareBitmapFromFile(FileHelpers::GetModulePath() + image_path);
  const auto video_frame = winrt::Windows::Media::VideoFrame::CreateWithSoftwareBitmap(software_bitmap);
  const auto image_feature_value = ImageFeatureValue::CreateFromVideoFrame(video_frame);

  auto value_provider = image_feature_value.as<WinML::ILotusValueProviderPrivate>();
  WinML::IValue *value;
  WINML_EXPECT_HRESULT_SUCCEEDED(value_provider->GetValue(context, &value));
  WINML_EXPECT_NOT_EQUAL(nullptr, value);
  return value;
}

WinML::IValue* LoadImageValue(LearningModelSession& session, const std::wstring& binding_name, const std::wstring& image_path) {
  auto model = session.Model();
  auto binding_descriptor = FindValidInputBinding(model, binding_name);
  WinML::BindingContext context{WinML::BindingType::kInput, session, binding_descriptor, {}, {}};
  return LoadImageValue(context, image_path);
}

// struct BindingContext {
//   BindingType type = BindingType::kInput;
//   winrt::Windows::AI::MachineLearning::LearningModelSession session = nullptr;
//   winrt::Windows::AI::MachineLearning::ILearningModelFeatureDescriptor descriptor = nullptr;
//   winrt::Windows::Foundation::Collections::IPropertySet properties = nullptr;
//   std::shared_ptr<PoolObjectWrapper> converter;
// };


void AdapterSessionTestSetup() {
  init_apartment();
  WINML_EXPECT_HRESULT_SUCCEEDED(Microsoft::WRL::MakeAndInitialize<WinML::OnnxruntimeEngineFactory>(&engine_factory));
  // WINML_EXPECT_HRESULT_SUCCEEDED(engine_factory.RuntimeClassInitialize());
  WINML_EXPECT_HRESULT_SUCCEEDED(engine_factory->GetOrtEnvironment(&ort_env));
  WINML_EXPECT_HRESULT_SUCCEEDED(engine_factory->EnableDebugOutput(true));
  WINML_EXPECT_NOT_EQUAL(nullptr, winml_adapter_api = engine_factory->UseWinmlAdapterApi());
  WINML_EXPECT_NOT_EQUAL(nullptr, ort_api = engine_factory->UseOrtApi());
  // TODO cleanup?
}

void AppendExecutionProvider_CPU() {
  const auto session_options = CreateUniqueOrtSessionOptions();
  THROW_IF_NOT_OK_MSG(winml_adapter_api->OrtSessionOptionsAppendExecutionProvider_CPU(session_options.get(), true), ort_api);
}

ID3D12Device* CreateD3DDevice() {
  ID3D12Device* device = nullptr;
  WINML_EXPECT_NO_THROW(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)));
  return device;
  // TODO Release at end of test
}

ID3D12CommandQueue* CreateD3DQueue(ID3D12Device* device) {
  ID3D12CommandQueue* queue = nullptr;
  D3D12_COMMAND_QUEUE_DESC command_queue_desc = {};
  command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  device->CreateCommandQueue(&command_queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
  return queue;
  // TODO Release at end of test
}

UniqueOrtSession CreateUniqueOrtSession(const UniqueOrtSessionOptions& unique_options) {
  OrtSession* session;
  THROW_IF_NOT_OK_MSG(winml_adapter_api->CreateSessionWithoutModel(ort_env, unique_options.get(), &session), ort_api);
  return UniqueOrtSession(session, ort_api->ReleaseSession);
}

UniqueOrtSession CreateUniqueOrtSession(const std::wstring& model_path, const UniqueOrtSessionOptions& unique_options) {
  OrtSession* session;
  ort_api->SetIntraOpNumThreads(unique_options.get(), 1);
  ort_api->SetSessionGraphOptimizationLevel(unique_options.get(), ORT_ENABLE_BASIC);
  THROW_IF_NOT_OK_MSG(winml_adapter_api->OrtSessionOptionsAppendExecutionProvider_CPU(unique_options.get(), true), ort_api);
  // ort_api->DisableMemPattern(unique_options.get());
  // const auto device = CreateD3DDevice();
  // const auto queue = CreateD3DQueue(device);
  // THROW_IF_NOT_OK_MSG(winml_adapter_api->OrtSessionOptionsAppendExecutionProvider_DML(unique_options.get(), device, queue), ort_api);
  THROW_IF_NOT_OK_MSG(ort_api->CreateSession(ort_env, model_path.c_str(), unique_options.get(), &session), ort_api);
  return UniqueOrtSession(session, ort_api->ReleaseSession);
}

void AppendExecutionProvider_DML() {
  GPUTEST
  const auto session_options = CreateUniqueOrtSessionOptions();

  const auto device = CreateD3DDevice();
  const auto queue = CreateD3DQueue(device);
  THROW_IF_NOT_OK_MSG(winml_adapter_api->OrtSessionOptionsAppendExecutionProvider_DML(session_options.get(), device, queue), ort_api);
}

void CreateWithoutModel() {
  const auto session_options = CreateUniqueOrtSessionOptions();
  CreateUniqueOrtSession(session_options);
}

void GetExecutionProvider_CPU() {
  const auto unique_options = CreateUniqueOrtSessionOptions();
  const auto model_path = FileHelpers::GetModulePath() + L"fns-candy.onnx";
  auto unique_session = CreateUniqueOrtSession(model_path, unique_options);

  // TODO load model
  // constexpr std::array<int64_t, 4> dimensions{1, 3, 224, 224};
  // constexpr size_t input_tensor_size = [&dimensions]() {
  //   size_t size = 1;
  //   for (auto dim : dimensions)
  //     size *= dim;
  //   return size;
  // } ();

  // OrtModel* model;
  // const std::string model_path = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(FileHelpers::GetModulePath()) + model_name;
  // winml_adapter_api->CreateModelFromPath(model_path.c_str(), model_path.size(), &model);


  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionInitialize(unique_session.get()), ort_api);

  OrtExecutionProvider* ort_provider;
  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionGetExecutionProvider(unique_session.get(), 0, &ort_provider), ort_api);
}

void GetExecutionProvider_DML() {
  GPUTEST
  const auto session_options = CreateUniqueOrtSessionOptions();
  const auto device = CreateD3DDevice();
  const auto queue = CreateD3DQueue(device);
  THROW_IF_NOT_OK_MSG(winml_adapter_api->OrtSessionOptionsAppendExecutionProvider_DML(session_options.get(), device, queue), ort_api);

  auto ort_session = CreateUniqueOrtSession(session_options);
  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionInitialize(ort_session.get()), ort_api);

  OrtExecutionProvider* ort_provider;
  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionGetExecutionProvider(ort_session.get(), 0, &ort_provider), ort_api);
  // Test if DML EP method can be called
  THROW_IF_NOT_OK_MSG(winml_adapter_api->DmlExecutionProviderFlushContext(ort_provider), ort_api);
}

void Initialize() {
  const auto unique_options = CreateUniqueOrtSessionOptions();
  auto unique_session = CreateUniqueOrtSession(unique_options);
  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionInitialize(unique_session.get()), ort_api);
}

void RegisterGraphTransformers() {
  const auto unique_options = CreateUniqueOrtSessionOptions();
  auto unique_session = CreateUniqueOrtSession(unique_options);

  winml_adapter_api->SessionRegisterGraphTransformers(unique_session.get());
}

void RegisterGraphTransformers_DML() {
  GPUTEST
  const auto unique_options = CreateUniqueOrtSessionOptions();
  auto unique_session = CreateUniqueOrtSession(unique_options);

  winml_adapter_api->SessionRegisterGraphTransformers(unique_session.get());
  // Assert that transformers were registered
}

void RegisterCustomRegistry() {
  IMLOperatorRegistry* registry;
  THROW_IF_NOT_OK_MSG(winml_adapter_api->CreateCustomRegistry(&registry), ort_api);
  // TODO add stuff to registry
  if (registry) {
    const auto unique_options = CreateUniqueOrtSessionOptions();
    auto unique_session = CreateUniqueOrtSession(unique_options);

    THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionRegisterCustomRegistry(unique_session.get(), registry), ort_api);
  }
}

void RegisterCustomRegistry_DML() {
  GPUTEST
  IMLOperatorRegistry* registry;
  THROW_IF_NOT_OK_MSG(winml_adapter_api->CreateCustomRegistry(&registry), ort_api);
  WINML_EXPECT_NOT_EQUAL(nullptr, registry);
  // TODO add stuff
  const auto unique_options = CreateUniqueOrtSessionOptions();
  auto unique_session = CreateUniqueOrtSession(unique_options);

  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionRegisterCustomRegistry(unique_session.get(), registry), ort_api);
}

void LoadAndPurloinModel() {
  const auto unique_options = CreateUniqueOrtSessionOptions();
  auto unique_session = CreateUniqueOrtSession(unique_options);

  // com_ptr<WinML::IEngineFactory> engine_factory;
  // WINML_THROW_IF_FAILED(CreateOnnxruntimeEngineFactory(engine_factory.put()));

  com_ptr<WinML::IModel> model;
  const auto model_path = "blah";
  WINML_THROW_IF_FAILED(engine_factory->CreateModel(model_path, sizeof(model_path), model.put()));

  Microsoft::WRL::ComPtr<WinML::IOnnxruntimeModel> onnxruntime_model;
  WINML_EXPECT_HRESULT_SUCCEEDED(model->QueryInterface(IID_PPV_ARGS(&onnxruntime_model)));
  OrtModel* ort_model;
  WINML_EXPECT_HRESULT_SUCCEEDED(onnxruntime_model->DetachOrtModel(&ort_model));
  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionLoadAndPurloinModel(unique_session.get(), ort_model), ort_api);
}

class MockSink : public onnxruntime::logging::ISink {
  std::vector<std::string> messages;

  void SendImpl(const onnxruntime::logging::Timestamp& timestamp, const std::string& logger_id, const onnxruntime::logging::Capture& message) override {
    // TODO Assert on profiling data
    std::cerr << "LOGGING\n";
  }
};

void Profiling() {
  const auto unique_options = CreateUniqueOrtSessionOptions();
  auto unique_session = CreateUniqueOrtSession(unique_options);

  auto sink = std::make_unique<MockSink>();
  const std::string logger_name("DefaultLogger");
  onnxruntime::logging::LoggingManager logging_manager(std::move(sink), onnxruntime::logging::Severity::kINFO, false, onnxruntime::logging::LoggingManager::InstanceType::Default, &logger_name);
  // onnxruntime::logging::Logger logger;
  winml_adapter_api->SessionStartProfiling(ort_env, unique_session.get());
  // winml_adapter_api->
}

void CopyInputAcrossDevices() {
  GPUTEST
  const auto unique_options = CreateUniqueOrtSessionOptions();
  auto unique_session = CreateUniqueOrtSession(unique_options);
  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionInitialize(unique_session.get()), ort_api);

  constexpr auto model_name = "fns-candy.onnx";
  const std::string model_path = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(FileHelpers::GetModulePath()) + model_name;
  constexpr std::array<int64_t, 4> dimensions{1, 3, 224, 224};
  constexpr size_t input_tensor_size = [&dimensions]() {
    size_t size = 1;
    for (auto dim : dimensions)
      size *= dim;
    return size;
  } ();

  OrtModel* model;
  winml_adapter_api->CreateModelFromPath(model_path.c_str(), model_path.size(), &model);

  OrtMemoryInfo* memory_info;
  THROW_IF_NOT_OK_MSG(ort_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info), ort_api);
  std::vector<float> input_tensor_values(input_tensor_size);
  OrtValue* input_tensor;
  THROW_IF_NOT_OK_MSG(ort_api->CreateTensorWithDataAsOrtValue(memory_info, input_tensor_values.data(), input_tensor_size * sizeof(float), dimensions.data(), 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor), ort_api);

  int is_tensor;
  THROW_IF_NOT_OK_MSG(ort_api->IsTensor(input_tensor, &is_tensor), ort_api);
  WINML_EXPECT_TRUE(is_tensor);

  OrtValue* dest_ort_value = nullptr;
  THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionCopyOneInputAcrossDevices(unique_session.get(), "input", input_tensor, &dest_ort_value), ort_api);

  ort_api->ReleaseMemoryInfo(memory_info);
}

void CopyInputAcrossDevices_DML() {
  GPUTEST

// HRESULT OnnxruntimeEngine::CreateOneInputAcrossDevices(const char* name, IValue* src, IValue** out) {
//   auto ort_api = engine_factory_->UseOrtApi();
//   auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

//   auto src_value = static_cast<OnnxruntimeValue*>(src);

//   bool is_set;
//   auto is_empty = SUCCEEDED(src_value->IsEmpty(&is_set)) && is_set;
//   auto is_tensor = SUCCEEDED(src_value->IsTensor(&is_set)) && is_set;

//   if (is_tensor && !is_empty) {
//     int16_t source_location;
//     int16_t input_required_location;
//     RETURN_HR_IF_NOT_OK_MSG(winml_adapter_api->ValueGetDeviceId(src_value->UseOrtValue(), &source_location),
//                             ort_api);
//     RETURN_HR_IF_NOT_OK_MSG(winml_adapter_api->SessionGetInputRequiredDeviceId(session_.get(), name, &input_required_location),
//                             ort_api);

//     if (source_location != input_required_location) {
//       OrtValue* dest_ort_value = nullptr;
//       RETURN_HR_IF_NOT_OK_MSG(winml_adapter_api->SessionCopyOneInputAcrossDevices(session_.get(), name,
//                                                                                   src_value->UseOrtValue(), &dest_ort_value),
//                               ort_api);
//       auto unique_dest_ort_value = UniqueOrtValue(dest_ort_value, ort_api->ReleaseValue);

//       RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<OnnxruntimeValue>(out, this, std::move(unique_dest_ort_value), UniqueOrtAllocator(nullptr, nullptr)));
//       return S_OK;
//     }
//   }

//   *out = src;
//   (*out)->AddRef();
//   return S_OK;
// }

  // const auto unique_options = CreateUniqueOrtSessionOptions();
  // auto unique_session = CreateUniqueOrtSession(unique_options);
  // THROW_IF_NOT_OK_MSG(winml_adapter_api->SessionInitialize(unique_session.get()), ort_api);

  // ...

  // RETURN_HR_IF_NOT_OK_MSG(winml_adapter_api->SessionCopyOneInputAcrossDevices(unique_session.get(), name, src_value->UseOrtValue(), &dest_ort_value),
}
}

const AdapterSessionTestAPi& getapi() {
  static constexpr AdapterSessionTestAPi api =
  {
    AdapterSessionTestSetup,
    AppendExecutionProvider_CPU,
    AppendExecutionProvider_DML,
    CreateWithoutModel,
    GetExecutionProvider_CPU,
    GetExecutionProvider_DML,
    Initialize,
    RegisterGraphTransformers,
    RegisterGraphTransformers_DML,
    RegisterCustomRegistry,
    RegisterCustomRegistry_DML,
    LoadAndPurloinModel,
    Profiling,
    CopyInputAcrossDevices,
    CopyInputAcrossDevices_DML
  };
  return api;
}
