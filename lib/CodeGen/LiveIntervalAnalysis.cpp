//===-- LiveIntervalAnalysis.cpp - Live Interval Analysis -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiveInterval analysis pass which is used
// by the Linear Scan Register allocator. This pass linearizes the
// basic blocks of the function in DFS order and uses the
// LiveVariables pass to conservatively compute live intervals for
// each virtual and physical register.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "regalloc"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/Value.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <limits>
#include <cmath>
using namespace llvm;

// Hidden options for help debugging.
static cl::opt<bool> DisableReMat("disable-rematerialization",
                                  cl::init(false), cl::Hidden);

STATISTIC(numIntervals , "Number of original intervals");

char LiveIntervals::ID = 0;
INITIALIZE_PASS_BEGIN(LiveIntervals, "liveintervals",
                "Live Interval Analysis", false, false)
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
INITIALIZE_PASS_DEPENDENCY(LiveVariables)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(SlotIndexes)
INITIALIZE_PASS_END(LiveIntervals, "liveintervals",
                "Live Interval Analysis", false, false)

void LiveIntervals::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AliasAnalysis>();
  AU.addPreserved<AliasAnalysis>();
  AU.addRequired<LiveVariables>();
  AU.addPreserved<LiveVariables>();
  AU.addPreservedID(MachineLoopInfoID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addPreserved<SlotIndexes>();
  AU.addRequiredTransitive<SlotIndexes>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

void LiveIntervals::releaseMemory() {
  // Free the live intervals themselves.
  for (DenseMap<unsigned, LiveInterval*>::iterator I = r2iMap_.begin(),
       E = r2iMap_.end(); I != E; ++I)
    delete I->second;

  r2iMap_.clear();
  RegMaskSlots.clear();
  RegMaskBits.clear();
  RegMaskBlocks.clear();

  // Release VNInfo memory regions, VNInfo objects don't need to be dtor'd.
  VNInfoAllocator.Reset();
}

/// runOnMachineFunction - Register allocate the whole function
///
bool LiveIntervals::runOnMachineFunction(MachineFunction &fn) {
  mf_ = &fn;
  mri_ = &mf_->getRegInfo();
  tm_ = &fn.getTarget();
  tri_ = tm_->getRegisterInfo();
  tii_ = tm_->getInstrInfo();
  aa_ = &getAnalysis<AliasAnalysis>();
  lv_ = &getAnalysis<LiveVariables>();
  indexes_ = &getAnalysis<SlotIndexes>();
  allocatableRegs_ = tri_->getAllocatableSet(fn);
  reservedRegs_ = tri_->getReservedRegs(fn);

  computeIntervals();

  numIntervals += getNumIntervals();

  DEBUG(dump());
  return true;
}

/// print - Implement the dump method.
void LiveIntervals::print(raw_ostream &OS, const Module* ) const {
  OS << "********** INTERVALS **********\n";

  // Dump the physregs.
  for (unsigned Reg = 1, RegE = tri_->getNumRegs(); Reg != RegE; ++Reg)
    if (const LiveInterval *LI = r2iMap_.lookup(Reg)) {
      LI->print(OS, tri_);
      OS << '\n';
    }

  // Dump the virtregs.
  for (unsigned Reg = 0, RegE = mri_->getNumVirtRegs(); Reg != RegE; ++Reg)
    if (const LiveInterval *LI =
        r2iMap_.lookup(TargetRegisterInfo::index2VirtReg(Reg))) {
      LI->print(OS, tri_);
      OS << '\n';
    }

  printInstrs(OS);
}

void LiveIntervals::printInstrs(raw_ostream &OS) const {
  OS << "********** MACHINEINSTRS **********\n";
  mf_->print(OS, indexes_);
}

void LiveIntervals::dumpInstrs() const {
  printInstrs(dbgs());
}

static
bool MultipleDefsBySameMI(const MachineInstr &MI, unsigned MOIdx) {
  unsigned Reg = MI.getOperand(MOIdx).getReg();
  for (unsigned i = MOIdx+1, e = MI.getNumOperands(); i < e; ++i) {
    const MachineOperand &MO = MI.getOperand(i);
    if (!MO.isReg())
      continue;
    if (MO.getReg() == Reg && MO.isDef()) {
      assert(MI.getOperand(MOIdx).getSubReg() != MO.getSubReg() &&
             MI.getOperand(MOIdx).getSubReg() &&
             (MO.getSubReg() || MO.isImplicit()));
      return true;
    }
  }
  return false;
}

/// isPartialRedef - Return true if the specified def at the specific index is
/// partially re-defining the specified live interval. A common case of this is
/// a definition of the sub-register.
bool LiveIntervals::isPartialRedef(SlotIndex MIIdx, MachineOperand &MO,
                                   LiveInterval &interval) {
  if (!MO.getSubReg() || MO.isEarlyClobber())
    return false;

  SlotIndex RedefIndex = MIIdx.getRegSlot();
  const LiveRange *OldLR =
    interval.getLiveRangeContaining(RedefIndex.getRegSlot(true));
  MachineInstr *DefMI = getInstructionFromIndex(OldLR->valno->def);
  if (DefMI != 0) {
    return DefMI->findRegisterDefOperandIdx(interval.reg) != -1;
  }
  return false;
}

void LiveIntervals::handleVirtualRegisterDef(MachineBasicBlock *mbb,
                                             MachineBasicBlock::iterator mi,
                                             SlotIndex MIIdx,
                                             MachineOperand& MO,
                                             unsigned MOIdx,
                                             LiveInterval &interval) {
  DEBUG(dbgs() << "\t\tregister: " << PrintReg(interval.reg, tri_));

  // Virtual registers may be defined multiple times (due to phi
  // elimination and 2-addr elimination).  Much of what we do only has to be
  // done once for the vreg.  We use an empty interval to detect the first
  // time we see a vreg.
  LiveVariables::VarInfo& vi = lv_->getVarInfo(interval.reg);
  if (interval.empty()) {
    // Get the Idx of the defining instructions.
    SlotIndex defIndex = MIIdx.getRegSlot(MO.isEarlyClobber());

    // Make sure the first definition is not a partial redefinition. Add an
    // <imp-def> of the full register.
    // FIXME: LiveIntervals shouldn't modify the code like this.  Whoever
    // created the machine instruction should annotate it with <undef> flags
    // as needed.  Then we can simply assert here.  The REG_SEQUENCE lowering
    // is the main suspect.
    if (MO.getSubReg()) {
      mi->addRegisterDefined(interval.reg);
      // Mark all defs of interval.reg on this instruction as reading <undef>.
      for (unsigned i = MOIdx, e = mi->getNumOperands(); i != e; ++i) {
        MachineOperand &MO2 = mi->getOperand(i);
        if (MO2.isReg() && MO2.getReg() == interval.reg && MO2.getSubReg())
          MO2.setIsUndef();
      }
    }

    VNInfo *ValNo = interval.getNextValue(defIndex, VNInfoAllocator);
    assert(ValNo->id == 0 && "First value in interval is not 0?");

    // Loop over all of the blocks that the vreg is defined in.  There are
    // two cases we have to handle here.  The most common case is a vreg
    // whose lifetime is contained within a basic block.  In this case there
    // will be a single kill, in MBB, which comes after the definition.
    if (vi.Kills.size() == 1 && vi.Kills[0]->getParent() == mbb) {
      // FIXME: what about dead vars?
      SlotIndex killIdx;
      if (vi.Kills[0] != mi)
        killIdx = getInstructionIndex(vi.Kills[0]).getRegSlot();
      else
        killIdx = defIndex.getDeadSlot();

      // If the kill happens after the definition, we have an intra-block
      // live range.
      if (killIdx > defIndex) {
        assert(vi.AliveBlocks.empty() &&
               "Shouldn't be alive across any blocks!");
        LiveRange LR(defIndex, killIdx, ValNo);
        interval.addRange(LR);
        DEBUG(dbgs() << " +" << LR << "\n");
        return;
      }
    }

    // The other case we handle is when a virtual register lives to the end
    // of the defining block, potentially live across some blocks, then is
    // live into some number of blocks, but gets killed.  Start by adding a
    // range that goes from this definition to the end of the defining block.
    LiveRange NewLR(defIndex, getMBBEndIdx(mbb), ValNo);
    DEBUG(dbgs() << " +" << NewLR);
    interval.addRange(NewLR);

    bool PHIJoin = lv_->isPHIJoin(interval.reg);

    if (PHIJoin) {
      // A phi join register is killed at the end of the MBB and revived as a new
      // valno in the killing blocks.
      assert(vi.AliveBlocks.empty() && "Phi join can't pass through blocks");
      DEBUG(dbgs() << " phi-join");
      ValNo->setHasPHIKill(true);
    } else {
      // Iterate over all of the blocks that the variable is completely
      // live in, adding [insrtIndex(begin), instrIndex(end)+4) to the
      // live interval.
      for (SparseBitVector<>::iterator I = vi.AliveBlocks.begin(),
               E = vi.AliveBlocks.end(); I != E; ++I) {
        MachineBasicBlock *aliveBlock = mf_->getBlockNumbered(*I);
        LiveRange LR(getMBBStartIdx(aliveBlock), getMBBEndIdx(aliveBlock), ValNo);
        interval.addRange(LR);
        DEBUG(dbgs() << " +" << LR);
      }
    }

    // Finally, this virtual register is live from the start of any killing
    // block to the 'use' slot of the killing instruction.
    for (unsigned i = 0, e = vi.Kills.size(); i != e; ++i) {
      MachineInstr *Kill = vi.Kills[i];
      SlotIndex Start = getMBBStartIdx(Kill->getParent());
      SlotIndex killIdx = getInstructionIndex(Kill).getRegSlot();

      // Create interval with one of a NEW value number.  Note that this value
      // number isn't actually defined by an instruction, weird huh? :)
      if (PHIJoin) {
        assert(getInstructionFromIndex(Start) == 0 &&
               "PHI def index points at actual instruction.");
        ValNo = interval.getNextValue(Start, VNInfoAllocator);
        ValNo->setIsPHIDef(true);
      }
      LiveRange LR(Start, killIdx, ValNo);
      interval.addRange(LR);
      DEBUG(dbgs() << " +" << LR);
    }

  } else {
    if (MultipleDefsBySameMI(*mi, MOIdx))
      // Multiple defs of the same virtual register by the same instruction.
      // e.g. %reg1031:5<def>, %reg1031:6<def> = VLD1q16 %reg1024<kill>, ...
      // This is likely due to elimination of REG_SEQUENCE instructions. Return
      // here since there is nothing to do.
      return;

    // If this is the second time we see a virtual register definition, it
    // must be due to phi elimination or two addr elimination.  If this is
    // the result of two address elimination, then the vreg is one of the
    // def-and-use register operand.

    // It may also be partial redef like this:
    // 80  %reg1041:6<def> = VSHRNv4i16 %reg1034<kill>, 12, pred:14, pred:%reg0
    // 120 %reg1041:5<def> = VSHRNv4i16 %reg1039<kill>, 12, pred:14, pred:%reg0
    bool PartReDef = isPartialRedef(MIIdx, MO, interval);
    if (PartReDef || mi->isRegTiedToUseOperand(MOIdx)) {
      // If this is a two-address definition, then we have already processed
      // the live range.  The only problem is that we didn't realize there
      // are actually two values in the live interval.  Because of this we
      // need to take the LiveRegion that defines this register and split it
      // into two values.
      SlotIndex RedefIndex = MIIdx.getRegSlot(MO.isEarlyClobber());

      const LiveRange *OldLR =
        interval.getLiveRangeContaining(RedefIndex.getRegSlot(true));
      VNInfo *OldValNo = OldLR->valno;
      SlotIndex DefIndex = OldValNo->def.getRegSlot();

      // Delete the previous value, which should be short and continuous,
      // because the 2-addr copy must be in the same MBB as the redef.
      interval.removeRange(DefIndex, RedefIndex);

      // The new value number (#1) is defined by the instruction we claimed
      // defined value #0.
      VNInfo *ValNo = interval.createValueCopy(OldValNo, VNInfoAllocator);

      // Value#0 is now defined by the 2-addr instruction.
      OldValNo->def = RedefIndex;

      // Add the new live interval which replaces the range for the input copy.
      LiveRange LR(DefIndex, RedefIndex, ValNo);
      DEBUG(dbgs() << " replace range with " << LR);
      interval.addRange(LR);

      // If this redefinition is dead, we need to add a dummy unit live
      // range covering the def slot.
      if (MO.isDead())
        interval.addRange(LiveRange(RedefIndex, RedefIndex.getDeadSlot(),
                                    OldValNo));

      DEBUG({
          dbgs() << " RESULT: ";
          interval.print(dbgs(), tri_);
        });
    } else if (lv_->isPHIJoin(interval.reg)) {
      // In the case of PHI elimination, each variable definition is only
      // live until the end of the block.  We've already taken care of the
      // rest of the live range.

      SlotIndex defIndex = MIIdx.getRegSlot();
      if (MO.isEarlyClobber())
        defIndex = MIIdx.getRegSlot(true);

      VNInfo *ValNo = interval.getNextValue(defIndex, VNInfoAllocator);

      SlotIndex killIndex = getMBBEndIdx(mbb);
      LiveRange LR(defIndex, killIndex, ValNo);
      interval.addRange(LR);
      ValNo->setHasPHIKill(true);
      DEBUG(dbgs() << " phi-join +" << LR);
    } else {
      llvm_unreachable("Multiply defined register");
    }
  }

  DEBUG(dbgs() << '\n');
}

#ifndef NDEBUG
static bool isRegLiveOutOf(const MachineBasicBlock *MBB, unsigned Reg) {
  for (MachineBasicBlock::const_succ_iterator SI = MBB->succ_begin(),
                                              SE = MBB->succ_end();
       SI != SE; ++SI) {
    const MachineBasicBlock* succ = *SI;
    if (succ->isLiveIn(Reg))
      return true;
  }
  return false;
}
#endif

void LiveIntervals::handlePhysicalRegisterDef(MachineBasicBlock *MBB,
                                              MachineBasicBlock::iterator mi,
                                              SlotIndex MIIdx,
                                              MachineOperand& MO,
                                              LiveInterval &interval) {
  DEBUG(dbgs() << "\t\tregister: " << PrintReg(interval.reg, tri_));

  SlotIndex baseIndex = MIIdx;
  SlotIndex start = baseIndex.getRegSlot(MO.isEarlyClobber());
  SlotIndex end = start;

  // If it is not used after definition, it is considered dead at
  // the instruction defining it. Hence its interval is:
  // [defSlot(def), defSlot(def)+1)
  // For earlyclobbers, the defSlot was pushed back one; the extra
  // advance below compensates.
  if (MO.isDead()) {
    DEBUG(dbgs() << " dead");
    end = start.getDeadSlot();
    goto exit;
  }

  // If it is not dead on definition, it must be killed by a
  // subsequent instruction. Hence its interval is:
  // [defSlot(def), useSlot(kill)+1)
  baseIndex = baseIndex.getNextIndex();
  while (++mi != MBB->end()) {

    if (mi->isDebugValue())
      continue;
    if (getInstructionFromIndex(baseIndex) == 0)
      baseIndex = indexes_->getNextNonNullIndex(baseIndex);

    if (mi->killsRegister(interval.reg, tri_)) {
      DEBUG(dbgs() << " killed");
      end = baseIndex.getRegSlot();
      goto exit;
    } else {
      int DefIdx = mi->findRegisterDefOperandIdx(interval.reg,false,false,tri_);
      if (DefIdx != -1) {
        if (mi->isRegTiedToUseOperand(DefIdx)) {
          // Two-address instruction.
          end = baseIndex.getRegSlot(mi->getOperand(DefIdx).isEarlyClobber());
        } else {
          // Another instruction redefines the register before it is ever read.
          // Then the register is essentially dead at the instruction that
          // defines it. Hence its interval is:
          // [defSlot(def), defSlot(def)+1)
          DEBUG(dbgs() << " dead");
          end = start.getDeadSlot();
        }
        goto exit;
      }
    }

    baseIndex = baseIndex.getNextIndex();
  }

  // If we get here the register *should* be live out.
  assert(!isAllocatable(interval.reg) && "Physregs shouldn't be live out!");

  // FIXME: We need saner rules for reserved regs.
  if (isReserved(interval.reg)) {
    end = start.getDeadSlot();
  } else {
    // Unreserved, unallocable registers like EFLAGS can be live across basic
    // block boundaries.
    assert(isRegLiveOutOf(MBB, interval.reg) && "Unreserved reg not live-out?");
    end = getMBBEndIdx(MBB);
  }
exit:
  assert(start < end && "did not find end of interval?");

  // Already exists? Extend old live interval.
  VNInfo *ValNo = interval.getVNInfoAt(start);
  bool Extend = ValNo != 0;
  if (!Extend)
    ValNo = interval.getNextValue(start, VNInfoAllocator);
  LiveRange LR(start, end, ValNo);
  interval.addRange(LR);
  DEBUG(dbgs() << " +" << LR << '\n');
}

void LiveIntervals::handleRegisterDef(MachineBasicBlock *MBB,
                                      MachineBasicBlock::iterator MI,
                                      SlotIndex MIIdx,
                                      MachineOperand& MO,
                                      unsigned MOIdx) {
  if (TargetRegisterInfo::isVirtualRegister(MO.getReg()))
    handleVirtualRegisterDef(MBB, MI, MIIdx, MO, MOIdx,
                             getOrCreateInterval(MO.getReg()));
  else
    handlePhysicalRegisterDef(MBB, MI, MIIdx, MO,
                              getOrCreateInterval(MO.getReg()));
}

void LiveIntervals::handleLiveInRegister(MachineBasicBlock *MBB,
                                         SlotIndex MIIdx,
                                         LiveInterval &interval) {
  assert(TargetRegisterInfo::isPhysicalRegister(interval.reg) &&
         "Only physical registers can be live in.");
  assert((!isAllocatable(interval.reg) || MBB->getParent()->begin() ||
          MBB->isLandingPad()) &&
          "Allocatable live-ins only valid for entry blocks and landing pads.");

  DEBUG(dbgs() << "\t\tlivein register: " << PrintReg(interval.reg, tri_));

  // Look for kills, if it reaches a def before it's killed, then it shouldn't
  // be considered a livein.
  MachineBasicBlock::iterator mi = MBB->begin();
  MachineBasicBlock::iterator E = MBB->end();
  // Skip over DBG_VALUE at the start of the MBB.
  if (mi != E && mi->isDebugValue()) {
    while (++mi != E && mi->isDebugValue())
      ;
    if (mi == E)
      // MBB is empty except for DBG_VALUE's.
      return;
  }

  SlotIndex baseIndex = MIIdx;
  SlotIndex start = baseIndex;
  if (getInstructionFromIndex(baseIndex) == 0)
    baseIndex = indexes_->getNextNonNullIndex(baseIndex);

  SlotIndex end = baseIndex;
  bool SeenDefUse = false;

  while (mi != E) {
    if (mi->killsRegister(interval.reg, tri_)) {
      DEBUG(dbgs() << " killed");
      end = baseIndex.getRegSlot();
      SeenDefUse = true;
      break;
    } else if (mi->modifiesRegister(interval.reg, tri_)) {
      // Another instruction redefines the register before it is ever read.
      // Then the register is essentially dead at the instruction that defines
      // it. Hence its interval is:
      // [defSlot(def), defSlot(def)+1)
      DEBUG(dbgs() << " dead");
      end = start.getDeadSlot();
      SeenDefUse = true;
      break;
    }

    while (++mi != E && mi->isDebugValue())
      // Skip over DBG_VALUE.
      ;
    if (mi != E)
      baseIndex = indexes_->getNextNonNullIndex(baseIndex);
  }

  // Live-in register might not be used at all.
  if (!SeenDefUse) {
    if (isAllocatable(interval.reg) || isReserved(interval.reg)) {
      // This must be an entry block or landing pad - we asserted so on entry
      // to the function. For these blocks the interval is dead on entry, so
      // we won't emit a live-range for it.
      DEBUG(dbgs() << " dead");
      return;
    } else {
      assert(isRegLiveOutOf(MBB, interval.reg) &&
             "Live in reg untouched in block should be be live through.");
      DEBUG(dbgs() << " live through");
      end = getMBBEndIdx(MBB);
    }
  }

  SlotIndex defIdx = getMBBStartIdx(MBB);
  assert(getInstructionFromIndex(defIdx) == 0 &&
         "PHI def index points at actual instruction.");
  VNInfo *vni = interval.getNextValue(defIdx, VNInfoAllocator);
  vni->setIsPHIDef(true);
  LiveRange LR(start, end, vni);

  interval.addRange(LR);
  DEBUG(dbgs() << " +" << LR << '\n');
}

/// computeIntervals - computes the live intervals for virtual
/// registers. for some ordering of the machine instructions [1,N] a
/// live interval is an interval [i, j) where 1 <= i <= j < N for
/// which a variable is live
void LiveIntervals::computeIntervals() {
  DEBUG(dbgs() << "********** COMPUTING LIVE INTERVALS **********\n"
               << "********** Function: "
               << ((Value*)mf_->getFunction())->getName() << '\n');

  RegMaskBlocks.resize(mf_->getNumBlockIDs());

  SmallVector<unsigned, 8> UndefUses;
  for (MachineFunction::iterator MBBI = mf_->begin(), E = mf_->end();
       MBBI != E; ++MBBI) {
    MachineBasicBlock *MBB = MBBI;
    RegMaskBlocks[MBB->getNumber()].first = RegMaskSlots.size();

    if (MBB->empty())
      continue;

    // Track the index of the current machine instr.
    SlotIndex MIIndex = getMBBStartIdx(MBB);
    DEBUG(dbgs() << "BB#" << MBB->getNumber()
          << ":\t\t# derived from " << MBB->getName() << "\n");

    // Create intervals for live-ins to this BB first.
    for (MachineBasicBlock::livein_iterator LI = MBB->livein_begin(),
           LE = MBB->livein_end(); LI != LE; ++LI) {
      handleLiveInRegister(MBB, MIIndex, getOrCreateInterval(*LI));
    }

    // Skip over empty initial indices.
    if (getInstructionFromIndex(MIIndex) == 0)
      MIIndex = indexes_->getNextNonNullIndex(MIIndex);

    for (MachineBasicBlock::iterator MI = MBB->begin(), miEnd = MBB->end();
         MI != miEnd; ++MI) {
      DEBUG(dbgs() << MIIndex << "\t" << *MI);
      if (MI->isDebugValue())
        continue;
      assert(indexes_->getInstructionFromIndex(MIIndex) == MI &&
             "Lost SlotIndex synchronization");

      // Handle defs.
      for (int i = MI->getNumOperands() - 1; i >= 0; --i) {
        MachineOperand &MO = MI->getOperand(i);

        // Collect register masks.
        if (MO.isRegMask()) {
          RegMaskSlots.push_back(MIIndex.getRegSlot());
          RegMaskBits.push_back(MO.getRegMask());
          continue;
        }

        if (!MO.isReg() || !MO.getReg())
          continue;

        // handle register defs - build intervals
        if (MO.isDef())
          handleRegisterDef(MBB, MI, MIIndex, MO, i);
        else if (MO.isUndef())
          UndefUses.push_back(MO.getReg());
      }

      // Move to the next instr slot.
      MIIndex = indexes_->getNextNonNullIndex(MIIndex);
    }

    // Compute the number of register mask instructions in this block.
    std::pair<unsigned, unsigned> &RMB = RegMaskBlocks[MBB->getNumber()];
    RMB.second = RegMaskSlots.size() - RMB.first;;
  }

  // Create empty intervals for registers defined by implicit_def's (except
  // for those implicit_def that define values which are liveout of their
  // blocks.
  for (unsigned i = 0, e = UndefUses.size(); i != e; ++i) {
    unsigned UndefReg = UndefUses[i];
    (void)getOrCreateInterval(UndefReg);
  }
}

LiveInterval* LiveIntervals::createInterval(unsigned reg) {
  float Weight = TargetRegisterInfo::isPhysicalRegister(reg) ? HUGE_VALF : 0.0F;
  return new LiveInterval(reg, Weight);
}

/// dupInterval - Duplicate a live interval. The caller is responsible for
/// managing the allocated memory.
LiveInterval* LiveIntervals::dupInterval(LiveInterval *li) {
  LiveInterval *NewLI = createInterval(li->reg);
  NewLI->Copy(*li, mri_, getVNInfoAllocator());
  return NewLI;
}

/// shrinkToUses - After removing some uses of a register, shrink its live
/// range to just the remaining uses. This method does not compute reaching
/// defs for new uses, and it doesn't remove dead defs.
bool LiveIntervals::shrinkToUses(LiveInterval *li,
                                 SmallVectorImpl<MachineInstr*> *dead) {
  DEBUG(dbgs() << "Shrink: " << *li << '\n');
  assert(TargetRegisterInfo::isVirtualRegister(li->reg)
         && "Can only shrink virtual registers");
  // Find all the values used, including PHI kills.
  SmallVector<std::pair<SlotIndex, VNInfo*>, 16> WorkList;

  // Blocks that have already been added to WorkList as live-out.
  SmallPtrSet<MachineBasicBlock*, 16> LiveOut;

  // Visit all instructions reading li->reg.
  for (MachineRegisterInfo::reg_iterator I = mri_->reg_begin(li->reg);
       MachineInstr *UseMI = I.skipInstruction();) {
    if (UseMI->isDebugValue() || !UseMI->readsVirtualRegister(li->reg))
      continue;
    SlotIndex Idx = getInstructionIndex(UseMI).getRegSlot();
    // Note: This intentionally picks up the wrong VNI in case of an EC redef.
    // See below.
    VNInfo *VNI = li->getVNInfoBefore(Idx);
    if (!VNI) {
      // This shouldn't happen: readsVirtualRegister returns true, but there is
      // no live value. It is likely caused by a target getting <undef> flags
      // wrong.
      DEBUG(dbgs() << Idx << '\t' << *UseMI
                   << "Warning: Instr claims to read non-existent value in "
                    << *li << '\n');
      continue;
    }
    // Special case: An early-clobber tied operand reads and writes the
    // register one slot early.  The getVNInfoBefore call above would have
    // picked up the value defined by UseMI.  Adjust the kill slot and value.
    if (SlotIndex::isSameInstr(VNI->def, Idx)) {
      Idx = VNI->def;
      VNI = li->getVNInfoBefore(Idx);
      assert(VNI && "Early-clobber tied value not available");
    }
    WorkList.push_back(std::make_pair(Idx, VNI));
  }

  // Create a new live interval with only minimal live segments per def.
  LiveInterval NewLI(li->reg, 0);
  for (LiveInterval::vni_iterator I = li->vni_begin(), E = li->vni_end();
       I != E; ++I) {
    VNInfo *VNI = *I;
    if (VNI->isUnused())
      continue;
    NewLI.addRange(LiveRange(VNI->def, VNI->def.getDeadSlot(), VNI));
  }

  // Keep track of the PHIs that are in use.
  SmallPtrSet<VNInfo*, 8> UsedPHIs;

  // Extend intervals to reach all uses in WorkList.
  while (!WorkList.empty()) {
    SlotIndex Idx = WorkList.back().first;
    VNInfo *VNI = WorkList.back().second;
    WorkList.pop_back();
    const MachineBasicBlock *MBB = getMBBFromIndex(Idx.getPrevSlot());
    SlotIndex BlockStart = getMBBStartIdx(MBB);

    // Extend the live range for VNI to be live at Idx.
    if (VNInfo *ExtVNI = NewLI.extendInBlock(BlockStart, Idx)) {
      (void)ExtVNI;
      assert(ExtVNI == VNI && "Unexpected existing value number");
      // Is this a PHIDef we haven't seen before?
      if (!VNI->isPHIDef() || VNI->def != BlockStart || !UsedPHIs.insert(VNI))
        continue;
      // The PHI is live, make sure the predecessors are live-out.
      for (MachineBasicBlock::const_pred_iterator PI = MBB->pred_begin(),
           PE = MBB->pred_end(); PI != PE; ++PI) {
        if (!LiveOut.insert(*PI))
          continue;
        SlotIndex Stop = getMBBEndIdx(*PI);
        // A predecessor is not required to have a live-out value for a PHI.
        if (VNInfo *PVNI = li->getVNInfoBefore(Stop))
          WorkList.push_back(std::make_pair(Stop, PVNI));
      }
      continue;
    }

    // VNI is live-in to MBB.
    DEBUG(dbgs() << " live-in at " << BlockStart << '\n');
    NewLI.addRange(LiveRange(BlockStart, Idx, VNI));

    // Make sure VNI is live-out from the predecessors.
    for (MachineBasicBlock::const_pred_iterator PI = MBB->pred_begin(),
         PE = MBB->pred_end(); PI != PE; ++PI) {
      if (!LiveOut.insert(*PI))
        continue;
      SlotIndex Stop = getMBBEndIdx(*PI);
      assert(li->getVNInfoBefore(Stop) == VNI &&
             "Wrong value out of predecessor");
      WorkList.push_back(std::make_pair(Stop, VNI));
    }
  }

  // Handle dead values.
  bool CanSeparate = false;
  for (LiveInterval::vni_iterator I = li->vni_begin(), E = li->vni_end();
       I != E; ++I) {
    VNInfo *VNI = *I;
    if (VNI->isUnused())
      continue;
    LiveInterval::iterator LII = NewLI.FindLiveRangeContaining(VNI->def);
    assert(LII != NewLI.end() && "Missing live range for PHI");
    if (LII->end != VNI->def.getDeadSlot())
      continue;
    if (VNI->isPHIDef()) {
      // This is a dead PHI. Remove it.
      VNI->setIsUnused(true);
      NewLI.removeRange(*LII);
      DEBUG(dbgs() << "Dead PHI at " << VNI->def << " may separate interval\n");
      CanSeparate = true;
    } else {
      // This is a dead def. Make sure the instruction knows.
      MachineInstr *MI = getInstructionFromIndex(VNI->def);
      assert(MI && "No instruction defining live value");
      MI->addRegisterDead(li->reg, tri_);
      if (dead && MI->allDefsAreDead()) {
        DEBUG(dbgs() << "All defs dead: " << VNI->def << '\t' << *MI);
        dead->push_back(MI);
      }
    }
  }

  // Move the trimmed ranges back.
  li->ranges.swap(NewLI.ranges);
  DEBUG(dbgs() << "Shrunk: " << *li << '\n');
  return CanSeparate;
}


//===----------------------------------------------------------------------===//
// Register allocator hooks.
//

void LiveIntervals::addKillFlags() {
  for (iterator I = begin(), E = end(); I != E; ++I) {
    unsigned Reg = I->first;
    if (TargetRegisterInfo::isPhysicalRegister(Reg))
      continue;
    if (mri_->reg_nodbg_empty(Reg))
      continue;
    LiveInterval *LI = I->second;

    // Every instruction that kills Reg corresponds to a live range end point.
    for (LiveInterval::iterator RI = LI->begin(), RE = LI->end(); RI != RE;
         ++RI) {
      // A block index indicates an MBB edge.
      if (RI->end.isBlock())
        continue;
      MachineInstr *MI = getInstructionFromIndex(RI->end);
      if (!MI)
        continue;
      MI->addRegisterKilled(Reg, NULL);
    }
  }
}

#ifndef NDEBUG
static bool intervalRangesSane(const LiveInterval& li) {
  if (li.empty()) {
    return true;
  }

  SlotIndex lastEnd = li.begin()->start;
  for (LiveInterval::const_iterator lrItr = li.begin(), lrEnd = li.end();
       lrItr != lrEnd; ++lrItr) {
    const LiveRange& lr = *lrItr;
    if (lastEnd > lr.start || lr.start >= lr.end)
      return false;
    lastEnd = lr.end;
  }

  return true;
}
#endif

template <typename DefSetT>
static void handleMoveDefs(LiveIntervals& lis, SlotIndex origIdx,
                           SlotIndex miIdx, const DefSetT& defs) {
  for (typename DefSetT::const_iterator defItr = defs.begin(),
                                        defEnd = defs.end();
       defItr != defEnd; ++defItr) {
    unsigned def = *defItr;
    LiveInterval& li = lis.getInterval(def);
    LiveRange* lr = li.getLiveRangeContaining(origIdx.getRegSlot());
    assert(lr != 0 && "No range for def?");
    lr->start = miIdx.getRegSlot();
    lr->valno->def = miIdx.getRegSlot();
    assert(intervalRangesSane(li) && "Broke live interval moving def.");
  }
}

template <typename DeadDefSetT>
static void handleMoveDeadDefs(LiveIntervals& lis, SlotIndex origIdx,
                               SlotIndex miIdx, const DeadDefSetT& deadDefs) {
  for (typename DeadDefSetT::const_iterator deadDefItr = deadDefs.begin(),
                                            deadDefEnd = deadDefs.end();
       deadDefItr != deadDefEnd; ++deadDefItr) {
    unsigned deadDef = *deadDefItr;
    LiveInterval& li = lis.getInterval(deadDef);
    LiveRange* lr = li.getLiveRangeContaining(origIdx.getRegSlot());
    assert(lr != 0 && "No range for dead def?");
    assert(lr->start == origIdx.getRegSlot() && "Bad dead range start?");
    assert(lr->end == origIdx.getDeadSlot() && "Bad dead range end?");
    assert(lr->valno->def == origIdx.getRegSlot() && "Bad dead valno def.");
    LiveRange t(*lr);
    t.start = miIdx.getRegSlot();
    t.valno->def = miIdx.getRegSlot();
    t.end = miIdx.getDeadSlot();
    li.removeRange(*lr);
    li.addRange(t);
    assert(intervalRangesSane(li) && "Broke live interval moving dead def.");
  }
}

template <typename ECSetT>
static void handleMoveECs(LiveIntervals& lis, SlotIndex origIdx,
                          SlotIndex miIdx, const ECSetT& ecs) {
  for (typename ECSetT::const_iterator ecItr = ecs.begin(), ecEnd = ecs.end();
       ecItr != ecEnd; ++ecItr) {
    unsigned ec = *ecItr;
    LiveInterval& li = lis.getInterval(ec);
    LiveRange* lr = li.getLiveRangeContaining(origIdx.getRegSlot(true));
    assert(lr != 0 && "No range for early clobber?");
    assert(lr->start == origIdx.getRegSlot(true) && "Bad EC range start?");
    assert(lr->end == origIdx.getRegSlot() && "Bad EC range end.");
    assert(lr->valno->def == origIdx.getRegSlot(true) && "Bad EC valno def.");
    LiveRange t(*lr);
    t.start = miIdx.getRegSlot(true);
    t.valno->def = miIdx.getRegSlot(true);
    t.end = miIdx.getRegSlot();
    li.removeRange(*lr);
    li.addRange(t);
    assert(intervalRangesSane(li) && "Broke live interval moving EC.");
  }
}

static void moveKillFlags(unsigned reg, SlotIndex oldIdx, SlotIndex newIdx,
                          LiveIntervals& lis,
                          const TargetRegisterInfo& tri) {
  MachineInstr* oldKillMI = lis.getInstructionFromIndex(oldIdx);
  MachineInstr* newKillMI = lis.getInstructionFromIndex(newIdx);
  assert(oldKillMI->killsRegister(reg) && "Old 'kill' instr isn't a kill.");
  assert(!newKillMI->killsRegister(reg) && "New kill instr is already a kill.");
  oldKillMI->clearRegisterKills(reg, &tri);
  newKillMI->addRegisterKilled(reg, &tri);
}

template <typename UseSetT>
static void handleMoveUses(const MachineBasicBlock *mbb,
                           const MachineRegisterInfo& mri,
                           const TargetRegisterInfo& tri,
                           const BitVector& reservedRegs, LiveIntervals &lis,
                           SlotIndex origIdx, SlotIndex miIdx,
                           const UseSetT &uses) {
  bool movingUp = miIdx < origIdx;
  for (typename UseSetT::const_iterator usesItr = uses.begin(),
                                        usesEnd = uses.end();
       usesItr != usesEnd; ++usesItr) {
    unsigned use = *usesItr;
    if (!lis.hasInterval(use))
      continue;
    if (TargetRegisterInfo::isPhysicalRegister(use) && reservedRegs.test(use))
      continue;
    LiveInterval& li = lis.getInterval(use);
    LiveRange* lr = li.getLiveRangeBefore(origIdx.getRegSlot());
    assert(lr != 0 && "No range for use?");
    bool liveThrough = lr->end > origIdx.getRegSlot();

    if (movingUp) {
      // If moving up and liveThrough - nothing to do.
      // If not live through we need to extend the range to the last use
      // between the old location and the new one.
      if (!liveThrough) {
        SlotIndex lastUseInRange = miIdx.getRegSlot();
        for (MachineRegisterInfo::use_iterator useI = mri.use_begin(use),
                                               useE = mri.use_end();
             useI != useE; ++useI) {
          const MachineInstr* mopI = &*useI;
          const MachineOperand& mop = useI.getOperand();
          SlotIndex instSlot = lis.getSlotIndexes()->getInstructionIndex(mopI);
          SlotIndex opSlot = instSlot.getRegSlot(mop.isEarlyClobber());
          if (opSlot > lastUseInRange && opSlot < origIdx)
            lastUseInRange = opSlot;
        }

        // If we found a new instr endpoint update the kill flags.
        if (lastUseInRange != miIdx.getRegSlot())
          moveKillFlags(use, miIdx, lastUseInRange, lis, tri);

        // Fix up the range end.
        lr->end = lastUseInRange;
      }
    } else {
      // Moving down is easy - the existing live range end tells us where
      // the last kill is.
      if (!liveThrough) {
        // Easy fix - just update the range endpoint.
        lr->end = miIdx.getRegSlot();
      } else {
        bool liveOut = lr->end >= lis.getSlotIndexes()->getMBBEndIdx(mbb);
        if (!liveOut && miIdx.getRegSlot() > lr->end) {
          moveKillFlags(use, lr->end, miIdx, lis, tri);
          lr->end = miIdx.getRegSlot();
        }
      }
    }
    assert(intervalRangesSane(li) && "Broke live interval moving use.");
  }
}



void LiveIntervals::handleMove(MachineInstr* mi) {
  SlotIndex origIdx = indexes_->getInstructionIndex(mi);
  indexes_->removeMachineInstrFromMaps(mi);
  SlotIndex miIdx = mi->isInsideBundle() ?
                     indexes_->getInstructionIndex(mi->getBundleStart()) :
                     indexes_->insertMachineInstrInMaps(mi);
  MachineBasicBlock* mbb = mi->getParent();
  assert(getMBBStartIdx(mbb) <= origIdx && origIdx < getMBBEndIdx(mbb) &&
         "Cannot handle moves across basic block boundaries.");
  assert(!mi->isBundled() && "Can't handle bundled instructions yet.");

  // Pick the direction.
  bool movingUp = miIdx < origIdx;

  // Collect the operands.
  DenseSet<unsigned> uses, defs, deadDefs, ecs;
  for (MachineInstr::mop_iterator mopItr = mi->operands_begin(),
         mopEnd = mi->operands_end();
       mopItr != mopEnd; ++mopItr) {
    const MachineOperand& mop = *mopItr;

    if (!mop.isReg() || mop.getReg() == 0)
      continue;
    unsigned reg = mop.getReg();

    if (mop.readsReg() && !ecs.count(reg)) {
      uses.insert(reg);
    }
    if (mop.isDef()) {
      if (mop.isDead()) {
        assert(!defs.count(reg) && "Can't mix defs with dead-defs.");
        deadDefs.insert(reg);
      } else if (mop.isEarlyClobber()) {
        uses.erase(reg);
        ecs.insert(reg);
      } else {
        assert(!deadDefs.count(reg) && "Can't mix defs with dead-defs.");
        defs.insert(reg);
      }
    }
  }

  if (movingUp) {
    handleMoveUses(mbb, *mri_, *tri_, reservedRegs_, *this, origIdx, miIdx, uses);
    handleMoveECs(*this, origIdx, miIdx, ecs);
    handleMoveDeadDefs(*this, origIdx, miIdx, deadDefs);
    handleMoveDefs(*this, origIdx, miIdx, defs);
  } else {
    handleMoveDefs(*this, origIdx, miIdx, defs);
    handleMoveDeadDefs(*this, origIdx, miIdx, deadDefs);
    handleMoveECs(*this, origIdx, miIdx, ecs);
    handleMoveUses(mbb, *mri_, *tri_, reservedRegs_, *this, origIdx, miIdx, uses);
  }
}

/// getReMatImplicitUse - If the remat definition MI has one (for now, we only
/// allow one) virtual register operand, then its uses are implicitly using
/// the register. Returns the virtual register.
unsigned LiveIntervals::getReMatImplicitUse(const LiveInterval &li,
                                            MachineInstr *MI) const {
  unsigned RegOp = 0;
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    MachineOperand &MO = MI->getOperand(i);
    if (!MO.isReg() || !MO.isUse())
      continue;
    unsigned Reg = MO.getReg();
    if (Reg == 0 || Reg == li.reg)
      continue;

    if (TargetRegisterInfo::isPhysicalRegister(Reg) && !isAllocatable(Reg))
      continue;
    RegOp = MO.getReg();
    break; // Found vreg operand - leave the loop.
  }
  return RegOp;
}

/// isValNoAvailableAt - Return true if the val# of the specified interval
/// which reaches the given instruction also reaches the specified use index.
bool LiveIntervals::isValNoAvailableAt(const LiveInterval &li, MachineInstr *MI,
                                       SlotIndex UseIdx) const {
  VNInfo *UValNo = li.getVNInfoAt(UseIdx);
  return UValNo && UValNo == li.getVNInfoAt(getInstructionIndex(MI));
}

/// isReMaterializable - Returns true if the definition MI of the specified
/// val# of the specified interval is re-materializable.
bool
LiveIntervals::isReMaterializable(const LiveInterval &li,
                                  const VNInfo *ValNo, MachineInstr *MI,
                                  const SmallVectorImpl<LiveInterval*> *SpillIs,
                                  bool &isLoad) {
  if (DisableReMat)
    return false;

  if (!tii_->isTriviallyReMaterializable(MI, aa_))
    return false;

  // Target-specific code can mark an instruction as being rematerializable
  // if it has one virtual reg use, though it had better be something like
  // a PIC base register which is likely to be live everywhere.
  unsigned ImpUse = getReMatImplicitUse(li, MI);
  if (ImpUse) {
    const LiveInterval &ImpLi = getInterval(ImpUse);
    for (MachineRegisterInfo::use_nodbg_iterator
           ri = mri_->use_nodbg_begin(li.reg), re = mri_->use_nodbg_end();
         ri != re; ++ri) {
      MachineInstr *UseMI = &*ri;
      SlotIndex UseIdx = getInstructionIndex(UseMI);
      if (li.getVNInfoAt(UseIdx) != ValNo)
        continue;
      if (!isValNoAvailableAt(ImpLi, MI, UseIdx))
        return false;
    }

    // If a register operand of the re-materialized instruction is going to
    // be spilled next, then it's not legal to re-materialize this instruction.
    if (SpillIs)
      for (unsigned i = 0, e = SpillIs->size(); i != e; ++i)
        if (ImpUse == (*SpillIs)[i]->reg)
          return false;
  }
  return true;
}

/// isReMaterializable - Returns true if every definition of MI of every
/// val# of the specified interval is re-materializable.
bool
LiveIntervals::isReMaterializable(const LiveInterval &li,
                                  const SmallVectorImpl<LiveInterval*> *SpillIs,
                                  bool &isLoad) {
  isLoad = false;
  for (LiveInterval::const_vni_iterator i = li.vni_begin(), e = li.vni_end();
       i != e; ++i) {
    const VNInfo *VNI = *i;
    if (VNI->isUnused())
      continue; // Dead val#.
    // Is the def for the val# rematerializable?
    MachineInstr *ReMatDefMI = getInstructionFromIndex(VNI->def);
    if (!ReMatDefMI)
      return false;
    bool DefIsLoad = false;
    if (!ReMatDefMI ||
        !isReMaterializable(li, VNI, ReMatDefMI, SpillIs, DefIsLoad))
      return false;
    isLoad |= DefIsLoad;
  }
  return true;
}

MachineBasicBlock*
LiveIntervals::intervalIsInOneMBB(const LiveInterval &LI) const {
  // A local live range must be fully contained inside the block, meaning it is
  // defined and killed at instructions, not at block boundaries. It is not
  // live in or or out of any block.
  //
  // It is technically possible to have a PHI-defined live range identical to a
  // single block, but we are going to return false in that case.

  SlotIndex Start = LI.beginIndex();
  if (Start.isBlock())
    return NULL;

  SlotIndex Stop = LI.endIndex();
  if (Stop.isBlock())
    return NULL;

  // getMBBFromIndex doesn't need to search the MBB table when both indexes
  // belong to proper instructions.
  MachineBasicBlock *MBB1 = indexes_->getMBBFromIndex(Start);
  MachineBasicBlock *MBB2 = indexes_->getMBBFromIndex(Stop);
  return MBB1 == MBB2 ? MBB1 : NULL;
}

float
LiveIntervals::getSpillWeight(bool isDef, bool isUse, unsigned loopDepth) {
  // Limit the loop depth ridiculousness.
  if (loopDepth > 200)
    loopDepth = 200;

  // The loop depth is used to roughly estimate the number of times the
  // instruction is executed. Something like 10^d is simple, but will quickly
  // overflow a float. This expression behaves like 10^d for small d, but is
  // more tempered for large d. At d=200 we get 6.7e33 which leaves a bit of
  // headroom before overflow.
  // By the way, powf() might be unavailable here. For consistency,
  // We may take pow(double,double).
  float lc = std::pow(1 + (100.0 / (loopDepth + 10)), (double)loopDepth);

  return (isDef + isUse) * lc;
}

LiveRange LiveIntervals::addLiveRangeToEndOfBlock(unsigned reg,
                                                  MachineInstr* startInst) {
  LiveInterval& Interval = getOrCreateInterval(reg);
  VNInfo* VN = Interval.getNextValue(
    SlotIndex(getInstructionIndex(startInst).getRegSlot()),
    getVNInfoAllocator());
  VN->setHasPHIKill(true);
  LiveRange LR(
     SlotIndex(getInstructionIndex(startInst).getRegSlot()),
     getMBBEndIdx(startInst->getParent()), VN);
  Interval.addRange(LR);

  return LR;
}


//===----------------------------------------------------------------------===//
//                          Register mask functions
//===----------------------------------------------------------------------===//

bool LiveIntervals::checkRegMaskInterference(LiveInterval &LI,
                                             BitVector &UsableRegs) {
  if (LI.empty())
    return false;
  LiveInterval::iterator LiveI = LI.begin(), LiveE = LI.end();

  // Use a smaller arrays for local live ranges.
  ArrayRef<SlotIndex> Slots;
  ArrayRef<const uint32_t*> Bits;
  if (MachineBasicBlock *MBB = intervalIsInOneMBB(LI)) {
    Slots = getRegMaskSlotsInBlock(MBB->getNumber());
    Bits = getRegMaskBitsInBlock(MBB->getNumber());
  } else {
    Slots = getRegMaskSlots();
    Bits = getRegMaskBits();
  }

  // We are going to enumerate all the register mask slots contained in LI.
  // Start with a binary search of RegMaskSlots to find a starting point.
  ArrayRef<SlotIndex>::iterator SlotI =
    std::lower_bound(Slots.begin(), Slots.end(), LiveI->start);
  ArrayRef<SlotIndex>::iterator SlotE = Slots.end();

  // No slots in range, LI begins after the last call.
  if (SlotI == SlotE)
    return false;

  bool Found = false;
  for (;;) {
    assert(*SlotI >= LiveI->start);
    // Loop over all slots overlapping this segment.
    while (*SlotI < LiveI->end) {
      // *SlotI overlaps LI. Collect mask bits.
      if (!Found) {
        // This is the first overlap. Initialize UsableRegs to all ones.
        UsableRegs.clear();
        UsableRegs.resize(tri_->getNumRegs(), true);
        Found = true;
      }
      // Remove usable registers clobbered by this mask.
      UsableRegs.clearBitsNotInMask(Bits[SlotI-Slots.begin()]);
      if (++SlotI == SlotE)
        return Found;
    }
    // *SlotI is beyond the current LI segment.
    LiveI = LI.advanceTo(LiveI, *SlotI);
    if (LiveI == LiveE)
      return Found;
    // Advance SlotI until it overlaps.
    while (*SlotI < LiveI->start)
      if (++SlotI == SlotE)
        return Found;
  }
}
