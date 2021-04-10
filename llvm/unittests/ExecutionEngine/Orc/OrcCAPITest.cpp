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

class OrcCAPITestBase : public testing::Test, public OrcExecutionTest {
protected:
  void reportError(LLVMErrorRef E, const char *Description) {
    char *Message = LLVMGetErrorMessage(E);
    ADD_FAILURE() << Description << ": " << Message;
    LLVMDisposeErrorMessage(Message);
  }
  static void materializationUnitFn() {}
};

TEST_F(OrcCAPITestBase, SymbolStringPoolUniquing) {
  LLVMInitializeNativeTarget();
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
    reportError(E, "Failed to initialize JIT");
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
  LLVMOrcReleaseSymbolStringPoolEntry(E1);
  LLVMOrcReleaseSymbolStringPoolEntry(E2);
  LLVMOrcReleaseSymbolStringPoolEntry(E3);
  LLVMOrcDisposeLLJIT(Jit);
}

TEST_F(OrcCAPITestBase, JITDylibLookup) {
  LLVMInitializeNativeTarget();
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
    reportError(E, "Failed to initialize JIT");
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

TEST_F(OrcCAPITestBase, MaterializationUnitCreation) {
  LLVMInitializeNativeTarget();
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
    reportError(E, "Failed to initialize JIT");
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
    reportError(E, "Failed to look up symbol named \"test\" in main Dylib");
    return;
  }
  ASSERT_EQ(Addr, OutAddr);
  LLVMOrcReleaseSymbolStringPoolEntry(Name);
  LLVMOrcDisposeLLJIT(Jit);
}

TEST_F(OrcCAPITestBase, ResourceTrackerDefinitionLifetime) {
  // This test case ensures that all symbols loaded into a JITDylib with a
  // ResourceTracker attached are cleared from the JITDylib once the RT is
  // removed.
  LLVMInitializeNativeTarget();
  LLVMOrcLLJITRef Jit;
  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  if (LLVMErrorRef E = LLVMOrcCreateLLJIT(&Jit, Builder)) {
    reportError(E, "Failed to initialize JIT");
    return;
  }
  LLVMOrcJITDylibRef MainDylib = LLVMOrcLLJITGetMainJITDylib(Jit);
  LLVMOrcResourceTrackerRef RT =
      LLVMOrcJITDylibCreateResourceTracker(MainDylib);
  LLVMOrcThreadSafeContextRef TSC = LLVMOrcCreateNewThreadSafeContext();
  LLVMContextRef Ctx = LLVMOrcThreadSafeContextGetContext(TSC);
  LLVMModuleRef Mod = LLVMModuleCreateWithNameInContext("test", Ctx);
  {
    LLVMTypeRef VoidType = LLVMVoidTypeInContext(Ctx);
    LLVMTypeRef FuncTy = LLVMFunctionType(VoidType, {}, 0, 0);
    LLVMValueRef Func = LLVMAddFunction(Mod, "test", FuncTy);
    LLVMBuilderRef IRBuilder = LLVMCreateBuilderInContext(Ctx);
    LLVMBasicBlockRef EntryBB = LLVMAppendBasicBlock(Func, "entry");
    LLVMPositionBuilderAtEnd(IRBuilder, EntryBB);
    LLVMBuildRetVoid(IRBuilder);
    LLVMDisposeBuilder(IRBuilder);
  }
  LLVMOrcThreadSafeModuleRef TSM = LLVMOrcCreateNewThreadSafeModule(Mod, TSC);
  if (LLVMErrorRef E = LLVMOrcLLJITAddLLVMIRModuleWithRT(Jit, RT, TSM)) {
    reportError(E, "Failed to add LLVM IR module to LLJIT");
    LLVMOrcDisposeLLJIT(Jit);
    return;
  }
  LLVMOrcJITTargetAddress TestFnAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &TestFnAddr, "test")) {
    reportError(E, "Failed to locate \"sum\" symbol");
    LLVMOrcDisposeLLJIT(Jit);
    return;
  }
  ASSERT_TRUE(!!TestFnAddr);
  LLVMOrcResourceTrackerRemove(RT);
  LLVMOrcJITTargetAddress OutAddr;
  LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &OutAddr, "test");
  ASSERT_TRUE(!!E);
  ASSERT_FALSE(!!OutAddr);
  LLVMOrcExecutionSessionRef ES = LLVMOrcLLJITGetExecutionSession(Jit);
  // FIXME: Provide a better way of clearing dangling references in
  //  SymbolStringPool from external items like LLVM IR modules. The function
  //  names are interned upon load, but never released
  LLVMOrcSymbolStringPoolEntryRef Name =
      LLVMOrcExecutionSessionIntern(ES, "test");
  LLVMOrcReleaseSymbolStringPoolEntry(Name);
  LLVMOrcReleaseSymbolStringPoolEntry(Name);
  LLVMOrcDisposeLLJIT(Jit);
}

TEST_F(OrcCAPITestBase, ExecutionTest) {
  if (!SupportsJIT)
    return;

  // This test performs OrcJIT compilation of a simple sum module
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  const char *Triple = LLVMGetDefaultTargetTriple();
  char *Msg = 0;
  LLVMTargetRef Target;
  if (LLVMGetTargetFromTriple(Triple, &Target, &Msg)) {
    ADD_FAILURE() << "Failed to retrieve target from triple " << Triple << ": "
                  << Msg;
    return;
  }
  LLVMTargetMachineRef TM = LLVMCreateTargetMachine(
      Target, Triple, "generic", "", LLVMCodeGenLevelNone, LLVMRelocDefault,
      LLVMCodeModelDefault);
  LLVMOrcJITTargetMachineBuilderRef TMB =
      LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(TM);
  if (LLVMErrorRef E = LLVMOrcJITTargetMachineBuilderDetectHost(&TMB)) {
    reportError(E, "Failed to detect host target from TargetMachineBuilder");
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
    LLVMDisposeBuilder(IRBuilder);
  }
  LLVMOrcThreadSafeModuleRef TSM = LLVMOrcCreateNewThreadSafeModule(Mod, TSC);
  LLVMOrcJITDylibRef MainDylib = LLVMOrcLLJITGetMainJITDylib(Jit);
  if (LLVMErrorRef E = LLVMOrcLLJITAddLLVMIRModule(Jit, MainDylib, TSM)) {
    reportError(E, "Failed to add LLVM IR module to LLJIT");
    LLVMOrcDisposeLLJIT(Jit);
    return;
  }
  LLVMOrcJITTargetAddress TestFnAddr;
  if (LLVMErrorRef E = LLVMOrcLLJITLookup(Jit, &TestFnAddr, "sum")) {
    reportError(E, "Failed to locate \"sum\" symbol");
    LLVMOrcDisposeLLJIT(Jit);
    return;
  }
  auto *SumFn = (int32_t(*)(int32_t, int32_t))TestFnAddr;
  int32_t Result = SumFn(1, 1);
  ASSERT_EQ(2, Result);
  LLVMOrcDisposeLLJIT(Jit);
}