//===-- WebAssemblyFrameLowering.cpp - WebAssembly Frame Lowering ----------==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file contains the WebAssembly implementation of
/// TargetFrameLowering class.
///
/// On WebAssembly, there aren't a lot of things to do here. There are no
/// callee-saved registers to save, and no spill slots.
///
/// The stack grows downward.
///
//===----------------------------------------------------------------------===//

#include "WebAssemblyFrameLowering.h"
#include "MCTargetDesc/WebAssemblyMCTargetDesc.h"
#include "WebAssemblyInstrInfo.h"
#include "WebAssemblyMachineFunctionInfo.h"
#include "WebAssemblySubtarget.h"
#include "WebAssemblyTargetMachine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
using namespace llvm;

#define DEBUG_TYPE "wasm-frame-info"

// TODO: wasm64
// TODO: Emit TargetOpcode::CFI_INSTRUCTION instructions

/// We need a base pointer in the case of having items on the stack that
/// require stricter alignment than the stack pointer itself.  Because we need
/// to shift the stack pointer by some unknown amount to force the alignment,
/// we need to record the value of the stack pointer on entry to the function.
bool WebAssemblyFrameLowering::hasBP(
    const MachineFunction &MF) const {
  const auto *RegInfo =
      MF.getSubtarget<WebAssemblySubtarget>().getRegisterInfo();
  return RegInfo->needsStackRealignment(MF);
}

/// Return true if the specified function should have a dedicated frame pointer
/// register.
bool WebAssemblyFrameLowering::hasFP(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();

  // When we have var-sized objects, we move the stack pointer by an unknown
  // amount, and need to emit a frame pointer to restore the stack to where we
  // were on function entry.
  // If we already need a base pointer, we use that to fix up the stack pointer.
  // If there are no fixed-size objects, we would have no use of a frame
  // pointer, and thus should not emit one.
  bool HasFixedSizedObjects = MFI.getStackSize() > 0;
  bool NeedsFixedReference = !hasBP(MF) || HasFixedSizedObjects;

  return MFI.isFrameAddressTaken() ||
         (MFI.hasVarSizedObjects() && NeedsFixedReference) ||
         MFI.hasStackMap() || MFI.hasPatchPoint();
}

/// Under normal circumstances, when a frame pointer is not required, we reserve
/// argument space for call sites in the function immediately on entry to the
/// current function. This eliminates the need for add/sub sp brackets around
/// call sites. Returns true if the call frame is included as part of the stack
/// frame.
bool WebAssemblyFrameLowering::hasReservedCallFrame(
    const MachineFunction &MF) const {
  return !MF.getFrameInfo().hasVarSizedObjects();
}


/// Returns true if this function needs a local user-space stack pointer.
/// Unlike a machine stack pointer, the wasm user stack pointer is a global
/// variable, so it is loaded into a register in the prolog.
bool WebAssemblyFrameLowering::needsSP(const MachineFunction &MF,
                                       const MachineFrameInfo &MFI) const {
  return MFI.getStackSize() || MFI.adjustsStack() || hasFP(MF);
}

/// Returns true if the local user-space stack pointer needs to be written back
/// to memory by this function (this is not meaningful if needsSP is false). If
/// false, the stack red zone can be used and only a local SP is needed.
bool WebAssemblyFrameLowering::needsSPWriteback(
    const MachineFunction &MF, const MachineFrameInfo &MFI) const {
  assert(needsSP(MF, MFI));
  return MFI.getStackSize() > RedZoneSize || MFI.hasCalls() ||
         MF.getFunction()->hasFnAttribute(Attribute::NoRedZone);
}

static void writeSPToMemory(unsigned SrcReg, MachineFunction &MF,
                            MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator &InsertAddr,
                            MachineBasicBlock::iterator &InsertStore,
                            const DebugLoc &DL) {
  const char *ES = "__stack_pointer";
  auto *SPSymbol = MF.createExternalSymbolName(ES);
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterClass *PtrRC =
      MRI.getTargetRegisterInfo()->getPointerRegClass(MF);
  unsigned Zero = MRI.createVirtualRegister(PtrRC);
  const auto *TII = MF.getSubtarget<WebAssemblySubtarget>().getInstrInfo();

  BuildMI(MBB, InsertAddr, DL, TII->get(WebAssembly::CONST_I32), Zero)
      .addImm(0);
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo(MF.getPSVManager().getExternalSymbolCallEntry(ES)),
      MachineMemOperand::MOStore, 4, 4);
  BuildMI(MBB, InsertStore, DL, TII->get(WebAssembly::STORE_I32))
      .addImm(2)  // p2align
      .addExternalSymbol(SPSymbol)
      .addReg(Zero)
      .addReg(SrcReg)
      .addMemOperand(MMO);
}

MachineBasicBlock::iterator
WebAssemblyFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  assert(!I->getOperand(0).getImm() && (hasFP(MF) || hasBP(MF)) &&
         "Call frame pseudos should only be used for dynamic stack adjustment");
  const auto *TII = MF.getSubtarget<WebAssemblySubtarget>().getInstrInfo();
  if (I->getOpcode() == TII->getCallFrameDestroyOpcode() &&
      needsSPWriteback(MF, MF.getFrameInfo())) {
    DebugLoc DL = I->getDebugLoc();
    writeSPToMemory(WebAssembly::SP32, MF, MBB, I, I, DL);
  }
  return MBB.erase(I);
}

void WebAssemblyFrameLowering::emitPrologue(MachineFunction &MF,
                                            MachineBasicBlock &MBB) const {
  // TODO: Do ".setMIFlag(MachineInstr::FrameSetup)" on emitted instructions
  auto &MFI = MF.getFrameInfo();
  assert(MFI.getCalleeSavedInfo().empty() &&
         "WebAssembly should not have callee-saved registers");

  if (!needsSP(MF, MFI)) return;
  uint64_t StackSize = MFI.getStackSize();

  const auto *TII = MF.getSubtarget<WebAssemblySubtarget>().getInstrInfo();
  auto &MRI = MF.getRegInfo();

  auto InsertPt = MBB.begin();
  DebugLoc DL;

  const TargetRegisterClass *PtrRC =
      MRI.getTargetRegisterInfo()->getPointerRegClass(MF);
  unsigned Zero = MRI.createVirtualRegister(PtrRC);
  unsigned SPReg = WebAssembly::SP32;
  if (StackSize)
    SPReg = MRI.createVirtualRegister(PtrRC);
  const char *ES = "__stack_pointer";
  auto *SPSymbol = MF.createExternalSymbolName(ES);
  BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::CONST_I32), Zero)
      .addImm(0);
  MachineMemOperand *LoadMMO = MF.getMachineMemOperand(
      MachinePointerInfo(MF.getPSVManager().getExternalSymbolCallEntry(ES)),
      MachineMemOperand::MOLoad, 4, 4);
  // Load the SP value.
  BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::LOAD_I32), SPReg)
      .addImm(2)       // p2align
      .addExternalSymbol(SPSymbol)
      .addReg(Zero)    // addr
      .addMemOperand(LoadMMO);

  bool HasBP = hasBP(MF);
  if (HasBP) {
    auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
    unsigned BasePtr = MRI.createVirtualRegister(PtrRC);
    FI->setBasePointerVreg(BasePtr);
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), BasePtr)
        .addReg(SPReg);
  }
  if (StackSize) {
    // Subtract the frame size
    unsigned OffsetReg = MRI.createVirtualRegister(PtrRC);
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::CONST_I32), OffsetReg)
        .addImm(StackSize);
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::SUB_I32),
            WebAssembly::SP32)
        .addReg(SPReg)
        .addReg(OffsetReg);
  }
  if (HasBP) {
    unsigned BitmaskReg = MRI.createVirtualRegister(PtrRC);
    unsigned Alignment = MFI.getMaxAlignment();
    assert((1 << countTrailingZeros(Alignment)) == Alignment &&
      "Alignment must be a power of 2");
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::CONST_I32), BitmaskReg)
        .addImm((int)~(Alignment - 1));
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::AND_I32),
            WebAssembly::SP32)
        .addReg(WebAssembly::SP32)
        .addReg(BitmaskReg);
  }
  if (hasFP(MF)) {
    // Unlike most conventional targets (where FP points to the saved FP),
    // FP points to the bottom of the fixed-size locals, so we can use positive
    // offsets in load/store instructions.
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY),
            WebAssembly::FP32)
        .addReg(WebAssembly::SP32);
  }
  if (StackSize && needsSPWriteback(MF, MFI)) {
    writeSPToMemory(WebAssembly::SP32, MF, MBB, InsertPt, InsertPt, DL);
  }
}

void WebAssemblyFrameLowering::emitEpilogue(MachineFunction &MF,
                                            MachineBasicBlock &MBB) const {
  auto &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (!needsSP(MF, MFI) || !needsSPWriteback(MF, MFI)) return;
  const auto *TII = MF.getSubtarget<WebAssemblySubtarget>().getInstrInfo();
  auto &MRI = MF.getRegInfo();
  auto InsertPt = MBB.getFirstTerminator();
  DebugLoc DL;

  if (InsertPt != MBB.end())
    DL = InsertPt->getDebugLoc();

  // Restore the stack pointer. If we had fixed-size locals, add the offset
  // subtracted in the prolog.
  unsigned SPReg = 0;
  MachineBasicBlock::iterator InsertAddr = InsertPt;
  if (hasBP(MF)) {
    auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
    SPReg = FI->getBasePointerVreg();
  } else if (StackSize) {
    const TargetRegisterClass *PtrRC =
        MRI.getTargetRegisterInfo()->getPointerRegClass(MF);
    unsigned OffsetReg = MRI.createVirtualRegister(PtrRC);
    InsertAddr =
        BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::CONST_I32), OffsetReg)
            .addImm(StackSize);
    // In the epilog we don't need to write the result back to the SP32 physreg
    // because it won't be used again. We can use a stackified register instead.
    SPReg = MRI.createVirtualRegister(PtrRC);
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::ADD_I32), SPReg)
        .addReg(hasFP(MF) ? WebAssembly::FP32 : WebAssembly::SP32)
        .addReg(OffsetReg);
  } else {
    SPReg = hasFP(MF) ? WebAssembly::FP32 : WebAssembly::SP32;
  }

  writeSPToMemory(SPReg, MF, MBB, InsertAddr, InsertPt, DL);
}
