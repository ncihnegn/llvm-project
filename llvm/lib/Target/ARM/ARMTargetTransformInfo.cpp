//===- ARMTargetTransformInfo.cpp - ARM specific TTI ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ARMTargetTransformInfo.h"
#include "ARMSubtarget.h"
#include "MCTargetDesc/ARMAddressingModes.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/CodeGen/CostTable.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MachineValueType.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "armtti"

static cl::opt<bool> DisableLowOverheadLoops(
  "disable-arm-loloops", cl::Hidden, cl::init(false),
  cl::desc("Disable the generation of low-overhead loops"));

bool ARMTTIImpl::areInlineCompatible(const Function *Caller,
                                     const Function *Callee) const {
  const TargetMachine &TM = getTLI()->getTargetMachine();
  const FeatureBitset &CallerBits =
      TM.getSubtargetImpl(*Caller)->getFeatureBits();
  const FeatureBitset &CalleeBits =
      TM.getSubtargetImpl(*Callee)->getFeatureBits();

  // To inline a callee, all features not in the whitelist must match exactly.
  bool MatchExact = (CallerBits & ~InlineFeatureWhitelist) ==
                    (CalleeBits & ~InlineFeatureWhitelist);
  // For features in the whitelist, the callee's features must be a subset of
  // the callers'.
  bool MatchSubset = ((CallerBits & CalleeBits) & InlineFeatureWhitelist) ==
                     (CalleeBits & InlineFeatureWhitelist);
  return MatchExact && MatchSubset;
}

int ARMTTIImpl::getIntImmCost(const APInt &Imm, Type *Ty) {
  assert(Ty->isIntegerTy());

 unsigned Bits = Ty->getPrimitiveSizeInBits();
 if (Bits == 0 || Imm.getActiveBits() >= 64)
   return 4;

  int64_t SImmVal = Imm.getSExtValue();
  uint64_t ZImmVal = Imm.getZExtValue();
  if (!ST->isThumb()) {
    if ((SImmVal >= 0 && SImmVal < 65536) ||
        (ARM_AM::getSOImmVal(ZImmVal) != -1) ||
        (ARM_AM::getSOImmVal(~ZImmVal) != -1))
      return 1;
    return ST->hasV6T2Ops() ? 2 : 3;
  }
  if (ST->isThumb2()) {
    if ((SImmVal >= 0 && SImmVal < 65536) ||
        (ARM_AM::getT2SOImmVal(ZImmVal) != -1) ||
        (ARM_AM::getT2SOImmVal(~ZImmVal) != -1))
      return 1;
    return ST->hasV6T2Ops() ? 2 : 3;
  }
  // Thumb1, any i8 imm cost 1.
  if (Bits == 8 || (SImmVal >= 0 && SImmVal < 256))
    return 1;
  if ((~SImmVal < 256) || ARM_AM::isThumbImmShiftedVal(ZImmVal))
    return 2;
  // Load from constantpool.
  return 3;
}

// Constants smaller than 256 fit in the immediate field of
// Thumb1 instructions so we return a zero cost and 1 otherwise.
int ARMTTIImpl::getIntImmCodeSizeCost(unsigned Opcode, unsigned Idx,
                                      const APInt &Imm, Type *Ty) {
  if (Imm.isNonNegative() && Imm.getLimitedValue() < 256)
    return 0;

  return 1;
}

int ARMTTIImpl::getIntImmCost(unsigned Opcode, unsigned Idx, const APInt &Imm,
                              Type *Ty) {
  // Division by a constant can be turned into multiplication, but only if we
  // know it's constant. So it's not so much that the immediate is cheap (it's
  // not), but that the alternative is worse.
  // FIXME: this is probably unneeded with GlobalISel.
  if ((Opcode == Instruction::SDiv || Opcode == Instruction::UDiv ||
       Opcode == Instruction::SRem || Opcode == Instruction::URem) &&
      Idx == 1)
    return 0;

  if (Opcode == Instruction::And) {
    // UXTB/UXTH
    if (Imm == 255 || Imm == 65535)
      return 0;
    // Conversion to BIC is free, and means we can use ~Imm instead.
    return std::min(getIntImmCost(Imm, Ty), getIntImmCost(~Imm, Ty));
  }

  if (Opcode == Instruction::Add)
    // Conversion to SUB is free, and means we can use -Imm instead.
    return std::min(getIntImmCost(Imm, Ty), getIntImmCost(-Imm, Ty));

  if (Opcode == Instruction::ICmp && Imm.isNegative() &&
      Ty->getIntegerBitWidth() == 32) {
    int64_t NegImm = -Imm.getSExtValue();
    if (ST->isThumb2() && NegImm < 1<<12)
      // icmp X, #-C -> cmn X, #C
      return 0;
    if (ST->isThumb() && NegImm < 1<<8)
      // icmp X, #-C -> adds X, #C
      return 0;
  }

  // xor a, -1 can always be folded to MVN
  if (Opcode == Instruction::Xor && Imm.isAllOnesValue())
    return 0;

  return getIntImmCost(Imm, Ty);
}

int ARMTTIImpl::getCastInstrCost(unsigned Opcode, Type *Dst, Type *Src,
                                 const Instruction *I) {
  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  // Single to/from double precision conversions.
  static const CostTblEntry NEONFltDblTbl[] = {
    // Vector fptrunc/fpext conversions.
    { ISD::FP_ROUND,   MVT::v2f64, 2 },
    { ISD::FP_EXTEND,  MVT::v2f32, 2 },
    { ISD::FP_EXTEND,  MVT::v4f32, 4 }
  };

  if (Src->isVectorTy() && ST->hasNEON() && (ISD == ISD::FP_ROUND ||
                                          ISD == ISD::FP_EXTEND)) {
    std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Src);
    if (const auto *Entry = CostTableLookup(NEONFltDblTbl, ISD, LT.second))
      return LT.first * Entry->Cost;
  }

  EVT SrcTy = TLI->getValueType(DL, Src);
  EVT DstTy = TLI->getValueType(DL, Dst);

  if (!SrcTy.isSimple() || !DstTy.isSimple())
    return BaseT::getCastInstrCost(Opcode, Dst, Src);

  // The extend of a load is free
  if (I && isa<LoadInst>(I->getOperand(0))) {
    static const TypeConversionCostTblEntry LoadConversionTbl[] = {
        {ISD::SIGN_EXTEND, MVT::i32, MVT::i16, 0},
        {ISD::ZERO_EXTEND, MVT::i32, MVT::i16, 0},
        {ISD::SIGN_EXTEND, MVT::i32, MVT::i8, 0},
        {ISD::ZERO_EXTEND, MVT::i32, MVT::i8, 0},
        {ISD::SIGN_EXTEND, MVT::i16, MVT::i8, 0},
        {ISD::ZERO_EXTEND, MVT::i16, MVT::i8, 0},
        {ISD::SIGN_EXTEND, MVT::i64, MVT::i32, 1},
        {ISD::ZERO_EXTEND, MVT::i64, MVT::i32, 1},
        {ISD::SIGN_EXTEND, MVT::i64, MVT::i16, 1},
        {ISD::ZERO_EXTEND, MVT::i64, MVT::i16, 1},
        {ISD::SIGN_EXTEND, MVT::i64, MVT::i8, 1},
        {ISD::ZERO_EXTEND, MVT::i64, MVT::i8, 1},
    };
    if (const auto *Entry = ConvertCostTableLookup(
            LoadConversionTbl, ISD, DstTy.getSimpleVT(), SrcTy.getSimpleVT()))
      return Entry->Cost;

    static const TypeConversionCostTblEntry MVELoadConversionTbl[] = {
        {ISD::SIGN_EXTEND, MVT::v4i32, MVT::v4i16, 0},
        {ISD::ZERO_EXTEND, MVT::v4i32, MVT::v4i16, 0},
        {ISD::SIGN_EXTEND, MVT::v4i32, MVT::v4i8, 0},
        {ISD::ZERO_EXTEND, MVT::v4i32, MVT::v4i8, 0},
        {ISD::SIGN_EXTEND, MVT::v8i16, MVT::v8i8, 0},
        {ISD::ZERO_EXTEND, MVT::v8i16, MVT::v8i8, 0},
    };
    if (SrcTy.isVector() && ST->hasMVEIntegerOps()) {
      if (const auto *Entry =
              ConvertCostTableLookup(MVELoadConversionTbl, ISD,
                                     DstTy.getSimpleVT(), SrcTy.getSimpleVT()))
        return Entry->Cost;
    }
  }

  // Some arithmetic, load and store operations have specific instructions
  // to cast up/down their types automatically at no extra cost.
  // TODO: Get these tables to know at least what the related operations are.
  static const TypeConversionCostTblEntry NEONVectorConversionTbl[] = {
    { ISD::SIGN_EXTEND, MVT::v4i32, MVT::v4i16, 0 },
    { ISD::ZERO_EXTEND, MVT::v4i32, MVT::v4i16, 0 },
    { ISD::SIGN_EXTEND, MVT::v2i64, MVT::v2i32, 1 },
    { ISD::ZERO_EXTEND, MVT::v2i64, MVT::v2i32, 1 },
    { ISD::TRUNCATE,    MVT::v4i32, MVT::v4i64, 0 },
    { ISD::TRUNCATE,    MVT::v4i16, MVT::v4i32, 1 },

    // The number of vmovl instructions for the extension.
    { ISD::SIGN_EXTEND, MVT::v4i64, MVT::v4i16, 3 },
    { ISD::ZERO_EXTEND, MVT::v4i64, MVT::v4i16, 3 },
    { ISD::SIGN_EXTEND, MVT::v8i32, MVT::v8i8, 3 },
    { ISD::ZERO_EXTEND, MVT::v8i32, MVT::v8i8, 3 },
    { ISD::SIGN_EXTEND, MVT::v8i64, MVT::v8i8, 7 },
    { ISD::ZERO_EXTEND, MVT::v8i64, MVT::v8i8, 7 },
    { ISD::SIGN_EXTEND, MVT::v8i64, MVT::v8i16, 6 },
    { ISD::ZERO_EXTEND, MVT::v8i64, MVT::v8i16, 6 },
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i8, 6 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i8, 6 },

    // Operations that we legalize using splitting.
    { ISD::TRUNCATE,    MVT::v16i8, MVT::v16i32, 6 },
    { ISD::TRUNCATE,    MVT::v8i8, MVT::v8i32, 3 },

    // Vector float <-> i32 conversions.
    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i32, 1 },
    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i32, 1 },

    { ISD::SINT_TO_FP,  MVT::v2f32, MVT::v2i8, 3 },
    { ISD::UINT_TO_FP,  MVT::v2f32, MVT::v2i8, 3 },
    { ISD::SINT_TO_FP,  MVT::v2f32, MVT::v2i16, 2 },
    { ISD::UINT_TO_FP,  MVT::v2f32, MVT::v2i16, 2 },
    { ISD::SINT_TO_FP,  MVT::v2f32, MVT::v2i32, 1 },
    { ISD::UINT_TO_FP,  MVT::v2f32, MVT::v2i32, 1 },
    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i1, 3 },
    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i1, 3 },
    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i8, 3 },
    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i8, 3 },
    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i16, 2 },
    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i16, 2 },
    { ISD::SINT_TO_FP,  MVT::v8f32, MVT::v8i16, 4 },
    { ISD::UINT_TO_FP,  MVT::v8f32, MVT::v8i16, 4 },
    { ISD::SINT_TO_FP,  MVT::v8f32, MVT::v8i32, 2 },
    { ISD::UINT_TO_FP,  MVT::v8f32, MVT::v8i32, 2 },
    { ISD::SINT_TO_FP,  MVT::v16f32, MVT::v16i16, 8 },
    { ISD::UINT_TO_FP,  MVT::v16f32, MVT::v16i16, 8 },
    { ISD::SINT_TO_FP,  MVT::v16f32, MVT::v16i32, 4 },
    { ISD::UINT_TO_FP,  MVT::v16f32, MVT::v16i32, 4 },

    { ISD::FP_TO_SINT,  MVT::v4i32, MVT::v4f32, 1 },
    { ISD::FP_TO_UINT,  MVT::v4i32, MVT::v4f32, 1 },
    { ISD::FP_TO_SINT,  MVT::v4i8, MVT::v4f32, 3 },
    { ISD::FP_TO_UINT,  MVT::v4i8, MVT::v4f32, 3 },
    { ISD::FP_TO_SINT,  MVT::v4i16, MVT::v4f32, 2 },
    { ISD::FP_TO_UINT,  MVT::v4i16, MVT::v4f32, 2 },

    // Vector double <-> i32 conversions.
    { ISD::SINT_TO_FP,  MVT::v2f64, MVT::v2i32, 2 },
    { ISD::UINT_TO_FP,  MVT::v2f64, MVT::v2i32, 2 },

    { ISD::SINT_TO_FP,  MVT::v2f64, MVT::v2i8, 4 },
    { ISD::UINT_TO_FP,  MVT::v2f64, MVT::v2i8, 4 },
    { ISD::SINT_TO_FP,  MVT::v2f64, MVT::v2i16, 3 },
    { ISD::UINT_TO_FP,  MVT::v2f64, MVT::v2i16, 3 },
    { ISD::SINT_TO_FP,  MVT::v2f64, MVT::v2i32, 2 },
    { ISD::UINT_TO_FP,  MVT::v2f64, MVT::v2i32, 2 },

    { ISD::FP_TO_SINT,  MVT::v2i32, MVT::v2f64, 2 },
    { ISD::FP_TO_UINT,  MVT::v2i32, MVT::v2f64, 2 },
    { ISD::FP_TO_SINT,  MVT::v8i16, MVT::v8f32, 4 },
    { ISD::FP_TO_UINT,  MVT::v8i16, MVT::v8f32, 4 },
    { ISD::FP_TO_SINT,  MVT::v16i16, MVT::v16f32, 8 },
    { ISD::FP_TO_UINT,  MVT::v16i16, MVT::v16f32, 8 }
  };

  if (SrcTy.isVector() && ST->hasNEON()) {
    if (const auto *Entry = ConvertCostTableLookup(NEONVectorConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  // Scalar float to integer conversions.
  static const TypeConversionCostTblEntry NEONFloatConversionTbl[] = {
    { ISD::FP_TO_SINT,  MVT::i1, MVT::f32, 2 },
    { ISD::FP_TO_UINT,  MVT::i1, MVT::f32, 2 },
    { ISD::FP_TO_SINT,  MVT::i1, MVT::f64, 2 },
    { ISD::FP_TO_UINT,  MVT::i1, MVT::f64, 2 },
    { ISD::FP_TO_SINT,  MVT::i8, MVT::f32, 2 },
    { ISD::FP_TO_UINT,  MVT::i8, MVT::f32, 2 },
    { ISD::FP_TO_SINT,  MVT::i8, MVT::f64, 2 },
    { ISD::FP_TO_UINT,  MVT::i8, MVT::f64, 2 },
    { ISD::FP_TO_SINT,  MVT::i16, MVT::f32, 2 },
    { ISD::FP_TO_UINT,  MVT::i16, MVT::f32, 2 },
    { ISD::FP_TO_SINT,  MVT::i16, MVT::f64, 2 },
    { ISD::FP_TO_UINT,  MVT::i16, MVT::f64, 2 },
    { ISD::FP_TO_SINT,  MVT::i32, MVT::f32, 2 },
    { ISD::FP_TO_UINT,  MVT::i32, MVT::f32, 2 },
    { ISD::FP_TO_SINT,  MVT::i32, MVT::f64, 2 },
    { ISD::FP_TO_UINT,  MVT::i32, MVT::f64, 2 },
    { ISD::FP_TO_SINT,  MVT::i64, MVT::f32, 10 },
    { ISD::FP_TO_UINT,  MVT::i64, MVT::f32, 10 },
    { ISD::FP_TO_SINT,  MVT::i64, MVT::f64, 10 },
    { ISD::FP_TO_UINT,  MVT::i64, MVT::f64, 10 }
  };
  if (SrcTy.isFloatingPoint() && ST->hasNEON()) {
    if (const auto *Entry = ConvertCostTableLookup(NEONFloatConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  // Scalar integer to float conversions.
  static const TypeConversionCostTblEntry NEONIntegerConversionTbl[] = {
    { ISD::SINT_TO_FP,  MVT::f32, MVT::i1, 2 },
    { ISD::UINT_TO_FP,  MVT::f32, MVT::i1, 2 },
    { ISD::SINT_TO_FP,  MVT::f64, MVT::i1, 2 },
    { ISD::UINT_TO_FP,  MVT::f64, MVT::i1, 2 },
    { ISD::SINT_TO_FP,  MVT::f32, MVT::i8, 2 },
    { ISD::UINT_TO_FP,  MVT::f32, MVT::i8, 2 },
    { ISD::SINT_TO_FP,  MVT::f64, MVT::i8, 2 },
    { ISD::UINT_TO_FP,  MVT::f64, MVT::i8, 2 },
    { ISD::SINT_TO_FP,  MVT::f32, MVT::i16, 2 },
    { ISD::UINT_TO_FP,  MVT::f32, MVT::i16, 2 },
    { ISD::SINT_TO_FP,  MVT::f64, MVT::i16, 2 },
    { ISD::UINT_TO_FP,  MVT::f64, MVT::i16, 2 },
    { ISD::SINT_TO_FP,  MVT::f32, MVT::i32, 2 },
    { ISD::UINT_TO_FP,  MVT::f32, MVT::i32, 2 },
    { ISD::SINT_TO_FP,  MVT::f64, MVT::i32, 2 },
    { ISD::UINT_TO_FP,  MVT::f64, MVT::i32, 2 },
    { ISD::SINT_TO_FP,  MVT::f32, MVT::i64, 10 },
    { ISD::UINT_TO_FP,  MVT::f32, MVT::i64, 10 },
    { ISD::SINT_TO_FP,  MVT::f64, MVT::i64, 10 },
    { ISD::UINT_TO_FP,  MVT::f64, MVT::i64, 10 }
  };

  if (SrcTy.isInteger() && ST->hasNEON()) {
    if (const auto *Entry = ConvertCostTableLookup(NEONIntegerConversionTbl,
                                                   ISD, DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  // MVE extend costs, taken from codegen tests. i8->i16 or i16->i32 is one
  // instruction, i8->i32 is two. i64 zexts are an VAND with a constant, sext
  // are linearised so take more.
  static const TypeConversionCostTblEntry MVEVectorConversionTbl[] = {
    { ISD::SIGN_EXTEND, MVT::v8i16, MVT::v8i8, 1 },
    { ISD::ZERO_EXTEND, MVT::v8i16, MVT::v8i8, 1 },
    { ISD::SIGN_EXTEND, MVT::v4i32, MVT::v4i8, 2 },
    { ISD::ZERO_EXTEND, MVT::v4i32, MVT::v4i8, 2 },
    { ISD::SIGN_EXTEND, MVT::v2i64, MVT::v2i8, 10 },
    { ISD::ZERO_EXTEND, MVT::v2i64, MVT::v2i8, 2 },
    { ISD::SIGN_EXTEND, MVT::v4i32, MVT::v4i16, 1 },
    { ISD::ZERO_EXTEND, MVT::v4i32, MVT::v4i16, 1 },
    { ISD::SIGN_EXTEND, MVT::v2i64, MVT::v2i16, 10 },
    { ISD::ZERO_EXTEND, MVT::v2i64, MVT::v2i16, 2 },
    { ISD::SIGN_EXTEND, MVT::v2i64, MVT::v2i32, 8 },
    { ISD::ZERO_EXTEND, MVT::v2i64, MVT::v2i32, 2 },
  };

  if (SrcTy.isVector() && ST->hasMVEIntegerOps()) {
    if (const auto *Entry = ConvertCostTableLookup(MVEVectorConversionTbl,
                                                   ISD, DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost * ST->getMVEVectorCostFactor();
  }

  // Scalar integer conversion costs.
  static const TypeConversionCostTblEntry ARMIntegerConversionTbl[] = {
    // i16 -> i64 requires two dependent operations.
    { ISD::SIGN_EXTEND, MVT::i64, MVT::i16, 2 },

    // Truncates on i64 are assumed to be free.
    { ISD::TRUNCATE,    MVT::i32, MVT::i64, 0 },
    { ISD::TRUNCATE,    MVT::i16, MVT::i64, 0 },
    { ISD::TRUNCATE,    MVT::i8,  MVT::i64, 0 },
    { ISD::TRUNCATE,    MVT::i1,  MVT::i64, 0 }
  };

  if (SrcTy.isInteger()) {
    if (const auto *Entry = ConvertCostTableLookup(ARMIntegerConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  int BaseCost = ST->hasMVEIntegerOps() && Src->isVectorTy()
                     ? ST->getMVEVectorCostFactor()
                     : 1;
  return BaseCost * BaseT::getCastInstrCost(Opcode, Dst, Src);
}

int ARMTTIImpl::getVectorInstrCost(unsigned Opcode, Type *ValTy,
                                   unsigned Index) {
  // Penalize inserting into an D-subregister. We end up with a three times
  // lower estimated throughput on swift.
  if (ST->hasSlowLoadDSubregister() && Opcode == Instruction::InsertElement &&
      ValTy->isVectorTy() && ValTy->getScalarSizeInBits() <= 32)
    return 3;

  if (ST->hasNEON() && (Opcode == Instruction::InsertElement ||
                        Opcode == Instruction::ExtractElement)) {
    // Cross-class copies are expensive on many microarchitectures,
    // so assume they are expensive by default.
    if (ValTy->getVectorElementType()->isIntegerTy())
      return 3;

    // Even if it's not a cross class copy, this likely leads to mixing
    // of NEON and VFP code and should be therefore penalized.
    if (ValTy->isVectorTy() &&
        ValTy->getScalarSizeInBits() <= 32)
      return std::max(BaseT::getVectorInstrCost(Opcode, ValTy, Index), 2U);
  }

  if (ST->hasMVEIntegerOps() && (Opcode == Instruction::InsertElement ||
                                 Opcode == Instruction::ExtractElement)) {
    // We say MVE moves costs at least the MVEVectorCostFactor, even though
    // they are scalar instructions. This helps prevent mixing scalar and
    // vector, to prevent vectorising where we end up just scalarising the
    // result anyway.
    return std::max(BaseT::getVectorInstrCost(Opcode, ValTy, Index),
                    ST->getMVEVectorCostFactor()) *
           ValTy->getVectorNumElements() / 2;
  }

  return BaseT::getVectorInstrCost(Opcode, ValTy, Index);
}

int ARMTTIImpl::getCmpSelInstrCost(unsigned Opcode, Type *ValTy, Type *CondTy,
                                   const Instruction *I) {
  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  // On NEON a vector select gets lowered to vbsl.
  if (ST->hasNEON() && ValTy->isVectorTy() && ISD == ISD::SELECT) {
    // Lowering of some vector selects is currently far from perfect.
    static const TypeConversionCostTblEntry NEONVectorSelectTbl[] = {
      { ISD::SELECT, MVT::v4i1, MVT::v4i64, 4*4 + 1*2 + 1 },
      { ISD::SELECT, MVT::v8i1, MVT::v8i64, 50 },
      { ISD::SELECT, MVT::v16i1, MVT::v16i64, 100 }
    };

    EVT SelCondTy = TLI->getValueType(DL, CondTy);
    EVT SelValTy = TLI->getValueType(DL, ValTy);
    if (SelCondTy.isSimple() && SelValTy.isSimple()) {
      if (const auto *Entry = ConvertCostTableLookup(NEONVectorSelectTbl, ISD,
                                                     SelCondTy.getSimpleVT(),
                                                     SelValTy.getSimpleVT()))
        return Entry->Cost;
    }

    std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, ValTy);
    return LT.first;
  }

  int BaseCost = ST->hasMVEIntegerOps() && ValTy->isVectorTy()
                     ? ST->getMVEVectorCostFactor()
                     : 1;
  return BaseCost * BaseT::getCmpSelInstrCost(Opcode, ValTy, CondTy, I);
}

int ARMTTIImpl::getAddressComputationCost(Type *Ty, ScalarEvolution *SE,
                                          const SCEV *Ptr) {
  // Address computations in vectorized code with non-consecutive addresses will
  // likely result in more instructions compared to scalar code where the
  // computation can more often be merged into the index mode. The resulting
  // extra micro-ops can significantly decrease throughput.
  unsigned NumVectorInstToHideOverhead = 10;
  int MaxMergeDistance = 64;

  if (ST->hasNEON()) {
    if (Ty->isVectorTy() && SE &&
        !BaseT::isConstantStridedAccessLessThan(SE, Ptr, MaxMergeDistance + 1))
      return NumVectorInstToHideOverhead;

    // In many cases the address computation is not merged into the instruction
    // addressing mode.
    return 1;
  }
  return BaseT::getAddressComputationCost(Ty, SE, Ptr);
}

int ARMTTIImpl::getMemcpyCost(const Instruction *I) {
  const MemCpyInst *MI = dyn_cast<MemCpyInst>(I);
  assert(MI && "MemcpyInst expected");
  ConstantInt *C = dyn_cast<ConstantInt>(MI->getLength());

  // To model the cost of a library call, we assume 1 for the call, and
  // 3 for the argument setup.
  const unsigned LibCallCost = 4;

  // If 'size' is not a constant, a library call will be generated.
  if (!C)
    return LibCallCost;

  const unsigned Size = C->getValue().getZExtValue();
  const unsigned DstAlign = MI->getDestAlignment();
  const unsigned SrcAlign = MI->getSourceAlignment();
  const Function *F = I->getParent()->getParent();
  const unsigned Limit = TLI->getMaxStoresPerMemmove(F->hasMinSize());
  std::vector<EVT> MemOps;

  // MemOps will be poplulated with a list of data types that needs to be
  // loaded and stored. That's why we multiply the number of elements by 2 to
  // get the cost for this memcpy.
  if (getTLI()->findOptimalMemOpLowering(
          MemOps, Limit, Size, DstAlign, SrcAlign, false /*IsMemset*/,
          false /*ZeroMemset*/, false /*MemcpyStrSrc*/, false /*AllowOverlap*/,
          MI->getDestAddressSpace(), MI->getSourceAddressSpace(),
          F->getAttributes()))
    return MemOps.size() * 2;

  // If we can't find an optimal memop lowering, return the default cost
  return LibCallCost;
}

int ARMTTIImpl::getShuffleCost(TTI::ShuffleKind Kind, Type *Tp, int Index,
                               Type *SubTp) {
  if (ST->hasNEON()) {
    if (Kind == TTI::SK_Broadcast) {
      static const CostTblEntry NEONDupTbl[] = {
          // VDUP handles these cases.
          {ISD::VECTOR_SHUFFLE, MVT::v2i32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2f32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2i64, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2f64, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v4i16, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v8i8, 1},

          {ISD::VECTOR_SHUFFLE, MVT::v4i32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v4f32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v8i16, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v16i8, 1}};

      std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Tp);

      if (const auto *Entry =
              CostTableLookup(NEONDupTbl, ISD::VECTOR_SHUFFLE, LT.second))
        return LT.first * Entry->Cost;
    }
    if (Kind == TTI::SK_Reverse) {
      static const CostTblEntry NEONShuffleTbl[] = {
          // Reverse shuffle cost one instruction if we are shuffling within a
          // double word (vrev) or two if we shuffle a quad word (vrev, vext).
          {ISD::VECTOR_SHUFFLE, MVT::v2i32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2f32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2i64, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2f64, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v4i16, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v8i8, 1},

          {ISD::VECTOR_SHUFFLE, MVT::v4i32, 2},
          {ISD::VECTOR_SHUFFLE, MVT::v4f32, 2},
          {ISD::VECTOR_SHUFFLE, MVT::v8i16, 2},
          {ISD::VECTOR_SHUFFLE, MVT::v16i8, 2}};

      std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Tp);

      if (const auto *Entry =
              CostTableLookup(NEONShuffleTbl, ISD::VECTOR_SHUFFLE, LT.second))
        return LT.first * Entry->Cost;
    }
    if (Kind == TTI::SK_Select) {
      static const CostTblEntry NEONSelShuffleTbl[] = {
          // Select shuffle cost table for ARM. Cost is the number of
          // instructions
          // required to create the shuffled vector.

          {ISD::VECTOR_SHUFFLE, MVT::v2f32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2i64, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2f64, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v2i32, 1},

          {ISD::VECTOR_SHUFFLE, MVT::v4i32, 2},
          {ISD::VECTOR_SHUFFLE, MVT::v4f32, 2},
          {ISD::VECTOR_SHUFFLE, MVT::v4i16, 2},

          {ISD::VECTOR_SHUFFLE, MVT::v8i16, 16},

          {ISD::VECTOR_SHUFFLE, MVT::v16i8, 32}};

      std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Tp);
      if (const auto *Entry = CostTableLookup(NEONSelShuffleTbl,
                                              ISD::VECTOR_SHUFFLE, LT.second))
        return LT.first * Entry->Cost;
    }
  }
  if (ST->hasMVEIntegerOps()) {
    if (Kind == TTI::SK_Broadcast) {
      static const CostTblEntry MVEDupTbl[] = {
          // VDUP handles these cases.
          {ISD::VECTOR_SHUFFLE, MVT::v4i32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v8i16, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v16i8, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v4f32, 1},
          {ISD::VECTOR_SHUFFLE, MVT::v8f16, 1}};

      std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Tp);

      if (const auto *Entry = CostTableLookup(MVEDupTbl, ISD::VECTOR_SHUFFLE,
                                              LT.second))
        return LT.first * Entry->Cost * ST->getMVEVectorCostFactor();
    }
  }
  int BaseCost = ST->hasMVEIntegerOps() && Tp->isVectorTy()
                     ? ST->getMVEVectorCostFactor()
                     : 1;
  return BaseCost * BaseT::getShuffleCost(Kind, Tp, Index, SubTp);
}

int ARMTTIImpl::getArithmeticInstrCost(
    unsigned Opcode, Type *Ty, TTI::OperandValueKind Op1Info,
    TTI::OperandValueKind Op2Info, TTI::OperandValueProperties Opd1PropInfo,
    TTI::OperandValueProperties Opd2PropInfo,
    ArrayRef<const Value *> Args) {
  int ISDOpcode = TLI->InstructionOpcodeToISD(Opcode);
  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Ty);

  const unsigned FunctionCallDivCost = 20;
  const unsigned ReciprocalDivCost = 10;
  static const CostTblEntry CostTbl[] = {
    // Division.
    // These costs are somewhat random. Choose a cost of 20 to indicate that
    // vectorizing devision (added function call) is going to be very expensive.
    // Double registers types.
    { ISD::SDIV, MVT::v1i64, 1 * FunctionCallDivCost},
    { ISD::UDIV, MVT::v1i64, 1 * FunctionCallDivCost},
    { ISD::SREM, MVT::v1i64, 1 * FunctionCallDivCost},
    { ISD::UREM, MVT::v1i64, 1 * FunctionCallDivCost},
    { ISD::SDIV, MVT::v2i32, 2 * FunctionCallDivCost},
    { ISD::UDIV, MVT::v2i32, 2 * FunctionCallDivCost},
    { ISD::SREM, MVT::v2i32, 2 * FunctionCallDivCost},
    { ISD::UREM, MVT::v2i32, 2 * FunctionCallDivCost},
    { ISD::SDIV, MVT::v4i16,     ReciprocalDivCost},
    { ISD::UDIV, MVT::v4i16,     ReciprocalDivCost},
    { ISD::SREM, MVT::v4i16, 4 * FunctionCallDivCost},
    { ISD::UREM, MVT::v4i16, 4 * FunctionCallDivCost},
    { ISD::SDIV, MVT::v8i8,      ReciprocalDivCost},
    { ISD::UDIV, MVT::v8i8,      ReciprocalDivCost},
    { ISD::SREM, MVT::v8i8,  8 * FunctionCallDivCost},
    { ISD::UREM, MVT::v8i8,  8 * FunctionCallDivCost},
    // Quad register types.
    { ISD::SDIV, MVT::v2i64, 2 * FunctionCallDivCost},
    { ISD::UDIV, MVT::v2i64, 2 * FunctionCallDivCost},
    { ISD::SREM, MVT::v2i64, 2 * FunctionCallDivCost},
    { ISD::UREM, MVT::v2i64, 2 * FunctionCallDivCost},
    { ISD::SDIV, MVT::v4i32, 4 * FunctionCallDivCost},
    { ISD::UDIV, MVT::v4i32, 4 * FunctionCallDivCost},
    { ISD::SREM, MVT::v4i32, 4 * FunctionCallDivCost},
    { ISD::UREM, MVT::v4i32, 4 * FunctionCallDivCost},
    { ISD::SDIV, MVT::v8i16, 8 * FunctionCallDivCost},
    { ISD::UDIV, MVT::v8i16, 8 * FunctionCallDivCost},
    { ISD::SREM, MVT::v8i16, 8 * FunctionCallDivCost},
    { ISD::UREM, MVT::v8i16, 8 * FunctionCallDivCost},
    { ISD::SDIV, MVT::v16i8, 16 * FunctionCallDivCost},
    { ISD::UDIV, MVT::v16i8, 16 * FunctionCallDivCost},
    { ISD::SREM, MVT::v16i8, 16 * FunctionCallDivCost},
    { ISD::UREM, MVT::v16i8, 16 * FunctionCallDivCost},
    // Multiplication.
  };

  if (ST->hasNEON()) {
    if (const auto *Entry = CostTableLookup(CostTbl, ISDOpcode, LT.second))
      return LT.first * Entry->Cost;

    int Cost = BaseT::getArithmeticInstrCost(Opcode, Ty, Op1Info, Op2Info,
                                             Opd1PropInfo, Opd2PropInfo);

    // This is somewhat of a hack. The problem that we are facing is that SROA
    // creates a sequence of shift, and, or instructions to construct values.
    // These sequences are recognized by the ISel and have zero-cost. Not so for
    // the vectorized code. Because we have support for v2i64 but not i64 those
    // sequences look particularly beneficial to vectorize.
    // To work around this we increase the cost of v2i64 operations to make them
    // seem less beneficial.
    if (LT.second == MVT::v2i64 &&
        Op2Info == TargetTransformInfo::OK_UniformConstantValue)
      Cost += 4;

    return Cost;
  }

  int BaseCost = ST->hasMVEIntegerOps() && Ty->isVectorTy()
                     ? ST->getMVEVectorCostFactor()
                     : 1;

  // The rest of this mostly follows what is done in BaseT::getArithmeticInstrCost,
  // without treating floats as more expensive that scalars or increasing the
  // costs for custom operations. The results is also multiplied by the
  // MVEVectorCostFactor where appropriate.
  if (TLI->isOperationLegalOrCustomOrPromote(ISDOpcode, LT.second))
    return LT.first * BaseCost;

  // Else this is expand, assume that we need to scalarize this op.
  if (Ty->isVectorTy()) {
    unsigned Num = Ty->getVectorNumElements();
    unsigned Cost = getArithmeticInstrCost(Opcode, Ty->getScalarType());
    // Return the cost of multiple scalar invocation plus the cost of
    // inserting and extracting the values.
    return BaseT::getScalarizationOverhead(Ty, Args) + Num * Cost;
  }

  return BaseCost;
}

int ARMTTIImpl::getMemoryOpCost(unsigned Opcode, Type *Src, unsigned Alignment,
                                unsigned AddressSpace, const Instruction *I) {
  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Src);

  if (ST->hasNEON() && Src->isVectorTy() && Alignment != 16 &&
      Src->getVectorElementType()->isDoubleTy()) {
    // Unaligned loads/stores are extremely inefficient.
    // We need 4 uops for vst.1/vld.1 vs 1uop for vldr/vstr.
    return LT.first * 4;
  }
  int BaseCost = ST->hasMVEIntegerOps() && Src->isVectorTy()
                     ? ST->getMVEVectorCostFactor()
                     : 1;
  return BaseCost * LT.first;
}

int ARMTTIImpl::getInterleavedMemoryOpCost(unsigned Opcode, Type *VecTy,
                                           unsigned Factor,
                                           ArrayRef<unsigned> Indices,
                                           unsigned Alignment,
                                           unsigned AddressSpace,
                                           bool UseMaskForCond,
                                           bool UseMaskForGaps) {
  assert(Factor >= 2 && "Invalid interleave factor");
  assert(isa<VectorType>(VecTy) && "Expect a vector type");

  // vldN/vstN doesn't support vector types of i64/f64 element.
  bool EltIs64Bits = DL.getTypeSizeInBits(VecTy->getScalarType()) == 64;

  if (Factor <= TLI->getMaxSupportedInterleaveFactor() && !EltIs64Bits &&
      !UseMaskForCond && !UseMaskForGaps) {
    unsigned NumElts = VecTy->getVectorNumElements();
    auto *SubVecTy = VectorType::get(VecTy->getScalarType(), NumElts / Factor);

    // vldN/vstN only support legal vector types of size 64 or 128 in bits.
    // Accesses having vector types that are a multiple of 128 bits can be
    // matched to more than one vldN/vstN instruction.
    if (NumElts % Factor == 0 &&
        TLI->isLegalInterleavedAccessType(SubVecTy, DL))
      return Factor * TLI->getNumInterleavedAccesses(SubVecTy, DL);
  }

  return BaseT::getInterleavedMemoryOpCost(Opcode, VecTy, Factor, Indices,
                                           Alignment, AddressSpace,
                                           UseMaskForCond, UseMaskForGaps);
}

bool ARMTTIImpl::isLoweredToCall(const Function *F) {
  if (!F->isIntrinsic())
    BaseT::isLoweredToCall(F);

  // Assume all Arm-specific intrinsics map to an instruction.
  if (F->getName().startswith("llvm.arm"))
    return false;

  switch (F->getIntrinsicID()) {
  default: break;
  case Intrinsic::powi:
  case Intrinsic::sin:
  case Intrinsic::cos:
  case Intrinsic::pow:
  case Intrinsic::log:
  case Intrinsic::log10:
  case Intrinsic::log2:
  case Intrinsic::exp:
  case Intrinsic::exp2:
    return true;
  case Intrinsic::sqrt:
  case Intrinsic::fabs:
  case Intrinsic::copysign:
  case Intrinsic::floor:
  case Intrinsic::ceil:
  case Intrinsic::trunc:
  case Intrinsic::rint:
  case Intrinsic::nearbyint:
  case Intrinsic::round:
  case Intrinsic::canonicalize:
  case Intrinsic::lround:
  case Intrinsic::llround:
  case Intrinsic::lrint:
  case Intrinsic::llrint:
    if (F->getReturnType()->isDoubleTy() && !ST->hasFP64())
      return true;
    if (F->getReturnType()->isHalfTy() && !ST->hasFullFP16())
      return true;
    // Some operations can be handled by vector instructions and assume
    // unsupported vectors will be expanded into supported scalar ones.
    // TODO Handle scalar operations properly.
    return !ST->hasFPARMv8Base() && !ST->hasVFP2Base();
  case Intrinsic::masked_store:
  case Intrinsic::masked_load:
  case Intrinsic::masked_gather:
  case Intrinsic::masked_scatter:
    return !ST->hasMVEIntegerOps();
  case Intrinsic::sadd_with_overflow:
  case Intrinsic::uadd_with_overflow:
  case Intrinsic::ssub_with_overflow:
  case Intrinsic::usub_with_overflow:
  case Intrinsic::sadd_sat:
  case Intrinsic::uadd_sat:
  case Intrinsic::ssub_sat:
  case Intrinsic::usub_sat:
    return false;
  }

  return BaseT::isLoweredToCall(F);
}

bool ARMTTIImpl::isHardwareLoopProfitable(Loop *L, ScalarEvolution &SE,
                                          AssumptionCache &AC,
                                          TargetLibraryInfo *LibInfo,
                                          HardwareLoopInfo &HWLoopInfo) {
  // Low-overhead branches are only supported in the 'low-overhead branch'
  // extension of v8.1-m.
  if (!ST->hasLOB() || DisableLowOverheadLoops)
    return false;

  if (!SE.hasLoopInvariantBackedgeTakenCount(L))
    return false;

  const SCEV *BackedgeTakenCount = SE.getBackedgeTakenCount(L);
  if (isa<SCEVCouldNotCompute>(BackedgeTakenCount))
    return false;

  const SCEV *TripCountSCEV =
    SE.getAddExpr(BackedgeTakenCount,
                  SE.getOne(BackedgeTakenCount->getType()));

  // We need to store the trip count in LR, a 32-bit register.
  if (SE.getUnsignedRangeMax(TripCountSCEV).getBitWidth() > 32)
    return false;

  // Making a call will trash LR and clear LO_BRANCH_INFO, so there's little
  // point in generating a hardware loop if that's going to happen.
  auto MaybeCall = [this](Instruction &I) {
    const ARMTargetLowering *TLI = getTLI();
    unsigned ISD = TLI->InstructionOpcodeToISD(I.getOpcode());
    EVT VT = TLI->getValueType(DL, I.getType(), true);
    if (TLI->getOperationAction(ISD, VT) == TargetLowering::LibCall)
      return true;

    // Check if an intrinsic will be lowered to a call and assume that any
    // other CallInst will generate a bl.
    if (auto *Call = dyn_cast<CallInst>(&I)) {
      if (isa<IntrinsicInst>(Call)) {
        if (const Function *F = Call->getCalledFunction())
          return isLoweredToCall(F);
      }
      return true;
    }

    // FPv5 provides conversions between integer, double-precision,
    // single-precision, and half-precision formats.
    switch (I.getOpcode()) {
    default:
      break;
    case Instruction::FPToSI:
    case Instruction::FPToUI:
    case Instruction::SIToFP:
    case Instruction::UIToFP:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
      return !ST->hasFPARMv8Base();
    }

    // FIXME: Unfortunately the approach of checking the Operation Action does
    // not catch all cases of Legalization that use library calls. Our
    // Legalization step categorizes some transformations into library calls as
    // Custom, Expand or even Legal when doing type legalization. So for now
    // we have to special case for instance the SDIV of 64bit integers and the
    // use of floating point emulation.
    if (VT.isInteger() && VT.getSizeInBits() >= 64) {
      switch (ISD) {
      default:
        break;
      case ISD::SDIV:
      case ISD::UDIV:
      case ISD::SREM:
      case ISD::UREM:
      case ISD::SDIVREM:
      case ISD::UDIVREM:
        return true;
      }
    }

    // Assume all other non-float operations are supported.
    if (!VT.isFloatingPoint())
      return false;

    // We'll need a library call to handle most floats when using soft.
    if (TLI->useSoftFloat()) {
      switch (I.getOpcode()) {
      default:
        return true;
      case Instruction::Alloca:
      case Instruction::Load:
      case Instruction::Store:
      case Instruction::Select:
      case Instruction::PHI:
        return false;
      }
    }

    // We'll need a libcall to perform double precision operations on a single
    // precision only FPU.
    if (I.getType()->isDoubleTy() && !ST->hasFP64())
      return true;

    // Likewise for half precision arithmetic.
    if (I.getType()->isHalfTy() && !ST->hasFullFP16())
      return true;

    return false;
  };

  auto IsHardwareLoopIntrinsic = [](Instruction &I) {
    if (auto *Call = dyn_cast<IntrinsicInst>(&I)) {
      switch (Call->getIntrinsicID()) {
      default:
        break;
      case Intrinsic::set_loop_iterations:
      case Intrinsic::test_set_loop_iterations:
      case Intrinsic::loop_decrement:
      case Intrinsic::loop_decrement_reg:
        return true;
      }
    }
    return false;
  };

  // Scan the instructions to see if there's any that we know will turn into a
  // call or if this loop is already a low-overhead loop.
  auto ScanLoop = [&](Loop *L) {
    for (auto *BB : L->getBlocks()) {
      for (auto &I : *BB) {
        if (MaybeCall(I) || IsHardwareLoopIntrinsic(I))
          return false;
      }
    }
    return true;
  };

  // Visit inner loops.
  for (auto Inner : *L)
    if (!ScanLoop(Inner))
      return false;

  if (!ScanLoop(L))
    return false;

  // TODO: Check whether the trip count calculation is expensive. If L is the
  // inner loop but we know it has a low trip count, calculating that trip
  // count (in the parent loop) may be detrimental.

  LLVMContext &C = L->getHeader()->getContext();
  HWLoopInfo.CounterInReg = true;
  HWLoopInfo.IsNestingLegal = false;
  HWLoopInfo.PerformEntryTest = true;
  HWLoopInfo.CountType = Type::getInt32Ty(C);
  HWLoopInfo.LoopDecrement = ConstantInt::get(HWLoopInfo.CountType, 1);
  return true;
}

void ARMTTIImpl::getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                                         TTI::UnrollingPreferences &UP) {
  // Only currently enable these preferences for M-Class cores.
  if (!ST->isMClass())
    return BasicTTIImplBase::getUnrollingPreferences(L, SE, UP);

  // Disable loop unrolling for Oz and Os.
  UP.OptSizeThreshold = 0;
  UP.PartialOptSizeThreshold = 0;
  if (L->getHeader()->getParent()->hasOptSize())
    return;

  // Only enable on Thumb-2 targets.
  if (!ST->isThumb2())
    return;

  SmallVector<BasicBlock*, 4> ExitingBlocks;
  L->getExitingBlocks(ExitingBlocks);
  LLVM_DEBUG(dbgs() << "Loop has:\n"
                    << "Blocks: " << L->getNumBlocks() << "\n"
                    << "Exit blocks: " << ExitingBlocks.size() << "\n");

  // Only allow another exit other than the latch. This acts as an early exit
  // as it mirrors the profitability calculation of the runtime unroller.
  if (ExitingBlocks.size() > 2)
    return;

  // Limit the CFG of the loop body for targets with a branch predictor.
  // Allowing 4 blocks permits if-then-else diamonds in the body.
  if (ST->hasBranchPredictor() && L->getNumBlocks() > 4)
    return;

  // Scan the loop: don't unroll loops with calls as this could prevent
  // inlining.
  unsigned Cost = 0;
  for (auto *BB : L->getBlocks()) {
    for (auto &I : *BB) {
      if (isa<CallInst>(I) || isa<InvokeInst>(I)) {
        ImmutableCallSite CS(&I);
        if (const Function *F = CS.getCalledFunction()) {
          if (!isLoweredToCall(F))
            continue;
        }
        return;
      }
      // Don't unroll vectorised loop. MVE does not benefit from it as much as
      // scalar code.
      if (I.getType()->isVectorTy())
        return;

      SmallVector<const Value*, 4> Operands(I.value_op_begin(),
                                            I.value_op_end());
      Cost += getUserCost(&I, Operands);
    }
  }

  LLVM_DEBUG(dbgs() << "Cost of loop: " << Cost << "\n");

  UP.Partial = true;
  UP.Runtime = true;
  UP.UpperBound = true;
  UP.UnrollRemainder = true;
  UP.DefaultUnrollRuntimeCount = 4;
  UP.UnrollAndJam = true;
  UP.UnrollAndJamInnerLoopThreshold = 60;

  // Force unrolling small loops can be very useful because of the branch
  // taken cost of the backedge.
  if (Cost < 12)
    UP.Force = true;
}

bool ARMTTIImpl::useReductionIntrinsic(unsigned Opcode, Type *Ty,
                                       TTI::ReductionFlags Flags) const {
  assert(isa<VectorType>(Ty) && "Expected Ty to be a vector type");
  unsigned ScalarBits = Ty->getScalarSizeInBits();
  if (!ST->hasMVEIntegerOps())
    return false;

  switch (Opcode) {
  case Instruction::FAdd:
  case Instruction::FMul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Mul:
  case Instruction::FCmp:
    return false;
  case Instruction::ICmp:
  case Instruction::Add:
    return ScalarBits < 64 && ScalarBits * Ty->getVectorNumElements() == 128;
  default:
    llvm_unreachable("Unhandled reduction opcode");
  }
  return false;
}
