//===--- OrcCAPITest.cpp - Unit tests for the OrcJIT v2 C API ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OrcTestCommon.h"
#include "llvm-c/Core.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Orc.h"
#include "gtest/gtest.h"

using namespace llvm;

class OrcCAPIExecutionTest : public testing::Test, public OrcExecutionTest {};
void materializationUnitFn() {}

TEST(OrcCAPITest, SymbolStringPoolUniquing) {
  LLVMInitializeNativeTarget();
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
    char *Msg = LLVMGetErrorMessage(E);
    ADD_FAILURE() << "Failed to initialize OrcJIT: " << Msg;
    LLVMDisposeErrorMessage(Msg);
    return;
  }
  LLVMOrcExecutionSessionRef ES = LLVMOrcLLJITGetExecutionSession(Jit);
  LLVMOrcSymbolStringPoolEntryRef E1 = LLVMOrcExecutionSessionIntern(ES, "aaa");
  LLVMOrcSymbolStringPoolEntryRef E2 = LLVMOrcExecutionSessionIntern(ES, "aaa");
  LLVMOrcSymbolStringPoolEntryRef E3 = LLVMOrcExecutionSessionIntern(ES, "bbb");
  const char *SymbolName = LLVMOrcSymbolStringPoolEntryStr(E1);
  ASSERT_EQ(E1, E2) << "String pool entries are not unique";
  ASSERT_NE(E1, E3) << "Unique symbol pool entries are equal";
  ASSERT_STREQ("aaa", SymbolName) << "String value of symbol is not equal";
  LLVMOrcDisposeLLJIT(Jit);
}

TEST(OrcCAPITest, JITDylibLookup) {
  LLVMInitializeNativeTarget();
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
    char *Msg = LLVMGetErrorMessage(E);
    ADD_FAILURE() << "Failed to initialize OrcJIT: " << Msg;
    LLVMDisposeErrorMessage(Msg);
    return;
  }
  LLVMOrcExecutionSessionRef ES = LLVMOrcLLJITGetExecutionSession(Jit);
  LLVMOrcJITDylibRef DoesNotExist =
      LLVMOrcExecutionSessionGetJITDylibByName(ES, "test");
  ASSERT_FALSE(!!DoesNotExist);
  LLVMOrcJITDylibRef L1 = LLVMOrcExecutionSessionCreateBareJITDylib(ES, "test");
  LLVMOrcJITDylibRef L2 = LLVMOrcExecutionSessionGetJITDylibByName(ES, "test");
  ASSERT_EQ(L1, L2) << "Located JIT Dylib is not equal to original";
  LLVMOrcDisposeLLJIT(Jit);
}

TEST(OrcCAPITest, MaterializationUnitCreation) {
  LLVMInitializeNativeTarget();
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
    char *Msg = LLVMGetErrorMessage(E);
    ADD_FAILURE() << "Failed to initialize OrcJIT: " << Msg;
    LLVMDisposeErrorMessage(Msg);
    return;
  }
  LLVMOrcExecutionSessionRef ES = LLVMOrcLLJITGetExecutionSession(Jit);
  LLVMOrcJITDylibRef Dylib = LLVMOrcLLJITGetMainJITDylib(Jit);
  LLVMOrcSymbolStringPoolEntryRef Name =
      LLVMOrcExecutionSessionIntern(ES, "test");
  LLVMJITSymbolFlags Flags = {LLVMJITSymbolGenericFlagsWeak, 0};
  LLVMOrcJITTargetAddress Addr = (intptr_t)(&materializationUnitFn);
  LLVMJITEvaluatedSymbol Sym = {Addr, Flags};
  LLVMJITCSymbolMapPair Pair = {Name, Sym};
  LLVMJITCSymbolMapPair Pairs[] = {Pair};
  LLVMOrcMaterializationUnitRef MU = LLVMOrcAbsoluteSymbols(Pairs, 1);
  LLVMOrcJITDylibDefine(Dylib, MU);
  LLVMOrcJITTargetAddress OutAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &OutAddr, "test")) {
    char *Msg = LLVMGetErrorMessage(E);
    ADD_FAILURE() << "Failed to look up symbol named \"test\" in JIT: " << Msg;
    LLVMDisposeErrorMessage(Msg);
    return;
  }
  ASSERT_EQ(Addr, OutAddr);
}

TEST_F(OrcCAPIExecutionTest, ExecutionTest) {
  if (!SupportsJIT)
    return;

  // This test performs OrcJIT compilation of a simple sum module
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  const char *Triple = LLVMGetDefaultTargetTriple();
  char *Msg = 0;
  LLVMTargetRef Target;
  if (LLVMGetTargetFromTriple(Triple, &Target, &Msg)) {
    ADD_FAILURE() << "Failed to retrieve target from triple: " << Triple;
    return;
  }
  LLVMTargetMachineRef TM = LLVMCreateTargetMachine(
      Target, Triple, "generic", "", LLVMCodeGenLevelNone, LLVMRelocDefault,
      LLVMCodeModelDefault);
  LLVMOrcJITTargetMachineBuilderRef TMB =
      LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(TM);
  if (LLVMErrorRef E = LLVMOrcJITTargetMachineBuilderDetectHost(&TMB)) {
    char *Err = LLVMGetErrorMessage(E);
    ADD_FAILURE() << "Failed to detect host from TMB: " << Err;
    LLVMDisposeErrorMessage(Err);
    LLVMOrcDisposeJITTargetMachineBuilder(TMB);
    LLVMDisposeTargetMachine(TM);
    return;
  }
  // Construct the JIT engine
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  LLVMOrcCreateLLJIT(&Jit, Builder);
  const char *TripleFromLL = LLVMOrcLLJITGetTripleString(Jit);
  ASSERT_STREQ(Triple, TripleFromLL);
  LLVMOrcThreadSafeContextRef TSC = LLVMOrcCreateNewThreadSafeContext();
  LLVMContextRef C = LLVMOrcThreadSafeContextGetContext(TSC);
  LLVMModuleRef Mod = LLVMModuleCreateWithNameInContext("Test", C);
  {
    LLVMTypeRef Int32Ty = LLVMInt32TypeInContext(C);
    LLVMTypeRef ParamTys[] = {Int32Ty, Int32Ty};
    LLVMTypeRef TestFnTy = LLVMFunctionType(Int32Ty, ParamTys, 2, 0);
    LLVMValueRef TestFn = LLVMAddFunction(Mod, "sum", TestFnTy);
    LLVMBuilderRef IRBuilder = LLVMCreateBuilderInContext(C);
    LLVMBasicBlockRef EntryBB = LLVMAppendBasicBlock(TestFn, "entry");
    LLVMPositionBuilderAtEnd(IRBuilder, EntryBB);
    LLVMValueRef Arg1 = LLVMGetParam(TestFn, 0);
    LLVMValueRef Arg2 = LLVMGetParam(TestFn, 1);
    LLVMValueRef Sum = LLVMBuildAdd(IRBuilder, Arg1, Arg2, "");
    LLVMBuildRet(IRBuilder, Sum);
  }
  LLVMOrcThreadSafeModuleRef TSM = LLVMOrcCreateNewThreadSafeModule(Mod, TSC);
  LLVMOrcJITDylibRef MainDylib = LLVMOrcLLJITGetMainJITDylib(Jit);
  if (LLVMErrorRef E = LLVMOrcLLJITAddLLVMIRModule(Jit, MainDylib, TSM)) {
    char *Err = LLVMGetErrorMessage(E);
    ADD_FAILURE() << "Failed to add LLVM ir module to LLJIT: " << Err;
    LLVMDisposeErrorMessage(Err);
    LLVMOrcDisposeLLJIT(Jit);
    return;
  }
  LLVMOrcJITTargetAddress TestFnAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &TestFnAddr, "sum")) {
    char *Err = LLVMGetErrorMessage(E);
    ADD_FAILURE() << "Failed to locate \"sum\" symbol: " << Err;
    LLVMDisposeErrorMessage(Err);
    LLVMOrcDisposeLLJIT(Jit);
    return;
  }
  auto *SumFn = (int32_t(*)(int32_t, int32_t))TestFnAddr;
  int32_t Result = SumFn(1, 1);
  ASSERT_EQ(2, Result);
  LLVMOrcDisposeLLJIT(Jit);
}