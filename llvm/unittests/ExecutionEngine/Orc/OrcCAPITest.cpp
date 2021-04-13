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

// OrcCAPITestBase contains several helper methods and pointers for unit tests
// written for the LLVM-C API. It provides the following helpers:
//
// 1. Jit: an LLVMOrcLLJIT instance which is freed upon test exit
// 2. ExecutionSession: the LLVMOrcExecutionSession for the JIT
// 3. MainDylib: the main JITDylib for the LLJIT instance
// 4. reportError: helper method for reporting an LLVMErrorRef to GTest
// 5. materializationUnitFn: function pointer to an empty function, used for
//                           materialization unit testing
// 6. definitionGeneratorFn: function pointer for a basic
//                           LLVMOrcCAPIDefinitionGeneratorTryToGenerateFunction
// 7. createTestModule: helper method for creating a basic thread-safe-module
class OrcCAPITestBase : public testing::Test, public OrcExecutionTest {
protected:
  LLVMOrcLLJITRef Jit;
  LLVMOrcExecutionSessionRef ExecutionSession;
  LLVMOrcJITDylibRef MainDylib;
  OrcCAPITestBase() {
    LLVMInitializeNativeTarget();
    LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
    if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
      reportError(E, "Failed to create LLJIT");
      return;
    }
    ExecutionSession = LLVMOrcLLJITGetExecutionSession(Jit);
    MainDylib = LLVMOrcLLJITGetMainJITDylib(Jit);
  }
  ~OrcCAPITestBase() { LLVMOrcDisposeLLJIT(Jit); }
  void reportError(LLVMErrorRef E, const char *Description) {
    char *Message = LLVMGetErrorMessage(E);
    ADD_FAILURE() << Description << ": " << Message;
    LLVMDisposeErrorMessage(Message);
  }
  static void materializationUnitFn() {}
  // Stub definition generator, where all Names are materialized from the
  // materializationUnitFn() test function and defined into the JIT Dylib
  static LLVMErrorRef
  definitionGeneratorFn(LLVMOrcDefinitionGeneratorRef G, void *Ctx,
                        LLVMOrcLookupStateRef *LS, LLVMOrcLookupKind K,
                        LLVMOrcJITDylibRef JD, LLVMOrcJITDylibLookupFlags F,
                        LLVMOrcCLookupSet Names, size_t NamesCount) {
    for (size_t I = 0; I < NamesCount; I++) {
      LLVMOrcCLookupSetElement Element = Names[I];
      LLVMOrcJITTargetAddress Addr =
          (LLVMOrcJITTargetAddress)(&materializationUnitFn);
      LLVMJITSymbolFlags Flags = {LLVMJITSymbolGenericFlagsWeak, 0};
      LLVMJITEvaluatedSymbol Sym = {Addr, Flags};
      LLVMJITCSymbolMapPair Pair = {Element.Name, Sym};
      LLVMJITCSymbolMapPair Pairs[] = {Pair};
      LLVMOrcMaterializationUnitRef MU = LLVMOrcAbsoluteSymbols(Pairs, 1);
      LLVMOrcJITDylibDefine(JD, MU);
    }
    return LLVMErrorSuccess;
  }
  // create a test LLVM IR module containing a function named "sum" which has
  // returns the sum of its two parameters
  static LLVMOrcThreadSafeModuleRef createTestModule() {
    LLVMOrcThreadSafeContextRef TSC = LLVMOrcCreateNewThreadSafeContext();
    LLVMContextRef Ctx = LLVMOrcThreadSafeContextGetContext(TSC);
    LLVMModuleRef Mod = LLVMModuleCreateWithNameInContext("test", Ctx);
    {
      LLVMTypeRef Int32Ty = LLVMInt32TypeInContext(Ctx);
      LLVMTypeRef ParamTys[] = {Int32Ty, Int32Ty};
      LLVMTypeRef TestFnTy = LLVMFunctionType(Int32Ty, ParamTys, 2, 0);
      LLVMValueRef TestFn = LLVMAddFunction(Mod, "sum", TestFnTy);
      LLVMBuilderRef IRBuilder = LLVMCreateBuilderInContext(Ctx);
      LLVMBasicBlockRef EntryBB = LLVMAppendBasicBlock(TestFn, "entry");
      LLVMPositionBuilderAtEnd(IRBuilder, EntryBB);
      LLVMValueRef Arg1 = LLVMGetParam(TestFn, 0);
      LLVMValueRef Arg2 = LLVMGetParam(TestFn, 1);
      LLVMValueRef Sum = LLVMBuildAdd(IRBuilder, Arg1, Arg2, "");
      LLVMBuildRet(IRBuilder, Sum);
      LLVMDisposeBuilder(IRBuilder);
    }
    LLVMOrcThreadSafeModuleRef TSM = LLVMOrcCreateNewThreadSafeModule(Mod, TSC);
    return TSM;
  }
};

TEST_F(OrcCAPITestBase, SymbolStringPoolUniquing) {
  LLVMOrcSymbolStringPoolEntryRef E1 =
      LLVMOrcExecutionSessionIntern(ExecutionSession, "aaa");
  LLVMOrcSymbolStringPoolEntryRef E2 =
      LLVMOrcExecutionSessionIntern(ExecutionSession, "aaa");
  LLVMOrcSymbolStringPoolEntryRef E3 =
      LLVMOrcExecutionSessionIntern(ExecutionSession, "bbb");
  const char *SymbolName = LLVMOrcSymbolStringPoolEntryStr(E1);
  ASSERT_EQ(E1, E2) << "String pool entries are not unique";
  ASSERT_NE(E1, E3) << "Unique symbol pool entries are equal";
  ASSERT_STREQ("aaa", SymbolName) << "String value of symbol is not equal";
  LLVMOrcReleaseSymbolStringPoolEntry(E1);
  LLVMOrcReleaseSymbolStringPoolEntry(E2);
  LLVMOrcReleaseSymbolStringPoolEntry(E3);
}

TEST_F(OrcCAPITestBase, JITDylibLookup) {
  LLVMOrcJITDylibRef DoesNotExist =
      LLVMOrcExecutionSessionGetJITDylibByName(ExecutionSession, "test");
  ASSERT_FALSE(!!DoesNotExist);
  LLVMOrcJITDylibRef L1 =
      LLVMOrcExecutionSessionCreateBareJITDylib(ExecutionSession, "test");
  LLVMOrcJITDylibRef L2 =
      LLVMOrcExecutionSessionGetJITDylibByName(ExecutionSession, "test");
  ASSERT_EQ(L1, L2) << "Located JIT Dylib is not equal to original";
}

TEST_F(OrcCAPITestBase, MaterializationUnitCreation) {
  LLVMOrcSymbolStringPoolEntryRef Name =
      LLVMOrcExecutionSessionIntern(ExecutionSession, "test");
  LLVMJITSymbolFlags Flags = {LLVMJITSymbolGenericFlagsWeak, 0};
  LLVMOrcJITTargetAddress Addr =
      (LLVMOrcJITTargetAddress)(&materializationUnitFn);
  LLVMJITEvaluatedSymbol Sym = {Addr, Flags};
  LLVMJITCSymbolMapPair Pair = {Name, Sym};
  LLVMJITCSymbolMapPair Pairs[] = {Pair};
  LLVMOrcMaterializationUnitRef MU = LLVMOrcAbsoluteSymbols(Pairs, 1);
  LLVMOrcJITDylibDefine(MainDylib, MU);
  LLVMOrcJITTargetAddress OutAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &OutAddr, "test")) {
    reportError(E, "Failed to look up symbol named \"test\" in main Dylib");
    return;
  }
  ASSERT_EQ(Addr, OutAddr);
  LLVMOrcReleaseSymbolStringPoolEntry(Name);
}

TEST_F(OrcCAPITestBase, DefinitionGenerators) {
  LLVMOrcDefinitionGeneratorRef Gen =
      LLVMOrcCreateCustomCAPIDefinitionGenerator(&definitionGeneratorFn,
                                                 nullptr);
  LLVMOrcJITDylibAddGenerator(MainDylib, Gen);
  LLVMOrcJITTargetAddress OutAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &OutAddr, "test")) {
    reportError(E, "Symbol \"test\" was not generated from Dylib Generator");
    return;
  }
  LLVMOrcJITTargetAddress ExpectedAddr =
      (LLVMOrcJITTargetAddress)(&materializationUnitFn);
  ASSERT_EQ(ExpectedAddr, OutAddr);
}

TEST_F(OrcCAPITestBase, ResourceTrackerDefinitionLifetime) {
  // This test case ensures that all symbols loaded into a JITDylib with a
  // ResourceTracker attached are cleared from the JITDylib once the RT is
  // removed.
  LLVMOrcResourceTrackerRef RT =
      LLVMOrcJITDylibCreateResourceTracker(MainDylib);
  LLVMOrcThreadSafeModuleRef TSM = createTestModule();
  if (LLVMErrorRef E = LLVMOrcLLJITAddLLVMIRModuleWithRT(Jit, RT, TSM)) {
    reportError(E, "Failed to add LLVM IR module to LLJIT");
    return;
  }
  LLVMOrcJITTargetAddress TestFnAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &TestFnAddr, "sum")) {
    reportError(E, "Failed to locate \"sum\" symbol");
    return;
  }
  ASSERT_TRUE(!!TestFnAddr);
  LLVMOrcResourceTrackerRemove(RT);
  LLVMOrcJITTargetAddress OutAddr;
  LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &OutAddr, "sum");
  ASSERT_TRUE(!!E);
  ASSERT_FALSE(!!OutAddr);
  LLVMOrcExecutionSessionRef ES = LLVMOrcLLJITGetExecutionSession(Jit);
  // FIXME: Provide a better way of clearing dangling references in
  //  SymbolStringPool from implicit calls
  LLVMOrcSymbolStringPoolEntryRef Name =
      LLVMOrcExecutionSessionIntern(ES, "sum");
  LLVMOrcReleaseSymbolStringPoolEntry(Name);
  LLVMOrcReleaseSymbolStringPoolEntry(Name);
}

TEST_F(OrcCAPITestBase, ExecutionTest) {
  if (!SupportsJIT)
    return;

  using SumFunctionType = int32_t(*)(int32_t, int32_t);

  // This test performs OrcJIT compilation of a simple sum module
  LLVMInitializeNativeAsmPrinter();
  LLVMOrcThreadSafeModuleRef TSM = createTestModule();
  if (LLVMErrorRef E = LLVMOrcLLJITAddLLVMIRModule(Jit, MainDylib, TSM)) {
    reportError(E, "Failed to add LLVM IR module to LLJIT");
    return;
  }
  LLVMOrcJITTargetAddress TestFnAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &TestFnAddr, "sum")) {
    reportError(E, "Failed to locate \"sum\" symbol");
    return;
  }
  auto *SumFn = (SumFunctionType)(TestFnAddr);
  int32_t Result = SumFn(1, 1);
  ASSERT_EQ(2, Result);
}