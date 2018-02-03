//===--- SourceUnparser.cpp --- Source Info Unparser ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements unparser to print metadata objects as a constructs of
// an appropriate high-level language.
//
//===----------------------------------------------------------------------===//

#include "SourceUnparser.h"
#include "tsar_utility.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/ADT/SmallBitVector.h>
#include <llvm/Support/raw_ostream.h>

using namespace tsar;
using namespace llvm;

bool SourceUnparserImp::unparseAsScalarTy(uint64_t Offset, bool IsPositive) {
  if (Offset == 0)
    return true;
  if (!mIsAddress) {
    updatePriority(TOKEN_ADDRESS, TOKEN_ADDRESS);
    mReversePrefix.push_back(TOKEN_ADDRESS);
    mIsAddress = true;
  }
  updatePriority(TOKEN_CAST_TO_ADDRESS, IsPositive ? TOKEN_ADD : TOKEN_SUB);
  mReversePrefix.push_back(TOKEN_CAST_TO_ADDRESS);
  mCurrType = nullptr;
  mSuffix.push_back(IsPositive ? TOKEN_ADD : TOKEN_SUB);
  mSuffix.push_back(TOKEN_UCONST);
  mUConsts.push_back(Offset);
  return true;
}

bool SourceUnparserImp::unparseAsStructureTy(uint64_t Offset, bool IsPositive) {
  assert(mCurrType && "Currently evaluated type must not be null!");
  auto DICTy = cast<DICompositeType>(mCurrType);
  auto TySize = getSize(mCurrType);
  if (DICTy->getElements().size() == 0 || !IsPositive || TySize <= Offset)
    return unparseAsScalarTy(Offset, IsPositive);
  DIDerivedType *CurrEl = nullptr;
  for (auto El: DICTy->getElements()) {
    auto DITy = cast<DIDerivedType>(stripDIType(cast<DIType>(El)));
    auto ElOffset = DITy->getOffsetInBits() / 8;
    // It is necessary to use both checks (== and >) to accurately evaluate
    // structures with bit fields. In this case the first field will be
    // used. To avoid usage of subsequent bit fields instead of it (==) is
    // necessary.
    if (ElOffset > Offset) {
      break;
    } else if (ElOffset == Offset) {
      CurrEl = DITy;
      break;
    }
    CurrEl = DITy;
  }
  assert(CurrEl && "Element of a structure must not be null!");
  assert(Offset >= CurrEl->getOffsetInBits() / 8 &&
    "Too large offset of a structure filed!");
  if (mIsAddress) {
    updatePriority(TOKEN_DEREF, TOKEN_DEREF);
    mReversePrefix.push_back(TOKEN_DEREF);
    mIsAddress = false;
  }
  updatePriority(TOKEN_FIELD, TOKEN_FIELD);
  mSuffix.append({ TOKEN_FIELD,TOKEN_IDENTIFIER });
  mIdentifiers.push_back(CurrEl->getName());
  mCurrType = stripDIType(CurrEl->getBaseType()).resolve();
  Offset -= CurrEl->getOffsetInBits() / 8;
  return unparse(Offset, true);
}

bool SourceUnparserImp::unparseAsUnionTy(uint64_t Offset, bool IsPositive) {
  return unparseAsScalarTy(Offset, IsPositive);
}

bool SourceUnparserImp::unparseAsPointerTy(uint64_t Offset, bool IsPositive) {
  assert(mCurrType && "Currently evaluated type must not be null!");
  if (!mIsAddress)
    return unparseAsScalarTy(Offset, IsPositive);
  auto DIDTy = cast<DIDerivedType>(mCurrType);
  mCurrType = stripDIType(DIDTy->getBaseType()).resolve();
  auto TySize = mCurrType ? getSize(mCurrType) : 0;
  if (TySize == 0)
    return unparseAsScalarTy(Offset, IsPositive);
  auto ElIdx = Offset / TySize;
  Offset -= ElIdx * TySize;
  if (IsPositive) {
    updatePriority(TOKEN_SUBSCRIPT_BEGIN, TOKEN_SUBSCRIPT_END);
    mSuffix.append({
      TOKEN_SUBSCRIPT_BEGIN, TOKEN_UCONST, TOKEN_SUBSCRIPT_END });
  } else {
    updatePriority(TOKEN_SUB, TOKEN_SUB);
    mSuffix.append({ TOKEN_SUB, TOKEN_UCONST });
  }
  mUConsts.push_back(Offset);
  return unparse(Offset, IsPositive);

}

bool SourceUnparserImp::unparseAsArrayTy(uint64_t Offset, bool IsPositive) {
  assert(mCurrType && "Currently evaluated type must not be null!");
  auto DICTy = cast<DICompositeType>(mCurrType);
  auto TySize = getSize(mCurrType);
  mCurrType = stripDIType(DICTy->getBaseType()).resolve();
  auto ElSize = mCurrType ? getSize(mCurrType) : 0;
  mIsAddress = true;
  if (DICTy->getElements().size() == 0 || !IsPositive || TySize <= Offset ||
      ElSize == 0)
    return unparseAsScalarTy(Offset, IsPositive);
  SmallVector<std::pair<int64_t, int64_t>, 8> Dims;
  if (mIsForwardDim)
    for (unsigned I = 0, E = DICTy->getElements().size(); I != E; ++I) {
      auto Dim = cast<DISubrange>(DICTy->getElements()[I]);
      if (Dim->getCount() <= 0)
        return unparseAsScalarTy(Offset, IsPositive);
      Dims.push_back(std::make_pair(Dim->getLowerBound(), Dim->getCount()));
    }
  else
    for (unsigned I = DICTy->getElements().size() - 1, E = 0; I != E; --I) {
      auto Dim = cast<DISubrange>(DICTy->getElements()[I]);
      if (Dim->getCount() <= 0)
        return unparseAsScalarTy(Offset, IsPositive);
      Dims.push_back(std::make_pair(Dim->getLowerBound(), Dim->getCount()));
    }
  mIsAddress = false;
  auto ElIdx = Offset / ElSize;
  Offset -= ElIdx * ElSize;
  updatePriority(TOKEN_SUBSCRIPT_BEGIN, TOKEN_SUBSCRIPT_END);
  mSuffix.push_back(TOKEN_SUBSCRIPT_BEGIN);
  for (unsigned Dim = 0, DimE = Dims.size(); Dim < Dims.size(); ++Dim) {
    unsigned Coeff = 1;
    for (unsigned I = Dim + 1; I < DimE; Coeff *= Dims[I].second, ++I);
    auto DimOffset = ElIdx / Coeff;
    if (Dims[Dim].first < 0 && DimOffset < std::abs(Dims[Dim].first)) {
      mSuffix.push_back(TOKEN_SUB);
      DimOffset = std::abs(Dims[Dim].first) - DimOffset;
    } else {
      DimOffset += Dims[Dim].first;
    }
    mSuffix.push_back(TOKEN_UCONST);
    mUConsts.push_back(DimOffset);
    ElIdx = ElIdx % Coeff;
  }
  mSuffix.push_back(TOKEN_SUBSCRIPT_END);
  return unparse(Offset, IsPositive);
}

bool SourceUnparserImp::unparseDeref() {
  if (mIsAddress) {
    updatePriority(TOKEN_DEREF, TOKEN_DEREF);
    mReversePrefix.push_back(TOKEN_DEREF);
    mIsAddress = false;
  } else {
    // Do not lower type hear because the pointer types will be evaluated
    // later in a separate method.
    if (mCurrType && mCurrType->getTag() != dwarf::DW_TAG_pointer_type)
      mCurrType = nullptr;
    mIsAddress = true;
  }
  return true;
}

bool SourceUnparserImp::unparse(uint64_t Offset, bool IsPositive) {
  if (mCurrType)
    switch (mCurrType->getTag()) {
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
      return unparseAsStructureTy(Offset, IsPositive);
    case dwarf::DW_TAG_array_type:
      return unparseAsArrayTy(Offset, IsPositive);
    case dwarf::DW_TAG_pointer_type:
      return unparseAsPointerTy(Offset, IsPositive);
    case dwarf::DW_TAG_union_type:
      return unparseAsUnionTy(Offset, IsPositive);
    }
  return unparseAsScalarTy(Offset, IsPositive);
}

bool SourceUnparserImp::unparse() {
  clear();
  mIdentifiers.push_back(mLoc.Var->getName());
  mLastOpPriority = getPriority(TOKEN_IDENTIFIER);
  SmallVector<uint64_t, 4> Offsets;
  SmallBitVector SignMask;
  mLoc.getOffsets(Offsets, SignMask);
  assert(!Offsets.empty() && "Number of offsets must not be null!");
  mCurrType = stripDIType(mLoc.Var->getType()).resolve();
  for (unsigned OffsetIdx = 0, E = Offsets.size() - 1;
       OffsetIdx < E; ++OffsetIdx) {
    if (!unparse(Offsets[OffsetIdx], SignMask.test(OffsetIdx)) ||
        !unparseDeref())
      return false;
  }
  if (!mCurrType || Offsets.back() != 0 || mLoc.getSize() < getSize(mCurrType))
    if (!unparse(Offsets.back(), SignMask.test(Offsets.size() - 1)))
      return false;
  if (mIsAddress) {
    updatePriority(TOKEN_DEREF, TOKEN_DEREF);
    mReversePrefix.push_back(TOKEN_DEREF);
  }
  return true;
}