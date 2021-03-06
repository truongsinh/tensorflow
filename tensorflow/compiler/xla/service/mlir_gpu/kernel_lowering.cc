/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/mlir_gpu/kernel_lowering.h"

#include <memory>

#include "absl/memory/memory.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"  // TF:local_config_mlir
#include "mlir/Conversion/LoopsToGPU/LoopsToGPUPass.h"  // TF:local_config_mlir
#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVM.h"  // TF:local_config_mlir
#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVMPass.h"  // TF:local_config_mlir
#include "mlir/Dialect/GPU/GPUDialect.h"  // TF:local_config_mlir
#include "mlir/Dialect/GPU/Passes.h"  // TF:local_config_mlir
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // TF:local_config_mlir
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"  // TF:local_config_mlir
#include "mlir/IR/Attributes.h"  // TF:local_config_mlir
#include "mlir/IR/Function.h"  // TF:local_config_mlir
#include "mlir/IR/Module.h"  // TF:local_config_mlir
#include "mlir/IR/OperationSupport.h"  // TF:local_config_mlir
#include "mlir/Pass/Pass.h"  // TF:local_config_mlir
#include "mlir/Pass/PassManager.h"  // TF:local_config_mlir
#include "mlir/Transforms/DialectConversion.h"  // TF:local_config_mlir
#include "mlir/Transforms/Passes.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"
#include "tensorflow/compiler/xla/util.h"

namespace xla {
namespace mlir_gpu {
namespace {

using ::mlir::ConversionTarget;
using ::mlir::FuncOp;
using ::mlir::LLVMTypeConverter;
using ::mlir::ModulePass;
using ::mlir::ModulePassBase;
using ::mlir::OwningRewritePatternList;
using ::mlir::PassManager;
using ::mlir::gpu::GPUDialect;
using ::mlir::LLVM::LLVMDialect;
using ::mlir::NVVM::NVVMDialect;

struct LowerKernelBodiesToNVVMPass
    : public ModulePass<LowerKernelBodiesToNVVMPass> {
 public:
  explicit LowerKernelBodiesToNVVMPass() = default;

  void runOnModule() override {
    auto module = getModule();
    ConversionTarget target(*module.getContext());
    LLVMTypeConverter converter(module.getContext());

    target.addLegalDialect<LLVMDialect>();
    target.addLegalDialect<NVVMDialect>();
    target.addDynamicallyLegalOp<FuncOp>(
        [&](FuncOp op) { return converter.isSignatureLegal(op.getType()); });

    OwningRewritePatternList patterns;
    populateStdToLLVMConversionPatterns(converter, patterns);
    populateGpuToNVVMConversionPatterns(converter, patterns);

    module.walk([this, &target, &patterns, &converter](FuncOp function) {
      if (!GPUDialect::isKernel(function)) {
        return;
      }
      if (failed(applyFullConversion(function, target, patterns, &converter))) {
        signalPassFailure();
      }
    });
  }
};

}  // namespace

Status LowerLHLOToGPU(mlir::ModuleOp module) {
  PassManager pm(module.getContext());

  // Transform element-wise operations to Affine.
  pm.addPass(::mlir::xla_lhlo::createLegalizeToAffinePass());
  // Transform affine to gpu launches.
  // TODO(b/137624192) This pass requires known dimensions. Generalization it.
  pm.addPass(::mlir::createSimpleLoopsToGPUPass(/*numBlockDims=*/0,
                                                /*numThreadDims=*/2));
  // Take launches to launches with kernels.
  pm.addPass(::mlir::createGpuKernelOutliningPass());
  // Some basic cleanup.
  pm.addPass(::mlir::createCSEPass());

  if (failed(pm.run(module))) {
    return InternalError("Lowering to NVVM IR failed.");
  }
  return Status::OK();
}

Status LowerKernelBodiesToNVVM(mlir::ModuleOp module) {
  // We cannot verify as the signature of the kernel is rewritten.
  PassManager pm(module.getContext(), /*verifyPasses=*/false);

  // Rewrite kernel functions to LLVM IR.
  pm.addPass(absl::make_unique<LowerKernelBodiesToNVVMPass>());
  // Some basic cleanup.
  pm.addPass(::mlir::createCSEPass());

  if (failed(pm.run(module))) {
    return InternalError("Lowering to NVVM IR failed.");
  }
  return Status::OK();
}

}  // namespace mlir_gpu
}  // namespace xla
