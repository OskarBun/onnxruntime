// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <sstream>

#include "gmock/gmock.h"
#include "test/providers/provider_test_utils.h"
#include <exception>
#include <memory>
#include "core/common/logging/logging.h"
#include "core/common/logging/sinks/clog_sink.h"
#include "core/session/inference_session.h"
#include "core/graph/function_container.h"
#include "test/util/include/default_providers.h"

using namespace ::onnxruntime::logging;

namespace onnxruntime {
namespace test {

// Check functions for tensor types

// The default implementation compares for equality, specialized versions for other types are below
template <typename T>
void Check(const OpTester::Data& expected_data, const Tensor& output_tensor) {
  auto& expected_tensor = expected_data.data_.Get<Tensor>();
  auto* expected = expected_tensor.template Data<T>();
  auto* output = output_tensor.template Data<T>();
  auto size = output_tensor.Shape().Size();

  for (int i = 0; i < size; ++i) {
    EXPECT_EQ(expected[i], output[i]);
  }
}

template <>
void Check<float>(const OpTester::Data& expected_data, const Tensor& output_tensor) {
  auto& expected_tensor = expected_data.data_.Get<Tensor>();
  auto* expected = expected_tensor.template Data<float>();
  auto* output = output_tensor.template Data<float>();
  auto size = output_tensor.Shape().Size();

  bool has_abs_err = expected_data.absolute_error_.has_value();
  bool has_rel_err = expected_data.relative_error_.has_value();

  float threshold = 0.001f;
#ifdef USE_CUDA
  threshold = 0.005f;
#endif

  for (int i = 0; i < size; ++i) {
    if (std::isinf(expected[i]))  // Test infinity for equality
      EXPECT_EQ(expected[i], output[i]);
    else {
      if (!has_abs_err && !has_rel_err) {
        // the default for existing tests
        EXPECT_NEAR(expected[i], output[i], threshold);
      } else {
        if (has_abs_err) {
          EXPECT_NEAR(expected[i], output[i], expected_data.absolute_error_.value());
        }
        if (has_rel_err) {
          EXPECT_NEAR(expected[i], output[i], expected_data.relative_error_.value() * std::abs(expected[i]));
        }
      }
    }
  }
}

template <typename Type>
void CheckDispatch(MLDataType type, const OpTester::Data& expected_data, const Tensor& output_tensor) {
  if (type == DataTypeImpl::GetType<Type>())
    Check<Type>(expected_data, output_tensor);
  else
    ONNXRUNTIME_THROW("OpTester:Check() not implemented for output tensor type of ", type);
}

template <typename Type, typename Next, typename... Types>
void CheckDispatch(MLDataType type, const OpTester::Data& expected_data, const Tensor& output_tensor) {
  if (type == DataTypeImpl::GetType<Type>())
    Check<Type>(expected_data, output_tensor);
  else
    CheckDispatch<Next, Types...>(type, expected_data, output_tensor);
}

void Check(const OpTester::Data& expected_data, const Tensor& output_tensor) {
  ONNXRUNTIME_ENFORCE(expected_data.data_.Get<Tensor>().Shape() == output_tensor.Shape(),
              "Expected output shape [" + expected_data.data_.Get<Tensor>().Shape().ToString() +
                  "] did not match run output shape [" +
                  output_tensor.Shape().ToString() + "]");

  CheckDispatch<bool, float, double, uint8_t, uint16_t, uint32_t, uint64_t, int8_t, int16_t, int32_t, int64_t, std::string, MLFloat16>(output_tensor.DataType(), expected_data, output_tensor);
}

// Check for non tensor types

template <typename T>
void Check(const OpTester::Data& expected_data, const T& run_output) {
  EXPECT_EQ(expected_data.data_.Get<T>(), run_output);
}

template <typename Type>
void CheckDispatch(MLDataType type, const OpTester::Data& expected_data, MLValue& mlvalue) {
  if (type == DataTypeImpl::GetType<Type>())
    Check<Type>(expected_data, mlvalue.Get<Type>());
  else
    ONNXRUNTIME_THROW("OpTester:Check() not implemented for output tensor type of ", type);
}

template <typename Type, typename Next, typename... Types>
void CheckDispatch(MLDataType type, const OpTester::Data& expected_data, MLValue& mlvalue) {
  if (type == DataTypeImpl::GetType<Type>())
    Check<Type>(expected_data, mlvalue.Get<Type>());
  else
    CheckDispatch<Next, Types...>(type, expected_data, mlvalue);
}

void Check(const OpTester::Data& expected_data, MLValue& mlvalue) {
  CheckDispatch<VectorMapStringToFloat, VectorMapInt64ToFloat>(expected_data.data_.Type(), expected_data, mlvalue);
}

OpTester::~OpTester() {
#if _DEBUG
  if (!run_called_) {
    std::cerr << "Someone forgot to call OpTester::Run()" << std::endl;
    __debugbreak();
  }
#endif
}

void OpTester::FillFeedsAndOutputNames(const std::vector<onnxruntime::NodeArg*>&,
                                       const std::vector<onnxruntime::NodeArg*>& output_defs,
                                       std::unordered_map<std::string, MLValue>& feeds,
                                       std::vector<std::string>& output_names) {
  for (auto& output : output_defs) {
    if (output->Exists())
      output_names.push_back(output->Name());
  }

  for (auto& input : input_data_) {
    if (input.def_.Exists())
      feeds[input.def_.Name()] = input.data_;
  }
}

void OpTester::SetOutputAbsErr(const char* name, float v) {
  auto it = std::find_if(
      output_data_.begin(),
      output_data_.end(),
      [name](Data& data) {
        return (data.def_.Name() == name);
      });
  ONNXRUNTIME_ENFORCE(it != output_data_.end());
  it->absolute_error_ = optional<float>(v);
}

void OpTester::SetOutputRelErr(const char* name, float v) {
  auto it = std::find_if(
      output_data_.begin(),
      output_data_.end(),
      [name](Data& data) {
        return (data.def_.Name() == name);
      });
  ONNXRUNTIME_ENFORCE(it != output_data_.end());
  it->relative_error_ = optional<float>(v);
}

void OpTester::Run(ExpectResult expect_result, const std::string& expected_failure_string, const std::unordered_set<std::string>& excluded_provider_types) {
  try {
#if _DEBUG
    run_called_ = true;
#endif
    // Generate the input & output def lists
    std::vector<onnxruntime::NodeArg*> input_defs;
    std::vector<onnxruntime::NodeArg*> output_defs;

    for (auto& data : input_data_) {
      input_defs.push_back(&data.def_);
    }

    for (auto& data : output_data_) {
      output_defs.push_back(&data.def_);
    }

    const IOnnxRuntimeOpSchemaRegistryList* local_schema_registries = custom_schema_registries_.empty()
                                                                          ? nullptr
                                                                          : &custom_schema_registries_;

    // Create a simple model
    std::unordered_map<std::string, int> domain_to_version;
    domain_to_version[domain_] = opset_version_;
    auto p_model = std::make_unique<onnxruntime::Model>("test", false, ModelMetaData(),
                                                        local_schema_registries, domain_to_version);
    onnxruntime::Graph& graph = p_model->MainGraph();
    auto& node = *graph.AddNode("node1", op_, op_, input_defs, output_defs, nullptr, domain_);

    // Add the attributes if any
    for (auto& add_attribute_fn : add_attribute_funcs_)
      add_attribute_fn(node);

    Status status = graph.Resolve();
    //ONNXRUNTIME_ENFORCE(status.IsOK(), status.ErrorMessage());
    if (!status.IsOK()) {
      if (expect_result == ExpectResult::kExpectFailure) {
        EXPECT_TRUE(!status.IsOK());
        EXPECT_THAT(status.ErrorMessage(), testing::HasSubstr(expected_failure_string));
      } else {
        LOGS_DEFAULT(ERROR) << "Resolve failed with status: " << status.ErrorMessage();
        EXPECT_TRUE(status.IsOK()) << status.ErrorMessage();
      }
    }
    if (!status.IsOK()) {
      return;
    }

    // Hookup the inputs and outputs
    std::unordered_map<std::string, MLValue> feeds;
    std::vector<std::string> output_names;
    FillFeedsAndOutputNames(input_defs, output_defs, feeds, output_names);

    // Run the model
    SessionOptions so;
    so.session_logid = op_;
    so.session_log_verbosity_level = 1;

    static const std::string all_provider_types[] = {
        kCpuExecutionProvider,
        kCudaExecutionProvider,
        kMklDnnExecutionProvider,
        kNupharExecutionProvider,
    };

    for (const auto& provider_type : all_provider_types) {
      if (excluded_provider_types.count(provider_type) > 0)
        continue;

      InferenceSession session_object{so};

      for (auto& custom_session_registry : custom_session_registries_)
        session_object.RegisterCustomRegistry(custom_session_registry);

      std::unique_ptr<IExecutionProvider> execution_provider;
      if (provider_type == onnxruntime::kCpuExecutionProvider)
        execution_provider = DefaultCpuExecutionProvider();
      else if (provider_type == onnxruntime::kCudaExecutionProvider)
        execution_provider = DefaultCudaExecutionProvider();
      else if (provider_type == onnxruntime::kMklDnnExecutionProvider)
        execution_provider = DefaultMkldnnExecutionProvider();
      else if (provider_type == onnxruntime::kNupharExecutionProvider)
        execution_provider = DefaultNupharExecutionProvider();

      // skip if execution provider is disabled
      if (execution_provider == nullptr)
        continue;

      //if node is not registered for the provider, skip
      node.SetExecutionProviderType(provider_type);
      auto reg = execution_provider->GetKernelRegistry();
      const KernelCreateInfo* kci;
      if (!reg->FindKernel(node, &kci).IsOK()) {
        continue;
      }
      EXPECT_TRUE(session_object.RegisterExecutionProvider(std::move(execution_provider)).IsOK());

      std::stringstream s1;
      p_model->ToProto().SerializeToOstream(&s1);
      status = session_object.Load(s1);
      EXPECT_TRUE(status.IsOK()) << status.ErrorMessage();
      if (!status.IsOK()) {
        LOGS_DEFAULT(ERROR) << "Load failed with status: " << status.ErrorMessage();
        return;
      }

      status = session_object.Initialize();
      if (!status.IsOK()) {
        if (expect_result == ExpectResult::kExpectFailure) {
          EXPECT_TRUE(!status.IsOK());
          EXPECT_THAT(status.ErrorMessage(), testing::HasSubstr(expected_failure_string));
        } else {
          LOGS_DEFAULT(ERROR) << "Initialize failed with status: " << status.ErrorMessage();
          EXPECT_TRUE(status.IsOK()) << status.ErrorMessage();
        }
      }
      if (!status.IsOK()) {
        return;
      }

      RunOptions run_options;
      run_options.run_tag = op_;
      run_options.run_log_verbosity_level = 1;
      std::vector<MLValue> fetches;
      status = session_object.Run(run_options, feeds, output_names, &fetches);
      if (status.IsOK()) {
        EXPECT_TRUE(expect_result == ExpectResult::kExpectSuccess);
        if (expect_result == ExpectResult::kExpectFailure) {
          return;
        }
      } else {
        if (expect_result == ExpectResult::kExpectFailure) {
          EXPECT_THAT(status.ErrorMessage(), testing::HasSubstr(expected_failure_string));
        } else {
          LOGS_DEFAULT(ERROR) << "Run failed with status: " << status.ErrorMessage();
          EXPECT_TRUE(status.IsOK()) << status.ErrorMessage();
        }
        return;
      }

      // Verify the outputs
      // Todo: support check output with map/sequence/....
      size_t idx = 0;
      for (auto& expected_data : output_data_) {
        MLValue& mlvalue = fetches[idx];
        if (mlvalue.Fence())
          mlvalue.Fence()->BeforeUsingAsInput(onnxruntime::kCpuExecutionProvider, 0);

        if (expected_data.def_.Exists()) {  // optional outputs won't exist
          if (expected_data.data_.IsTensor()) {
            Check(expected_data, mlvalue.Get<Tensor>());
          } else {
            Check(expected_data, mlvalue);
          }
          ++idx;

          // skip missing trailing optional outputs
          if (idx == fetches.size())
            break;
        }
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << ex.what();
    // rethrow as some tests for error handling expect this
    throw;
  }
}
}  // namespace test
}  // namespace onnxruntime
