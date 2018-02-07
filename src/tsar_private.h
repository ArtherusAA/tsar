//===--- tsar_private.h - Private Variable Analyzer -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines passes to determine locations which can be privatized.
// We use data-flow framework to implement this kind of analysis. This file
// contains elements which is necessary to determine this framework.
// The following articles can be helpful to understand it:
//  * "Automatic Array Privatization" Peng Tu and David Padua
//  * "Array Privatization for Parallel Execution of Loops" Zhiyuan Li.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRIVATE_H
#define TSAR_PRIVATE_H

#include "tsar_df_graph.h"
#include "tsar_df_location.h"
#include "DefinedMemory.h"
#include "GraphNumbering.h"
#include "LiveMemory.h"
#include "tsar_pass.h"
#include "tsar_trait.h"
#include "tsar_utility.h"
#include <utility.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Pass.h>
#include <forward_list>
#include <tuple>

namespace tsar {
class AliasNode;
class AliasTree;
class DefUseSet;
class DFLoop;
class EstimateMemory;
template<class GraphType> class SpanningTreeRelation;

/// This determine relation between two nodes in an alias tree.
using AliasTreeRelation = SpanningTreeRelation<const AliasTree *>;

/// Information about privatizability of locations for an analyzed region.
using PrivateInfo =
  llvm::DenseMap<DFNode *, std::unique_ptr<DependencySet>,
    llvm::DenseMapInfo<DFNode *>,
    TaggedDenseMapPair<
      bcl::tagged<DFNode *, DFNode>,
      bcl::tagged<std::unique_ptr<DependencySet>, DependencySet>>>;

namespace detail {
enum TraitId : unsigned long long;
class TraitImp;
}
}

namespace llvm {
class Loop;

/// This pass determines locations which can be privatized.
class PrivateRecognitionPass :
    public FunctionPass, private bcl::Uncopyable {
  /// \brief Map from memory location to traits.
  ///
  /// Note, that usage of DenseSet instead of DenseMap in this case may
  /// degraded performance. It is possible to store EstimateMemory into Trait
  /// structure, but in this case to insert new trait search will be performed
  /// two times. At first, find() will be called and than insert(). If map
  /// is used only insert() is necessary (see resolveAccesses() for details).
  using TraitMap = DenseMap<
    const tsar::EstimateMemory *, tsar::detail::TraitImp *,
    DenseMapInfo<const tsar::EstimateMemory *>,
    tsar::TaggedDenseMapPair<
      bcl::tagged<const tsar::EstimateMemory *, tsar::EstimateMemory>,
      bcl::tagged<tsar::detail::TraitImp *, tsar::detail::TraitImp>>>;

  /// Map from unknown memory location to traits.
  using UnknownMap = DenseMap<
    const llvm::Instruction *,
    std::tuple<const tsar::AliasNode *, tsar::detail::TraitImp*>,
    DenseMapInfo<const llvm::Instruction *>,
    tsar::TaggedDenseMapTuple<
      bcl::tagged<const llvm::Instruction *, llvm::Instruction>,
      bcl::tagged<const tsar::AliasNode *, tsar::AliasNode>,
      bcl::tagged<tsar::detail::TraitImp *, tsar::detail::TraitImp>>>;

  /// List of memory location traits.
  using TraitList = std::forward_list<bcl::tagged_pair<
      bcl::tagged<const tsar::EstimateMemory *, tsar::EstimateMemory>,
      bcl::tagged<tsar::detail::TraitImp, tsar::detail::TraitImp>>>;

  /// List of unknown memory location traits.
  using UnknownList = std::forward_list<bcl::tagged_pair<
      bcl::tagged<const llvm::Instruction *, llvm::Instruction>,
      bcl::tagged<tsar::detail::TraitImp, tsar::detail::TraitImp>>>;

  /// Pair of lists of traits.
  using TraitPair = bcl::tagged_tuple<
      bcl::tagged<TraitList *, TraitList>,
      bcl::tagged<UnknownList *, UnknownList>>;

  /// Map from alias node to a number of memory locations.
  using AliasMap = DenseMap<const tsar::AliasNode *,
    std::tuple<TraitList, UnknownList>,
    DenseMapInfo<const tsar::AliasNode *>,
    tsar::TaggedDenseMapTuple<
      bcl::tagged<const tsar::AliasNode *, tsar::AliasNode>,
      bcl::tagged<TraitList, TraitList>,
      bcl::tagged<UnknownList, UnknownList>>>;

public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  PrivateRecognitionPass() : FunctionPass(ID) {
    initializePrivateRecognitionPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about privatizability of locations for an analyzed
  /// region.
  tsar::PrivateInfo & getPrivateInfo() noexcept {return mPrivates;}

  /// Returns information about privatizability of locations for an analyzed
  /// region.
  const tsar::PrivateInfo & getPrivateInfo() const noexcept {return mPrivates;}

  /// Recognizes private (last private) variables for loops
  /// in the specified function.
  /// \pre A control-flow graph of the specified function must not contain
  /// unreachable nodes.
  bool runOnFunction(Function &F) override;

  /// Releases allocated memory.
  void releaseMemory() override {
    mPrivates.clear();
    mDefInfo = nullptr;
    mLiveInfo = nullptr;
    mAliasTree = nullptr;
  }

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Prints out the internal state of the pass. This also used to produce
  /// analysis correctness tests.
  void print(raw_ostream &OS, const Module *M) const override;

private:
  /// \brief Implements recognition of privatizable locations.
  ///
  /// Privatizability analysis is performed in two steps. Firstly,
  /// body of each natural loop is analyzed. Secondly, when live locations
  /// for each basic block are discovered, results of loop body analysis must be
  /// finalized. The result of this analysis should be complemented to separate
  /// private from last private locations. The case where location access
  /// is performed by pointer is also considered. Shared locations also
  /// analyzed.
  /// \param [in] Numbers This contains numbers of all alias nodes.
  /// \param [in, out] R Region in a data-flow graph, it can not be null.
  /// \pre Results of live memory analysis and reach definition analysis
  /// must be available from mLiveInfo and mDefInfo.
  void resolveCandidats(
    const tsar::GraphNumbering<const tsar::AliasNode *> &Numbers,
    tsar::DFRegion *R);

  /// \brief Evaluates explicitly accessed variables in a loop.
  ///
  /// Preliminary results will be stored in ExplicitAccesses and NodeTraits
  /// parameters.
  void resolveAccesses(const tsar::DFNode *LatchNode,
    const tsar::DFNode *ExitNode, const tsar::DefUseSet &DefUse,
    const tsar::LiveSet &LS, TraitMap &ExplicitAccesses,
    UnknownMap &ExplicitUnknowns, AliasMap &NodeTraits);

  /// Evaluates cases when location access is performed by pointer in a loop.
  void resolvePointers(const tsar::DefUseSet &DefUse,
    TraitMap &ExplicitAccesses);

  /// \brief Recognizes addresses of locations which is evaluated in a loop a
  /// for which need to pay attention during loop transformation.
  ///
  /// In the following example the variable X can be privatized, but address
  /// of the original variable X should be available after transformation.
  /// \code
  /// int X;
  /// for (...)
  ///   ... = &X;
  /// ..X = ...;
  /// \endcode
  void resolveAddresses(tsar::DFLoop *L, const tsar::DefUseSet &DefUse,
    TraitMap &ExplicitAccesses, AliasMap &NodeTraits);

  /// \brief Propagates traits of explicitly accessed locations to all nodes of
  /// an alias tree.
  ///
  /// Note that analysis which is performed for base locations is not the same
  /// as the analysis which is performed for variables from a source code.
  /// For example, the base location for (short&)X is a memory location with
  /// a size equal to the size_of(short), regardless the size of X which might
  /// have type int. Be careful when results of this analysis are propagated
  /// for variables from a source code.
  /// for (...) { (short&X) = ... ;} ... = X;
  /// The short part of X will be recognized as last private, but the whole
  /// variable X must be also set to first private to preserve the value
  /// obtained before the loop. This method propagates trait to all estimate
  /// locations and alias nodes in a hierarchy.
  /// \post Traits will be stored into a dependency set `DS`,
  /// `ExplicitAccesses`, `ExplicitUnknowns` and `NodeTraits` will be corrupted
  /// and can no longer be used.
  /// \param [in] Numbers This contains numbers of all alias nodes.
  /// \param [in] R Region in a data-flow graph, it can not be null.
  /// \param [in, out] ExplicitAccesses List of estimate memory locations and
  /// their traits which is explicitly accessed in a loop.
  /// \param [in, out] ExplicitUnknowns List of unknown memory locations and
  /// their traits which is explicitly accessed in a loop.
  /// \param [in, out] NodeTraits Utility list of traits for all nodes in
  /// the alias tree.
  /// \param [out] DS Representation of traits of a currently evaluated loop.
  void propagateTraits(
    const tsar::GraphNumbering<const tsar::AliasNode *> &Numbers,
    const tsar::DFRegion &R,
    TraitMap &ExplicitAccesses, UnknownMap &ExplicitUnknowns,
    AliasMap &NodeTraits, tsar::DependencySet &DS);

  /// \brief This removes redundant traits from a list.
  ///
  /// A trait of an estimate memory is redundant if there are some other
  /// estimate memory in a list which cover or equal the first one. This method
  /// removes redundant traits which are produced by an estimate memory from
  /// CurrItr. This method also finds for each estimate memory location, which
  /// stored in alias node N, the largest estimate location from `N` which
  /// covers it. All parameters of this method (except `N)` are in-out.
  void removeRedundant(const tsar::AliasNode *N, TraitList &Traits,
      TraitList::iterator &BeforeCurrItr, TraitList::iterator &CurrItr);

  /// \brief Checks whether an estimate memory location should be first private
  /// and stores appropriate traits if necessary.
  ///
  /// This checks whether whole base location will be written in the loop.
  /// Let us propose the following explanation. Consider a loop
  /// where some location Loc is written and this memory is going to be read
  /// after the program exit from this loop. It is possible that the
  /// estimate memory for this location covers this location, so not a whole
  /// memory that comprises estimate memory is written in the loop.
  /// To avoid a loss of data stored before the loop execution in a part of
  /// memory which is not written after copy out from this loop the
  /// estimate memory location must be also set as a first private.
  /// \param [in] Numbers This contains numbers of all alias nodes.
  /// \param [in] R Region in a data-flow graph, it can not be null.
  /// \param [in,out] TraitItr Traits of a location, it will be updated
  /// if necessary.
  /// \param [in,out] Dptr Traits of a location from TraitItr, it will be
  /// updated if necessary.
  void checkFirstPrivate(
    const tsar::GraphNumbering<const tsar::AliasNode *> &Numbers,
    const tsar::DFRegion &R,
    const TraitList::iterator &TraitItr, tsar::DependencyDescriptor &Dptr);

  /// \brief This store results of analysis of a loop into a dependency set.
  ///
  /// First private locations will be also explored and `Traits` will be updated
  /// if necessary.
  /// \param [in] Numbers This contains numbers of all alias nodes.
  /// \param [in] R Region in a data-flow graph, it can not be null.
  /// \param [in] N An alias node which has been analyzed.
  /// \param [in] ExplicitAccesses List of estimate memory locations and
  /// their traits which is explicitly accessed in a loop.
  /// \param [in] ExplicitUnknowns List of unknown memory locations and
  /// their traits which is explicitly accessed in a loop.
  /// \param [in, out] TraitPair Internal representation of traits of estimate
  /// memory from alias node 'N' which are explicitly accessed in a loop.
  /// This traits will be safely combined and trait for the whole node 'N' will
  /// be obtained.
  /// \param [out] DS Dependency set which stores results for a loop which
  /// is currently evaluated.
  void storeResults(
     const tsar::GraphNumbering<const tsar::AliasNode *> &Numbers,
     const tsar::DFRegion &R, const tsar::AliasNode &N,
     const TraitMap &ExplicitAccesses, const UnknownMap &ExplicitUnknowns,
     const TraitPair &Traits, tsar::DependencySet &DS);

  /// \brief Converts internal representation of a trait to a dependency
  /// descriptor.
  ///
  /// This method also calculates statistic which proposes number of determined
  /// traits. If different locations has the same traits TraitNumber parameter
  /// can be used to take into account all traits. It also can be set to 0
  /// if a specified trait should not be counted.
  tsar::DependencyDescriptor toDescriptor(const tsar::detail::TraitImp &T,
    unsigned TraitNumber);

private:
  tsar::PrivateInfo mPrivates;
  const tsar::DefinedMemoryInfo *mDefInfo = nullptr;
  const tsar::LiveMemoryInfo *mLiveInfo = nullptr;
  const tsar::AliasTree *mAliasTree = nullptr;
};
}
#endif//TSAR_PRIVATE_H
