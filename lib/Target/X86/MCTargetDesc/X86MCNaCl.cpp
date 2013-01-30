//=== X86MCNaCl.cpp - Expansion of NaCl pseudo-instructions      --*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "x86-sandboxing"

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCNaCl.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCContext.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

// This option makes it possible to overwrite the x86 jmp mask immediate.
// Setting it to -1 will effectively turn masking into a nop which will
// help with linking this code with non-sandboxed libs (at least for x86-32).
cl::opt<int> FlagSfiX86JmpMask("sfi-x86-jmp-mask", cl::init(-32));

cl::opt<bool> FlagUseZeroBasedSandbox("sfi-zero-based-sandbox",
                                      cl::desc("Use a zero-based sandbox model"
                                               " for the NaCl SFI."),
                                      cl::init(false));

static unsigned PrefixSaved = 0;
static bool PrefixPass = false;

// See the notes below where these functions are defined.
namespace {
unsigned getX86SubSuperRegister_(unsigned Reg, EVT VT, bool High=false);
unsigned DemoteRegTo32_(unsigned RegIn);
} // namespace

static void EmitDirectCall(const MCOperand &Op, bool Is64Bit,
                           MCStreamer &Out) {
  Out.EmitBundleLock(true);

  MCInst CALLInst;
  CALLInst.setOpcode(Is64Bit ? X86::CALL64pcrel32 : X86::CALLpcrel32);
  CALLInst.addOperand(Op);
  Out.EmitInstruction(CALLInst);
  Out.EmitBundleUnlock();
}

static void EmitIndirectBranch(const MCOperand &Op, bool Is64Bit, bool IsCall,
                               MCStreamer &Out) {
  const bool UseZeroBasedSandbox = FlagUseZeroBasedSandbox;
  const int JmpMask = FlagSfiX86JmpMask;
  const unsigned Reg32 = Op.getReg();
  const unsigned Reg64 = getX86SubSuperRegister_(Reg32, MVT::i64);

  Out.EmitBundleLock(IsCall);

  MCInst ANDInst;
  ANDInst.setOpcode(X86::AND32ri8);
  ANDInst.addOperand(MCOperand::CreateReg(Reg32));
  ANDInst.addOperand(MCOperand::CreateReg(Reg32));
  ANDInst.addOperand(MCOperand::CreateImm(JmpMask));
  Out.EmitInstruction(ANDInst);

  if (Is64Bit && !UseZeroBasedSandbox) {
    MCInst InstADD;
    InstADD.setOpcode(X86::ADD64rr);
    InstADD.addOperand(MCOperand::CreateReg(Reg64));
    InstADD.addOperand(MCOperand::CreateReg(Reg64));
    InstADD.addOperand(MCOperand::CreateReg(X86::R15));
    Out.EmitInstruction(InstADD);
  }

  if (IsCall) {
    MCInst CALLInst;
    CALLInst.setOpcode(Is64Bit ? X86::CALL64r : X86::CALL32r);
    CALLInst.addOperand(MCOperand::CreateReg(Is64Bit ? Reg64 : Reg32));
    Out.EmitInstruction(CALLInst);
  } else {
    MCInst JMPInst;
    JMPInst.setOpcode(Is64Bit ? X86::JMP64r : X86::JMP32r);
    JMPInst.addOperand(MCOperand::CreateReg(Is64Bit ? Reg64 : Reg32));
    Out.EmitInstruction(JMPInst);
  }
  Out.EmitBundleUnlock();
}

static void EmitRet(const MCOperand *AmtOp, bool Is64Bit, MCStreamer &Out) {
  MCInst POPInst;
  POPInst.setOpcode(Is64Bit ? X86::POP64r : X86::POP32r);
  POPInst.addOperand(MCOperand::CreateReg(Is64Bit ? X86::RCX : X86::ECX));
  Out.EmitInstruction(POPInst);

  if (AmtOp) {
    assert(!Is64Bit);
    MCInst ADDInst;
    unsigned ADDReg = X86::ESP;
    ADDInst.setOpcode(X86::ADD32ri);
    ADDInst.addOperand(MCOperand::CreateReg(ADDReg));
    ADDInst.addOperand(MCOperand::CreateReg(ADDReg));
    ADDInst.addOperand(*AmtOp);
    Out.EmitInstruction(ADDInst);
  }

  MCInst JMPInst;
  JMPInst.setOpcode(Is64Bit ? X86::NACL_JMP64r : X86::NACL_JMP32r);
  JMPInst.addOperand(MCOperand::CreateReg(X86::ECX));
  Out.EmitInstruction(JMPInst);
}

static void EmitTrap(bool Is64Bit, MCStreamer &Out) {
  // Rewrite to:
  //    X86-32:  mov $0, 0
  //    X86-64:  mov $0, (%r15)
  const bool UseZeroBasedSandbox = FlagUseZeroBasedSandbox;
  unsigned BaseReg = Is64Bit && !UseZeroBasedSandbox ? X86::R15 : 0;

  MCInst Tmp;
  Tmp.setOpcode(X86::MOV32mi);
  Tmp.addOperand(MCOperand::CreateReg(BaseReg)); // BaseReg
  Tmp.addOperand(MCOperand::CreateImm(1)); // Scale
  Tmp.addOperand(MCOperand::CreateReg(0)); // IndexReg
  Tmp.addOperand(MCOperand::CreateImm(0)); // Offset
  Tmp.addOperand(MCOperand::CreateReg(0)); // SegmentReg
  Tmp.addOperand(MCOperand::CreateImm(0)); // Value

  Out.EmitInstruction(Tmp);
}

// Fix a register after being truncated to 32-bits.
static void EmitRegFix(unsigned Reg64, MCStreamer &Out) {
  // lea (%rsp, %r15, 1), %rsp
  // We do not need to add the R15 base for the zero-based sandbox model
  const bool UseZeroBasedSandbox = FlagUseZeroBasedSandbox;
  if (!UseZeroBasedSandbox) {
    MCInst Tmp;
    Tmp.setOpcode(X86::LEA64r);
    Tmp.addOperand(MCOperand::CreateReg(Reg64));    // DestReg
    Tmp.addOperand(MCOperand::CreateReg(Reg64));    // BaseReg
    Tmp.addOperand(MCOperand::CreateImm(1));        // Scale
    Tmp.addOperand(MCOperand::CreateReg(X86::R15)); // IndexReg
    Tmp.addOperand(MCOperand::CreateImm(0));        // Offset
    Tmp.addOperand(MCOperand::CreateReg(0));        // SegmentReg
    Out.EmitInstruction(Tmp);
  }
}

static void EmitSPArith(unsigned Opc, const MCOperand &ImmOp,
                        MCStreamer &Out) {
  Out.EmitBundleLock(false);

  MCInst Tmp;
  Tmp.setOpcode(Opc);
  Tmp.addOperand(MCOperand::CreateReg(X86::RSP));
  Tmp.addOperand(MCOperand::CreateReg(X86::RSP));
  Tmp.addOperand(ImmOp);
  Out.EmitInstruction(Tmp);

  EmitRegFix(X86::RSP, Out);
  Out.EmitBundleUnlock();
}

static void EmitSPAdj(const MCOperand &ImmOp, MCStreamer &Out) {
  Out.EmitBundleLock(false);

  MCInst Tmp;
  Tmp.setOpcode(X86::LEA64_32r);
  Tmp.addOperand(MCOperand::CreateReg(X86::RSP)); // DestReg
  Tmp.addOperand(MCOperand::CreateReg(X86::RBP)); // BaseReg
  Tmp.addOperand(MCOperand::CreateImm(1));        // Scale
  Tmp.addOperand(MCOperand::CreateReg(0));        // IndexReg
  Tmp.addOperand(ImmOp);                          // Offset
  Tmp.addOperand(MCOperand::CreateReg(0));        // SegmentReg
  Out.EmitInstruction(Tmp);

  EmitRegFix(X86::RSP, Out);
  Out.EmitBundleUnlock();
}

static void EmitPrefix(unsigned Opc, MCStreamer &Out) {
  assert(PrefixSaved == 0);
  assert(PrefixPass == false);

  MCInst PrefixInst;
  PrefixInst.setOpcode(Opc);
  PrefixPass = true;
  Out.EmitInstruction(PrefixInst);

  assert(PrefixSaved == 0);
  assert(PrefixPass == false);
}

static void EmitMoveRegReg(bool Is64Bit, unsigned ToReg,
                           unsigned FromReg, MCStreamer &Out) {
  MCInst Move;
  Move.setOpcode(Is64Bit ? X86::MOV64rr : X86::MOV32rr);
  Move.addOperand(MCOperand::CreateReg(ToReg));
  Move.addOperand(MCOperand::CreateReg(FromReg));
  Out.EmitInstruction(Move);
}

static void EmitRegTruncate(unsigned Reg64, MCStreamer &Out) {
  unsigned Reg32 = getX86SubSuperRegister_(Reg64, MVT::i32);
  EmitMoveRegReg(false, Reg32, Reg32, Out);
}

static void HandleMemoryRefTruncation(MCInst *Inst, unsigned IndexOpPosition,
                                      MCStreamer &Out) {
  const bool UseZeroBasedSandbox = FlagUseZeroBasedSandbox;
  unsigned IndexReg = Inst->getOperand(IndexOpPosition).getReg();
  if (UseZeroBasedSandbox) {
    // With the zero-based sandbox, we use a 32-bit register on the index
    Inst->getOperand(IndexOpPosition).setReg(DemoteRegTo32_(IndexReg));
  } else {
    EmitRegTruncate(IndexReg, Out);
  }
}

static void ShortenMemoryRef(MCInst *Inst, unsigned IndexOpPosition) {
  unsigned ImmOpPosition = IndexOpPosition - 1;
  unsigned BaseOpPosition = IndexOpPosition - 2;
  unsigned IndexReg = Inst->getOperand(IndexOpPosition).getReg();
  // For the SIB byte, if the scale is 1 and the base is 0, then
  // an equivalent setup moves index to base, and index to 0.  The
  // equivalent setup is optimized to remove the SIB byte in
  // X86MCCodeEmitter.cpp.
  if (Inst->getOperand(ImmOpPosition).getImm() == 1 &&
      Inst->getOperand(BaseOpPosition).getReg() == 0) {
    Inst->getOperand(BaseOpPosition).setReg(IndexReg);
    Inst->getOperand(IndexOpPosition).setReg(0);
  }
}

static void EmitLoad(bool Is64Bit,
                     unsigned DestReg,
                     unsigned BaseReg,
                     unsigned Scale,
                     unsigned IndexReg,
                     unsigned Offset,
                     unsigned SegmentReg,
                     MCStreamer &Out) {
  // Load DestReg from address BaseReg + Scale * IndexReg + Offset
  MCInst Load;
  Load.setOpcode(Is64Bit ? X86::MOV64rm : X86::MOV32rm);
  Load.addOperand(MCOperand::CreateReg(DestReg));
  Load.addOperand(MCOperand::CreateReg(BaseReg));
  Load.addOperand(MCOperand::CreateImm(Scale));
  Load.addOperand(MCOperand::CreateReg(IndexReg));
  Load.addOperand(MCOperand::CreateImm(Offset));
  Load.addOperand(MCOperand::CreateReg(SegmentReg));
  Out.EmitInstruction(Load);
}

static bool SandboxMemoryRef(MCInst *Inst,
                             unsigned *IndexOpPosition) {
  for (unsigned i = 0, last = Inst->getNumOperands(); i < last; i++) {
    if (!Inst->getOperand(i).isReg() ||
        Inst->getOperand(i).getReg() != X86::PSEUDO_NACL_SEG) {
      continue;
    }
    // Return the index register that will need to be truncated.
    // The order of operands on a memory reference is always:
    // (BaseReg, ScaleImm, IndexReg, DisplacementImm, SegmentReg),
    // So if we found a match for a segment register value, we know that
    // the index register is exactly two operands prior.
    *IndexOpPosition = i - 2;

    // Remove the PSEUDO_NACL_SEG annotation.
    Inst->getOperand(i).setReg(0);
    return true;
  }
  return false;
}

static void EmitTLSAddr32(const MCInst &Inst, MCStreamer &Out) {
  Out.EmitBundleLock(true);

  MCInst LeaInst;
  LeaInst.setOpcode(X86::LEA32r);
  LeaInst.addOperand(MCOperand::CreateReg(X86::EAX));    // DestReg
  LeaInst.addOperand(Inst.getOperand(0)); // BaseReg
  LeaInst.addOperand(Inst.getOperand(1)); // Scale
  LeaInst.addOperand(Inst.getOperand(2)); // IndexReg
  LeaInst.addOperand(Inst.getOperand(3)); // Offset
  LeaInst.addOperand(Inst.getOperand(4)); // SegmentReg
  Out.EmitInstruction(LeaInst);

  MCInst CALLInst;
  CALLInst.setOpcode(X86::CALLpcrel32);
  MCContext &context = Out.getContext();
  const MCSymbolRefExpr *expr =
    MCSymbolRefExpr::Create(
      context.GetOrCreateSymbol(StringRef("___tls_get_addr")),
      MCSymbolRefExpr::VK_PLT, context);
  CALLInst.addOperand(MCOperand::CreateExpr(expr));
  Out.EmitInstruction(CALLInst);
  Out.EmitBundleUnlock();
}


static void EmitREST(const MCInst &Inst, unsigned Reg32,
                     bool IsMem, MCStreamer &Out) {
  unsigned Reg64 = getX86SubSuperRegister_(Reg32, MVT::i64);
  Out.EmitBundleLock(false);
  if (!IsMem) {
    EmitMoveRegReg(false, Reg32, Inst.getOperand(0).getReg(), Out);
  } else {
    unsigned IndexOpPosition;
    MCInst SandboxedInst = Inst;
    if (SandboxMemoryRef(&SandboxedInst, &IndexOpPosition)) {
      HandleMemoryRefTruncation(&SandboxedInst, IndexOpPosition, Out);
      ShortenMemoryRef(&SandboxedInst, IndexOpPosition);
    }
    EmitLoad(false,
             Reg32,
             SandboxedInst.getOperand(0).getReg(),  // BaseReg
             SandboxedInst.getOperand(1).getImm(),  // Scale
             SandboxedInst.getOperand(2).getReg(),  // IndexReg
             SandboxedInst.getOperand(3).getImm(),  // Offset
             SandboxedInst.getOperand(4).getReg(),  // SegmentReg
             Out);
  }

  EmitRegFix(Reg64, Out);
  Out.EmitBundleUnlock();
}


namespace llvm {
// CustomExpandInstNaClX86 -
//   If Inst is a NaCl pseudo instruction, emits the substitute
//   expansion to the MCStreamer and returns true.
//   Otherwise, returns false.
//
//   NOTE: Each time this function calls Out.EmitInstruction(), it will be
//   called again recursively to rewrite the new instruction being emitted.
//   Care must be taken to ensure that this does not result in an infinite
//   loop. Also, global state must be managed carefully so that it is
//   consistent during recursive calls.
//
//   We need global state to keep track of the explicit prefix (PREFIX_*)
//   instructions. Unfortunately, the assembly parser prefers to generate
//   these instead of combined instructions. At this time, having only
//   one explicit prefix is supported.
bool CustomExpandInstNaClX86(const MCInst &Inst, MCStreamer &Out) {
  const bool UseZeroBasedSandbox = FlagUseZeroBasedSandbox;
  // If we are emitting to .s, just emit all pseudo-instructions directly.
  if (Out.hasRawTextSupport()) {
    return false;
  }
  unsigned Opc = Inst.getOpcode();
  DEBUG(dbgs() << "CustomExpandInstNaClX86("; Inst.dump(); dbgs() << ")\n");
  switch (Opc) {
  case X86::LOCK_PREFIX:
  case X86::REP_PREFIX:
  case X86::REPNE_PREFIX:
  case X86::REX64_PREFIX:
    // Ugly hack because LLVM AsmParser is not smart enough to combine
    // prefixes back into the instruction they modify.
    if (PrefixPass) {
      PrefixPass = false;
      PrefixSaved = 0;
      return false;
    }
    assert(PrefixSaved == 0);
    PrefixSaved = Opc;
    return true;
  case X86::NACL_TRAP32:
    assert(PrefixSaved == 0);
    EmitTrap(false, Out);
    return true;
  case X86::NACL_TRAP64:
    assert(PrefixSaved == 0);
    EmitTrap(true, Out);
    return true;
  case X86::NACL_CALL32d:
    assert(PrefixSaved == 0);
    EmitDirectCall(Inst.getOperand(0), false, Out);
    return true;
  case X86::NACL_CALL64d:
    assert(PrefixSaved == 0);
    EmitDirectCall(Inst.getOperand(0), true, Out);
    return true;
  case X86::NACL_CALL32r:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), false, true, Out);
    return true;
  case X86::NACL_CALL64r:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), true, true, Out);
    return true;
  case X86::NACL_JMP32r:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), false, false, Out);
    return true;
  case X86::NACL_TLS_addr32:
    assert(PrefixSaved == 0);
    EmitTLSAddr32(Inst, Out);
    return true;
  case X86::NACL_JMP64r:
  case X86::NACL_JMP64z:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), true, false, Out);
    return true;
  case X86::NACL_RET32:
    assert(PrefixSaved == 0);
    EmitRet(NULL, false, Out);
    return true;
  case X86::NACL_RET64:
    assert(PrefixSaved == 0);
    EmitRet(NULL, true, Out);
    return true;
  case X86::NACL_RETI32:
    assert(PrefixSaved == 0);
    EmitRet(&Inst.getOperand(0), false, Out);
    return true;
  case X86::NACL_ASPi8:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::ADD32ri8, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_ASPi32:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::ADD32ri, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_SSPi8:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::SUB32ri8, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_SSPi32:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::SUB32ri, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_ANDSPi32:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::AND32ri, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_SPADJi32:
    assert(PrefixSaved == 0);
    EmitSPAdj(Inst.getOperand(0), Out);
    return true;
  case X86::NACL_RESTBPm:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::EBP, true, Out);
    return true;
  case X86::NACL_RESTBPr:
  case X86::NACL_RESTBPrz:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::EBP, false, Out);
    return true;
  case X86::NACL_RESTSPm:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::ESP, true, Out);
    return true;
  case X86::NACL_RESTSPr:
  case X86::NACL_RESTSPrz:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::ESP, false, Out);
    return true;
  }

  unsigned IndexOpPosition;
  MCInst SandboxedInst = Inst;
  if (SandboxMemoryRef(&SandboxedInst, &IndexOpPosition)) {
    unsigned PrefixLocal = PrefixSaved;
    PrefixSaved = 0;

    if (PrefixLocal || !UseZeroBasedSandbox)
      Out.EmitBundleLock(false);

    HandleMemoryRefTruncation(&SandboxedInst, IndexOpPosition, Out);
    ShortenMemoryRef(&SandboxedInst, IndexOpPosition);

    if (PrefixLocal)
      EmitPrefix(PrefixLocal, Out);
    Out.EmitInstruction(SandboxedInst);

    if (PrefixLocal || !UseZeroBasedSandbox)
      Out.EmitBundleUnlock();
    return true;
  }

  if (PrefixSaved) {
    unsigned PrefixLocal = PrefixSaved;
    PrefixSaved = 0;
    EmitPrefix(PrefixLocal, Out);
  }
  return false;
}

} // namespace llvm




// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
//
// This is an exact copy of getX86SubSuperRegister from X86RegisterInfo.h
// We cannot use the original because it is part of libLLVMX86CodeGen,
// which cannot be a dependency of this module (libLLVMX86Desc).
//
// However, in all likelyhood, the real getX86SubSuperRegister will
// eventually be moved to MCTargetDesc, and then this copy can be
// removed.

namespace {
unsigned getX86SubSuperRegister_(unsigned Reg, EVT VT, bool High) {
  switch (VT.getSimpleVT().SimpleTy) {
  default: return Reg;
  case MVT::i8:
    if (High) {
      switch (Reg) {
      default: return 0;
      case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
        return X86::AH;
      case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
        return X86::DH;
      case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
        return X86::CH;
      case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
        return X86::BH;
      }
    } else {
      switch (Reg) {
      default: return 0;
      case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
        return X86::AL;
      case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
        return X86::DL;
      case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
        return X86::CL;
      case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
        return X86::BL;
      case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
        return X86::SIL;
      case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
        return X86::DIL;
      case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
        return X86::BPL;
      case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
        return X86::SPL;
      case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
        return X86::R8B;
      case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
        return X86::R9B;
      case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
        return X86::R10B;
      case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
        return X86::R11B;
      case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
        return X86::R12B;
      case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
        return X86::R13B;
      case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
        return X86::R14B;
      case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
        return X86::R15B;
      }
    }
  case MVT::i16:
    switch (Reg) {
    default: return Reg;
    case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
      return X86::AX;
    case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
      return X86::DX;
    case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
      return X86::CX;
    case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
      return X86::BX;
    case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
      return X86::SI;
    case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
      return X86::DI;
    case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
      return X86::BP;
    case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
      return X86::SP;
    case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
      return X86::R8W;
    case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
      return X86::R9W;
    case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
      return X86::R10W;
    case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
      return X86::R11W;
    case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
      return X86::R12W;
    case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
      return X86::R13W;
    case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
      return X86::R14W;
    case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
      return X86::R15W;
    }
  case MVT::i32:
    switch (Reg) {
    default: return Reg;
    case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
      return X86::EAX;
    case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
      return X86::EDX;
    case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
      return X86::ECX;
    case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
      return X86::EBX;
    case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
      return X86::ESI;
    case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
      return X86::EDI;
    case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
      return X86::EBP;
    case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
      return X86::ESP;
    case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
      return X86::R8D;
    case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
      return X86::R9D;
    case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
      return X86::R10D;
    case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
      return X86::R11D;
    case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
      return X86::R12D;
    case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
      return X86::R13D;
    case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
      return X86::R14D;
    case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
      return X86::R15D;
    }
  case MVT::i64:
    switch (Reg) {
    default: return Reg;
    case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
      return X86::RAX;
    case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
      return X86::RDX;
    case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
      return X86::RCX;
    case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
      return X86::RBX;
    case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
      return X86::RSI;
    case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
      return X86::RDI;
    case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
      return X86::RBP;
    case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
      return X86::RSP;
    case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
      return X86::R8;
    case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
      return X86::R9;
    case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
      return X86::R10;
    case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
      return X86::R11;
    case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
      return X86::R12;
    case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
      return X86::R13;
    case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
      return X86::R14;
    case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
      return X86::R15;
    }
  }

  return Reg;
}

// This is a copy of DemoteRegTo32 from X86NaClRewritePass.cpp.
// We cannot use the original because it uses part of libLLVMX86CodeGen,
// which cannot be a dependency of this module (libLLVMX86Desc).
// Note that this function calls getX86SubSuperRegister_, which is
// also a copied function for the same reason.

unsigned DemoteRegTo32_(unsigned RegIn) {
  if (RegIn == 0)
    return 0;
  unsigned RegOut = getX86SubSuperRegister_(RegIn, MVT::i32, false);
  assert(RegOut != 0);
  return RegOut;
}
} //namespace
// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
