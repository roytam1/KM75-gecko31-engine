/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/CodeGenerator-shared-inl.h"

#include "mozilla/DebugOnly.h"

#include "jit/IonCaches.h"
#include "jit/IonMacroAssembler.h"
#include "jit/IonSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/ParallelFunctions.h"
#include "vm/TraceLogging.h"

#include "jit/IonFrames-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

namespace js {
namespace jit {

MacroAssembler&
CodeGeneratorShared::ensureMasm(MacroAssembler* masmArg)
{
    if (masmArg)
        return *masmArg;
    maybeMasm_.construct();
    return maybeMasm_.ref();
}

CodeGeneratorShared::CodeGeneratorShared(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masmArg)
  : oolIns(nullptr),
    maybeMasm_(),
    masm(ensureMasm(masmArg)),
    gen(gen),
    graph(*graph),
    current(nullptr),
    snapshots_(),
    recovers_(),
    deoptTable_(nullptr),
#ifdef DEBUG
    pushedArgs_(0),
#endif
    lastOsiPointOffset_(0),
    sps_(&GetIonContext()->runtime->spsProfiler(), &lastPC_),
    osrEntryOffset_(0),
    skipArgCheckEntryOffset_(0),
    frameDepth_(graph->paddedLocalSlotsSize() + graph->argumentsSize())
{
    if (!gen->compilingAsmJS())
        masm.setInstrumentation(&sps_);

    // Since asm.js uses the system ABI which does not necessarily use a
    // regular array where all slots are sizeof(Value), it maintains the max
    // argument stack depth separately.
    if (gen->compilingAsmJS()) {
        JS_ASSERT(graph->argumentSlotCount() == 0);
        frameDepth_ += gen->maxAsmJSStackArgBytes();

        // An MAsmJSCall does not align the stack pointer at calls sites but instead
        // relies on the a priori stack adjustment (in the prologue) on platforms
        // (like x64) which require the stack to be aligned.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
        bool forceAlign = true;
#else
        bool forceAlign = false;
#endif
        if (gen->performsAsmJSCall() || forceAlign) {
            unsigned alignmentAtCall = AlignmentMidPrologue + frameDepth_;
            if (unsigned rem = alignmentAtCall % StackAlignment)
                frameDepth_ += StackAlignment - rem;
        }

        // FrameSizeClass is only used for bailing, which cannot happen in
        // asm.js code.
        frameClass_ = FrameSizeClass::None();
    } else {
        frameClass_ = FrameSizeClass::FromDepth(frameDepth_);
    }
}

bool
CodeGeneratorShared::generateOutOfLineCode()
{
    for (size_t i = 0; i < outOfLineCode_.length(); i++) {
        if (!gen->alloc().ensureBallast())
            return false;
        masm.setFramePushed(outOfLineCode_[i]->framePushed());
        lastPC_ = outOfLineCode_[i]->pc();
        if (!sps_.prepareForOOL())
            return false;
        sps_.setPushed(outOfLineCode_[i]->script());
        outOfLineCode_[i]->bind(&masm);

        oolIns = outOfLineCode_[i];
        if (!outOfLineCode_[i]->generate(this))
            return false;
        sps_.finishOOL();
    }
    oolIns = nullptr;

    return true;
}

bool
CodeGeneratorShared::addOutOfLineCode(OutOfLineCode* code)
{
    code->setFramePushed(masm.framePushed());
    // If an OOL instruction adds another OOL instruction, then use the original
    // instruction's script/pc instead of the basic block's that we're on
    // because they're probably not relevant any more.
    if (oolIns)
        code->setSource(oolIns->script(), oolIns->pc());
    else
        code->setSource(current ? current->mir()->info().script() : nullptr, lastPC_);
    JS_ASSERT_IF(code->script(), code->script()->containsPC(code->pc()));
    return outOfLineCode_.append(code);
}

// see OffsetOfFrameSlot
static inline int32_t
ToStackIndex(LAllocation* a)
{
    if (a->isStackSlot()) {
        JS_ASSERT(a->toStackSlot()->slot() >= 1);
        return a->toStackSlot()->slot();
    }
    JS_ASSERT(-int32_t(sizeof(IonJSFrameLayout)) <= a->toArgument()->index());
    return -int32_t(sizeof(IonJSFrameLayout) + a->toArgument()->index());
}

bool
CodeGeneratorShared::encodeAllocations(LSnapshot* snapshot, MResumePoint* resumePoint,
                                       uint32_t* startIndex)
{
    IonSpew(IonSpew_Codegen, "Encoding %u of resume point %p's operands starting from %u",
            resumePoint->numOperands(), (void*) resumePoint, *startIndex);
    for (uint32_t allocno = 0, e = resumePoint->numOperands(); allocno < e; allocno++) {
        uint32_t i = allocno + *startIndex;
        MDefinition* mir = resumePoint->getOperand(allocno);

        if (mir->isBox())
            mir = mir->toBox()->getOperand(0);

        MIRType type = mir->isUnused()
                       ? MIRType_MagicOptimizedOut
                       : mir->type();

        RValueAllocation alloc;

        switch (type) {
          case MIRType_Undefined:
            alloc = RValueAllocation::Undefined();
            break;
          case MIRType_Null:
            alloc = RValueAllocation::Null();
            break;
          case MIRType_Int32:
          case MIRType_String:
          case MIRType_Object:
          case MIRType_Boolean:
          case MIRType_Double:
          case MIRType_Float32:
          {
            LAllocation* payload = snapshot->payloadOfSlot(i);
            JSValueType valueType = ValueTypeFromMIRType(type);
            if (payload->isMemory()) {
                if (type == MIRType_Float32)
                    alloc = RValueAllocation::Float32(ToStackIndex(payload));
                else
                    alloc = RValueAllocation::Typed(valueType, ToStackIndex(payload));
            } else if (payload->isGeneralReg()) {
                alloc = RValueAllocation::Typed(valueType, ToRegister(payload));
            } else if (payload->isFloatReg()) {
                FloatRegister reg = ToFloatRegister(payload);
                if (type == MIRType_Float32)
                    alloc = RValueAllocation::Float32(reg);
                else
                    alloc = RValueAllocation::Double(reg);
            } else {
                MConstant* constant = mir->toConstant();
                uint32_t index;
                if (!graph.addConstantToPool(constant->value(), &index))
                    return false;
                alloc = RValueAllocation::ConstantPool(index);
            }
            break;
          }
          case MIRType_MagicOptimizedArguments:
          case MIRType_MagicOptimizedOut:
          {
            uint32_t index;
            JSWhyMagic why = (type == MIRType_MagicOptimizedArguments
                              ? JS_OPTIMIZED_ARGUMENTS
                              : JS_OPTIMIZED_OUT);
            Value v = MagicValue(why);
            if (!graph.addConstantToPool(v, &index))
                return false;
            alloc = RValueAllocation::ConstantPool(index);
            break;
          }
          default:
          {
            JS_ASSERT(mir->type() == MIRType_Value);
            LAllocation* payload = snapshot->payloadOfSlot(i);
#ifdef JS_NUNBOX32
            LAllocation* type = snapshot->typeOfSlot(i);
            if (type->isRegister()) {
                if (payload->isRegister())
                    alloc = RValueAllocation::Untyped(ToRegister(type), ToRegister(payload));
                else
                    alloc = RValueAllocation::Untyped(ToRegister(type), ToStackIndex(payload));
            } else {
                if (payload->isRegister())
                    alloc = RValueAllocation::Untyped(ToStackIndex(type), ToRegister(payload));
                else
                    alloc = RValueAllocation::Untyped(ToStackIndex(type), ToStackIndex(payload));
            }
#elif JS_PUNBOX64
            if (payload->isRegister())
                alloc = RValueAllocation::Untyped(ToRegister(payload));
            else
                alloc = RValueAllocation::Untyped(ToStackIndex(payload));
#endif
            break;
          }
        }

        snapshots_.add(alloc);
    }

    *startIndex += resumePoint->numOperands();
    return true;
}

bool
CodeGeneratorShared::encode(LRecoverInfo* recover)
{
    if (recover->recoverOffset() != INVALID_RECOVER_OFFSET)
        return true;

    uint32_t frameCount = recover->mir()->frameCount();
    IonSpew(IonSpew_Snapshots, "Encoding LRecoverInfo %p (frameCount %u)",
            (void*)recover, frameCount);

    MResumePoint::Mode mode = recover->mir()->mode();
    JS_ASSERT(mode != MResumePoint::Outer);
    bool resumeAfter = (mode == MResumePoint::ResumeAfter);

    RecoverOffset offset = recovers_.startRecover(frameCount, resumeAfter);

    for (MResumePoint** it = recover->begin(), **end = recover->end();
         it != end;
         ++it)
    {
        if (!recovers_.writeFrame(*it))
            return false;
    }

    recovers_.endRecover();
    recover->setRecoverOffset(offset);
    return !recovers_.oom();
}

bool
CodeGeneratorShared::encode(LSnapshot* snapshot)
{
    if (snapshot->snapshotOffset() != INVALID_SNAPSHOT_OFFSET)
        return true;

    LRecoverInfo* recoverInfo = snapshot->recoverInfo();
    if (!encode(recoverInfo))
        return false;

    RecoverOffset recoverOffset = recoverInfo->recoverOffset();
    MOZ_ASSERT(recoverOffset != INVALID_RECOVER_OFFSET);

    IonSpew(IonSpew_Snapshots, "Encoding LSnapshot %p (LRecover %p)",
            (void*)snapshot, (void*) recoverInfo);

    SnapshotOffset offset = snapshots_.startSnapshot(recoverOffset, snapshot->bailoutKind());

#ifdef TRACK_SNAPSHOTS
    uint32_t pcOpcode = 0;
    uint32_t lirOpcode = 0;
    uint32_t lirId = 0;
    uint32_t mirOpcode = 0;
    uint32_t mirId = 0;

    if (LInstruction* ins = instruction()) {
        lirOpcode = ins->op();
        lirId = ins->id();
        if (ins->mirRaw()) {
            mirOpcode = ins->mirRaw()->op();
            mirId = ins->mirRaw()->id();
            if (ins->mirRaw()->trackedPc())
                pcOpcode = *ins->mirRaw()->trackedPc();
        }
    }
    snapshots_.trackSnapshot(pcOpcode, mirOpcode, mirId, lirOpcode, lirId);
#endif

    uint32_t startIndex = 0;
    for (MResumePoint** it = recoverInfo->begin(), **end = recoverInfo->end();
         it != end;
         ++it)
    {
        MResumePoint* mir = *it;
        if (!encodeAllocations(snapshot, mir, &startIndex))
            return false;
    }

    MOZ_ASSERT(snapshots_.allocWritten() == snapshot->numSlots());
    snapshots_.endSnapshot();
    snapshot->setSnapshotOffset(offset);
    return !snapshots_.oom();
}

bool
CodeGeneratorShared::assignBailoutId(LSnapshot* snapshot)
{
    JS_ASSERT(snapshot->snapshotOffset() != INVALID_SNAPSHOT_OFFSET);

    // Can we not use bailout tables at all?
    if (!deoptTable_)
        return false;

    JS_ASSERT(frameClass_ != FrameSizeClass::None());

    if (snapshot->bailoutId() != INVALID_BAILOUT_ID)
        return true;

    // Is the bailout table full?
    if (bailouts_.length() >= BAILOUT_TABLE_SIZE)
        return false;

    unsigned bailoutId = bailouts_.length();
    snapshot->setBailoutId(bailoutId);
    IonSpew(IonSpew_Snapshots, "Assigned snapshot bailout id %u", bailoutId);
    return bailouts_.append(snapshot->snapshotOffset());
}

void
CodeGeneratorShared::encodeSafepoints()
{
    for (SafepointIndex* it = safepointIndices_.begin(), *end = safepointIndices_.end();
         it != end;
         ++it)
    {
        LSafepoint* safepoint = it->safepoint();

        if (!safepoint->encoded()) {
            safepoint->fixupOffset(&masm);
            safepoints_.encode(safepoint);
        }

        it->resolve();
    }
}

bool
CodeGeneratorShared::markSafepoint(LInstruction* ins)
{
    return markSafepointAt(masm.currentOffset(), ins);
}

bool
CodeGeneratorShared::markSafepointAt(uint32_t offset, LInstruction* ins)
{
    JS_ASSERT_IF(!safepointIndices_.empty(),
                 offset - safepointIndices_.back().displacement() >= sizeof(uint32_t));
    return safepointIndices_.append(SafepointIndex(offset, ins->safepoint()));
}

void
CodeGeneratorShared::ensureOsiSpace()
{
    // For a refresher, an invalidation point is of the form:
    // 1: call <target>
    // 2: ...
    // 3: <osipoint>
    //
    // The four bytes *before* instruction 2 are overwritten with an offset.
    // Callers must ensure that the instruction itself has enough bytes to
    // support this.
    //
    // The bytes *at* instruction 3 are overwritten with an invalidation jump.
    // jump. These bytes may be in a completely different IR sequence, but
    // represent the join point of the call out of the function.
    //
    // At points where we want to ensure that invalidation won't corrupt an
    // important instruction, we make sure to pad with nops.
    if (masm.currentOffset() - lastOsiPointOffset_ < Assembler::patchWrite_NearCallSize()) {
        int32_t paddingSize = Assembler::patchWrite_NearCallSize();
        paddingSize -= masm.currentOffset() - lastOsiPointOffset_;
        for (int32_t i = 0; i < paddingSize; ++i)
            masm.nop();
    }
    JS_ASSERT(masm.currentOffset() - lastOsiPointOffset_ >= Assembler::patchWrite_NearCallSize());
    lastOsiPointOffset_ = masm.currentOffset();
}

bool
CodeGeneratorShared::markOsiPoint(LOsiPoint* ins, uint32_t* callPointOffset)
{
    if (!encode(ins->snapshot()))
        return false;

    ensureOsiSpace();

    *callPointOffset = masm.currentOffset();
    SnapshotOffset so = ins->snapshot()->snapshotOffset();
    return osiIndices_.append(OsiIndex(*callPointOffset, so));
}

#ifdef CHECK_OSIPOINT_REGISTERS
template <class Op>
static void
HandleRegisterDump(Op op, MacroAssembler& masm, RegisterSet liveRegs, Register activation,
                   Register scratch)
{
    const size_t baseOffset = JitActivation::offsetOfRegs();

    // Handle live GPRs.
    for (GeneralRegisterIterator iter(liveRegs.gprs()); iter.more(); iter++) {
        Register reg = *iter;
        Address dump(activation, baseOffset + RegisterDump::offsetOfRegister(reg));

        if (reg == activation) {
            // To use the original value of the activation register (that's
            // now on top of the stack), we need the scratch register.
            masm.push(scratch);
            masm.loadPtr(Address(StackPointer, sizeof(uintptr_t)), scratch);
            op(scratch, dump);
            masm.pop(scratch);
        } else {
            op(reg, dump);
        }
    }

    // Handle live FPRs.
    for (FloatRegisterIterator iter(liveRegs.fpus()); iter.more(); iter++) {
        FloatRegister reg = *iter;
        Address dump(activation, baseOffset + RegisterDump::offsetOfRegister(reg));
        op(reg, dump);
    }
}

class StoreOp
{
    MacroAssembler& masm;

  public:
    StoreOp(MacroAssembler& masm)
      : masm(masm)
    {}

    void operator()(Register reg, Address dump) {
        masm.storePtr(reg, dump);
    }
    void operator()(FloatRegister reg, Address dump) {
        masm.storeDouble(reg, dump);
    }
};

static void
StoreAllLiveRegs(MacroAssembler& masm, RegisterSet liveRegs)
{
    // Store a copy of all live registers before performing the call.
    // When we reach the OsiPoint, we can use this to check nothing
    // modified them in the meantime.

    // Load pointer to the JitActivation in a scratch register.
    GeneralRegisterSet allRegs(GeneralRegisterSet::All());
    Register scratch = allRegs.takeAny();
    masm.push(scratch);
    masm.loadJitActivation(scratch);

    Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
    masm.add32(Imm32(1), checkRegs);

    StoreOp op(masm);
    HandleRegisterDump<StoreOp>(op, masm, liveRegs, scratch, allRegs.getAny());

    masm.pop(scratch);
}

class VerifyOp
{
    MacroAssembler& masm;
    Label* failure_;

  public:
    VerifyOp(MacroAssembler& masm, Label* failure)
      : masm(masm), failure_(failure)
    {}

    void operator()(Register reg, Address dump) {
        masm.branchPtr(Assembler::NotEqual, dump, reg, failure_);
    }
    void operator()(FloatRegister reg, Address dump) {
        masm.loadDouble(dump, ScratchFloatReg);
        masm.branchDouble(Assembler::DoubleNotEqual, ScratchFloatReg, reg, failure_);
    }
};

static void
OsiPointRegisterCheckFailed()
{
    // Any live register captured by a safepoint (other than temp registers)
    // must remain unchanged between the call and the OsiPoint instruction.
    MOZ_ASSUME_UNREACHABLE("Modified registers between VM call and OsiPoint");
}

void
CodeGeneratorShared::verifyOsiPointRegs(LSafepoint* safepoint)
{
    // Ensure the live registers stored by callVM did not change between
    // the call and this OsiPoint. Try-catch relies on this invariant.

    // Load pointer to the JitActivation in a scratch register.
    GeneralRegisterSet allRegs(GeneralRegisterSet::All());
    Register scratch = allRegs.takeAny();
    masm.push(scratch);
    masm.loadJitActivation(scratch);

    // If we should not check registers (because the instruction did not call
    // into the VM, or a GC happened), we're done.
    Label failure, done;
    Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
    masm.branch32(Assembler::Equal, checkRegs, Imm32(0), &done);

    // Having more than one VM function call made in one visit function at
    // runtime is a sec-ciritcal error, because if we conservatively assume that
    // one of the function call can re-enter Ion, then the invalidation process
    // will potentially add a call at a random location, by patching the code
    // before the return address.
    masm.branch32(Assembler::NotEqual, checkRegs, Imm32(1), &failure);

    // Ignore clobbered registers. Some instructions (like LValueToInt32) modify
    // temps after calling into the VM. This is fine because no other
    // instructions (including this OsiPoint) will depend on them. Also
    // backtracking can also use the same register for an input and an output.
    // These are marked as clobbered and shouldn't get checked.
    RegisterSet liveRegs = safepoint->liveRegs();
    liveRegs = RegisterSet::Intersect(liveRegs, RegisterSet::Not(safepoint->clobberedRegs()));

    VerifyOp op(masm, &failure);
    HandleRegisterDump<VerifyOp>(op, masm, liveRegs, scratch, allRegs.getAny());

    masm.jump(&done);

    // Do not profile the callWithABI that occurs below.  This is to avoid a
    // rare corner case that occurs when profiling interacts with itself:
    //
    // When slow profiling assertions are turned on, FunctionBoundary ops
    // (which update the profiler pseudo-stack) may emit a callVM, which
    // forces them to have an osi point associated with them.  The
    // FunctionBoundary for inline function entry is added to the caller's
    // graph with a PC from the caller's code, but during codegen it modifies
    // SPS instrumentation to add the callee as the current top-most script.
    // When codegen gets to the OSIPoint, and the callWithABI below is
    // emitted, the codegen thinks that the current frame is the callee, but
    // the PC it's using from the OSIPoint refers to the caller.  This causes
    // the profiler instrumentation of the callWithABI below to ASSERT, since
    // the script and pc are mismatched.  To avoid this, we simply omit
    // instrumentation for these callWithABIs.
    masm.bind(&failure);
    masm.setupUnalignedABICall(0, scratch);
    masm.callWithABINoProfiling(JS_FUNC_TO_DATA_PTR(void*, OsiPointRegisterCheckFailed));
    masm.breakpoint();

    masm.bind(&done);
    masm.pop(scratch);
}

bool
CodeGeneratorShared::shouldVerifyOsiPointRegs(LSafepoint* safepoint)
{
    if (!js_JitOptions.checkOsiPointRegisters)
        return false;

    if (gen->info().executionMode() != SequentialExecution)
        return false;

    if (safepoint->liveRegs().empty(true) && safepoint->liveRegs().empty(false))
        return false; // No registers to check.

    return true;
}

void
CodeGeneratorShared::resetOsiPointRegs(LSafepoint* safepoint)
{
    if (!shouldVerifyOsiPointRegs(safepoint))
        return;

    // Set checkRegs to 0. If we perform a VM call, the instruction
    // will set it to 1.
    GeneralRegisterSet allRegs(GeneralRegisterSet::All());
    Register scratch = allRegs.takeAny();
    masm.push(scratch);
    masm.loadJitActivation(scratch);
    Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
    masm.store32(Imm32(0), checkRegs);
    masm.pop(scratch);
}
#endif

// Before doing any call to Cpp, you should ensure that volatile
// registers are evicted by the register allocator.
bool
CodeGeneratorShared::callVM(const VMFunction& fun, LInstruction* ins, const Register* dynStack)
{
    // Different execution modes have different sets of VM functions.
    JS_ASSERT(fun.executionMode == gen->info().executionMode());

    // If we're calling a function with an out parameter type of double, make
    // sure we have an FPU.
    JS_ASSERT_IF(fun.outParam == Type_Double, GetIonContext()->runtime->jitSupportsFloatingPoint());

#ifdef DEBUG
    if (ins->mirRaw()) {
        JS_ASSERT(ins->mirRaw()->isInstruction());
        MInstruction* mir = ins->mirRaw()->toInstruction();
        JS_ASSERT_IF(mir->isEffectful(), mir->resumePoint());
    }
#endif

#ifdef JS_TRACE_LOGGING
    if (!emitTracelogStartEvent(TraceLogger::VM))
        return false;
#endif

    // Stack is:
    //    ... frame ...
    //    [args]
#ifdef DEBUG
    JS_ASSERT(pushedArgs_ == fun.explicitArgs);
    pushedArgs_ = 0;
#endif

    // Get the wrapper of the VM function.
    JitCode* wrapper = gen->jitRuntime()->getVMWrapper(fun);
    if (!wrapper)
        return false;

#ifdef CHECK_OSIPOINT_REGISTERS
    if (shouldVerifyOsiPointRegs(ins->safepoint()))
        StoreAllLiveRegs(masm, ins->safepoint()->liveRegs());
#endif

    // Call the wrapper function.  The wrapper is in charge to unwind the stack
    // when returning from the call.  Failures are handled with exceptions based
    // on the return value of the C functions.  To guard the outcome of the
    // returned value, use another LIR instruction.
    uint32_t callOffset;
    if (dynStack)
        callOffset = masm.callWithExitFrame(wrapper, *dynStack);
    else
        callOffset = masm.callWithExitFrame(wrapper);

    if (!markSafepointAt(callOffset, ins))
        return false;

    // Remove rest of the frame left on the stack. We remove the return address
    // which is implicitly poped when returning.
    int framePop = sizeof(IonExitFrameLayout) - sizeof(void*);

    // Pop arguments from framePushed.
    masm.implicitPop(fun.explicitStackSlots() * sizeof(void*) + framePop);
    // Stack is:
    //    ... frame ...

#ifdef JS_TRACE_LOGGING
    if (!emitTracelogStopEvent(TraceLogger::VM))
        return false;
#endif

    return true;
}

class OutOfLineTruncateSlow : public OutOfLineCodeBase<CodeGeneratorShared>
{
    FloatRegister src_;
    Register dest_;
    bool needFloat32Conversion_;

  public:
    OutOfLineTruncateSlow(FloatRegister src, Register dest, bool needFloat32Conversion = false)
      : src_(src), dest_(dest), needFloat32Conversion_(needFloat32Conversion)
    { }

    bool accept(CodeGeneratorShared* codegen) {
        return codegen->visitOutOfLineTruncateSlow(this);
    }
    FloatRegister src() const {
        return src_;
    }
    Register dest() const {
        return dest_;
    }
    bool needFloat32Conversion() const {
        return needFloat32Conversion_;
    }

};

OutOfLineCode*
CodeGeneratorShared::oolTruncateDouble(const FloatRegister& src, const Register& dest)
{
    OutOfLineTruncateSlow* ool = new(alloc()) OutOfLineTruncateSlow(src, dest);
    if (!addOutOfLineCode(ool))
        return nullptr;
    return ool;
}

bool
CodeGeneratorShared::emitTruncateDouble(const FloatRegister& src, const Register& dest)
{
    OutOfLineCode* ool = oolTruncateDouble(src, dest);
    if (!ool)
        return false;

    masm.branchTruncateDouble(src, dest, ool->entry());
    masm.bind(ool->rejoin());
    return true;
}

bool
CodeGeneratorShared::emitTruncateFloat32(const FloatRegister& src, const Register& dest)
{
    OutOfLineTruncateSlow* ool = new(alloc()) OutOfLineTruncateSlow(src, dest, true);
    if (!addOutOfLineCode(ool))
        return false;

    masm.branchTruncateFloat32(src, dest, ool->entry());
    masm.bind(ool->rejoin());
    return true;
}

bool
CodeGeneratorShared::visitOutOfLineTruncateSlow(OutOfLineTruncateSlow* ool)
{
    FloatRegister src = ool->src();
    Register dest = ool->dest();

    saveVolatile(dest);

    if (ool->needFloat32Conversion()) {
        masm.push(src);
        masm.convertFloat32ToDouble(src, src);
    }

    masm.setupUnalignedABICall(1, dest);
    masm.passABIArg(src, MoveOp::DOUBLE);
    if (gen->compilingAsmJS())
        masm.callWithABI(AsmJSImm_ToInt32);
    else
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::ToInt32));
    masm.storeCallResult(dest);

    if (ool->needFloat32Conversion())
        masm.pop(src);

    restoreVolatile(dest);

    masm.jump(ool->rejoin());
    return true;
}

bool
CodeGeneratorShared::omitOverRecursedCheck() const
{
    // If the current function makes no calls (which means it isn't recursive)
    // and it uses only a small amount of stack space, it doesn't need a
    // stack overflow check. Note that the actual number here is somewhat
    // arbitrary, and codegen actually uses small bounded amounts of
    // additional stack space in some cases too.
    return frameSize() < 64 && !gen->performsCall();
}

void
CodeGeneratorShared::emitPreBarrier(Register base, const LAllocation* index, MIRType type)
{
    if (index->isConstant()) {
        Address address(base, ToInt32(index) * sizeof(Value));
        masm.patchableCallPreBarrier(address, type);
    } else {
        BaseIndex address(base, ToRegister(index), TimesEight);
        masm.patchableCallPreBarrier(address, type);
    }
}

void
CodeGeneratorShared::emitPreBarrier(Address address, MIRType type)
{
    masm.patchableCallPreBarrier(address, type);
}

void
CodeGeneratorShared::dropArguments(unsigned argc)
{
    pushedArgumentSlots_.shrinkBy(argc);
}

bool
CodeGeneratorShared::markArgumentSlots(LSafepoint* safepoint)
{
    for (size_t i = 0; i < pushedArgumentSlots_.length(); i++) {
        if (!safepoint->addValueSlot(pushedArgumentSlots_[i]))
            return false;
    }
    return true;
}

OutOfLineAbortPar*
CodeGeneratorShared::oolAbortPar(ParallelBailoutCause cause, MBasicBlock* basicBlock,
                                 jsbytecode* bytecode)
{
    OutOfLineAbortPar* ool = new(alloc()) OutOfLineAbortPar(cause, basicBlock, bytecode);
    if (!ool || !addOutOfLineCode(ool))
        return nullptr;
    return ool;
}

OutOfLineAbortPar*
CodeGeneratorShared::oolAbortPar(ParallelBailoutCause cause, LInstruction* lir)
{
    MDefinition* mir = lir->mirRaw();
    MBasicBlock* block = mir->block();
    jsbytecode* pc = mir->trackedPc();
    if (!pc) {
        if (lir->snapshot())
            pc = lir->snapshot()->mir()->pc();
        else
            pc = block->pc();
    }
    return oolAbortPar(cause, block, pc);
}

OutOfLinePropagateAbortPar*
CodeGeneratorShared::oolPropagateAbortPar(LInstruction* lir)
{
    OutOfLinePropagateAbortPar* ool = new(alloc()) OutOfLinePropagateAbortPar(lir);
    if (!ool || !addOutOfLineCode(ool))
        return nullptr;
    return ool;
}

bool
OutOfLineAbortPar::generate(CodeGeneratorShared* codegen)
{
    codegen->callTraceLIR(0xDEADBEEF, nullptr, "AbortPar");
    return codegen->visitOutOfLineAbortPar(this);
}

bool
OutOfLinePropagateAbortPar::generate(CodeGeneratorShared* codegen)
{
    codegen->callTraceLIR(0xDEADBEEF, nullptr, "AbortPar");
    return codegen->visitOutOfLinePropagateAbortPar(this);
}

bool
CodeGeneratorShared::callTraceLIR(uint32_t blockIndex, LInstruction* lir,
                                  const char* bailoutName)
{
    JS_ASSERT_IF(!lir, bailoutName);

    if (!IonSpewEnabled(IonSpew_Trace))
        return true;

    uint32_t execMode = (uint32_t) gen->info().executionMode();
    uint32_t lirIndex;
    const char* lirOpName;
    const char* mirOpName;
    JSScript* script;
    jsbytecode* pc;

    masm.PushRegsInMask(RegisterSet::Volatile());
    masm.reserveStack(sizeof(IonLIRTraceData));

    // This first move is here so that when you scan the disassembly,
    // you can easily pick out where each instruction begins.  The
    // next few items indicate to you the Basic Block / LIR.
    masm.move32(Imm32(0xDEADBEEF), CallTempReg0);

    if (lir) {
        lirIndex = lir->id();
        lirOpName = lir->opName();
        if (MDefinition* mir = lir->mirRaw()) {
            mirOpName = mir->opName();
            script = mir->block()->info().script();
            pc = mir->trackedPc();
        } else {
            mirOpName = nullptr;
            script = nullptr;
            pc = nullptr;
        }
    } else {
        blockIndex = lirIndex = 0xDEADBEEF;
        lirOpName = mirOpName = bailoutName;
        script = nullptr;
        pc = nullptr;
    }

    masm.store32(Imm32(blockIndex),
                 Address(StackPointer, offsetof(IonLIRTraceData, blockIndex)));
    masm.store32(Imm32(lirIndex),
                 Address(StackPointer, offsetof(IonLIRTraceData, lirIndex)));
    masm.store32(Imm32(execMode),
                 Address(StackPointer, offsetof(IonLIRTraceData, execModeInt)));
    masm.storePtr(ImmPtr(lirOpName),
                  Address(StackPointer, offsetof(IonLIRTraceData, lirOpName)));
    masm.storePtr(ImmPtr(mirOpName),
                  Address(StackPointer, offsetof(IonLIRTraceData, mirOpName)));
    masm.storePtr(ImmGCPtr(script),
                  Address(StackPointer, offsetof(IonLIRTraceData, script)));
    masm.storePtr(ImmPtr(pc),
                  Address(StackPointer, offsetof(IonLIRTraceData, pc)));

    masm.movePtr(StackPointer, CallTempReg0);
    masm.setupUnalignedABICall(1, CallTempReg1);
    masm.passABIArg(CallTempReg0);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, TraceLIR));

    masm.freeStack(sizeof(IonLIRTraceData));
    masm.PopRegsInMask(RegisterSet::Volatile());

    return true;
}

typedef bool (*InterruptCheckFn)(JSContext*);
const VMFunction InterruptCheckInfo = FunctionInfo<InterruptCheckFn>(InterruptCheck);

Label*
CodeGeneratorShared::labelForBackedgeWithImplicitCheck(MBasicBlock* mir)
{
    // If this is a loop backedge to a loop header with an implicit interrupt
    // check, use a patchable jump. Skip this search if compiling without a
    // script for asm.js, as there will be no interrupt check instruction.
    // Due to critical edge unsplitting there may no longer be unique loop
    // backedges, so just look for any edge going to an earlier block in RPO.
    if (!gen->compilingAsmJS() && mir->isLoopHeader() && mir->id() <= current->mir()->id()) {
        for (LInstructionIterator iter = mir->lir()->begin(); iter != mir->lir()->end(); iter++) {
            if (iter->isLabel() || iter->isMoveGroup()) {
                // Continue searching for an interrupt check.
            } else if (iter->isInterruptCheckImplicit()) {
                return iter->toInterruptCheckImplicit()->oolEntry();
            } else {
                // The interrupt check should be the first instruction in the
                // loop header other than the initial label and move groups.
                JS_ASSERT(iter->isInterruptCheck() || iter->isInterruptCheckPar());
                return nullptr;
            }
        }
    }

    return nullptr;
}

void
CodeGeneratorShared::jumpToBlock(MBasicBlock* mir)
{
    // No jump necessary if we can fall through to the next block.
    if (isNextBlock(mir->lir()))
        return;

    if (Label* oolEntry = labelForBackedgeWithImplicitCheck(mir)) {
        // Note: the backedge is initially a jump to the next instruction.
        // It will be patched to the target block's label during link().
        RepatchLabel rejoin;
        CodeOffsetJump backedge = masm.jumpWithPatch(&rejoin);
        masm.bind(&rejoin);

        masm.propagateOOM(patchableBackedges_.append(PatchableBackedgeInfo(backedge, mir->lir()->label(), oolEntry)));
    } else {
        masm.jump(mir->lir()->label());
    }
}

// This function is not used for MIPS. MIPS has branchToBlock.
#ifndef JS_CODEGEN_MIPS
void
CodeGeneratorShared::jumpToBlock(MBasicBlock* mir, Assembler::Condition cond)
{
    if (Label* oolEntry = labelForBackedgeWithImplicitCheck(mir)) {
        // Note: the backedge is initially a jump to the next instruction.
        // It will be patched to the target block's label during link().
        RepatchLabel rejoin;
        CodeOffsetJump backedge = masm.jumpWithPatch(&rejoin, cond);
        masm.bind(&rejoin);

        masm.propagateOOM(patchableBackedges_.append(PatchableBackedgeInfo(backedge, mir->lir()->label(), oolEntry)));
    } else {
        masm.j(cond, mir->lir()->label());
    }
}
#endif

size_t
CodeGeneratorShared::addCacheLocations(const CacheLocationList& locs, size_t* numLocs)
{
    size_t firstIndex = runtimeData_.length();
    size_t numLocations = 0;
    for (CacheLocationList::iterator iter = locs.begin(); iter != locs.end(); iter++) {
        // allocateData() ensures that sizeof(CacheLocation) is word-aligned.
        // If this changes, we will need to pad to ensure alignment.
        size_t curIndex = allocateData(sizeof(CacheLocation));
        new (&runtimeData_[curIndex]) CacheLocation(iter->pc, iter->script);
        numLocations++;
    }
    JS_ASSERT(numLocations != 0);
    *numLocs = numLocations;
    return firstIndex;
}

ReciprocalMulConstants
CodeGeneratorShared::computeDivisionConstants(int d) {
    // In what follows, d is positive and is not a power of 2.
    JS_ASSERT(d > 0 && (d & (d - 1)) != 0);

    // Speeding up division by non power-of-2 constants is possible by
    // calculating, during compilation, a value M such that high-order
    // bits of M*n correspond to the result of the division. Formally,
    // we compute values 0 <= M < 2^32 and 0 <= s < 31 such that
    //         (M * n) >> (32 + s) = floor(n/d)    if n >= 0
    //         (M * n) >> (32 + s) = ceil(n/d) - 1 if n < 0.
    // The original presentation of this technique appears in Hacker's
    // Delight, a book by Henry S. Warren, Jr.. A proof of correctness
    // for our version follows.

    // Define p = 32 + s, M = ceil(2^p/d), and assume that s satisfies
    //                     M - 2^p/d <= 2^(s+1)/d.                 (1)
    // (Observe that s = FloorLog32(d) satisfies this, because in this
    // case d <= 2^(s+1) and so the RHS of (1) is at least one). Then,
    //
    // a) If s <= FloorLog32(d), then M <= 2^32 - 1.
    // Proof: Indeed, M is monotone in s and, for s = FloorLog32(d),
    // the inequalities 2^31 > d >= 2^s + 1 readily imply
    //    2^p / d  = 2^p/(d - 1) * (d - 1)/d
    //            <= 2^32 * (1 - 1/d) < 2 * (2^31 - 1) = 2^32 - 2.
    // The claim follows by applying the ceiling function.
    //
    // b) For any 0 <= n < 2^31, floor(Mn/2^p) = floor(n/d).
    // Proof: Put x = floor(Mn/2^p); it's the unique integer for which
    //                    Mn/2^p - 1 < x <= Mn/2^p.                (2)
    // Using M >= 2^p/d on the LHS and (1) on the RHS, we get
    //           n/d - 1 < x <= n/d + n/(2^31 d) < n/d + 1/d.
    // Since x is an integer, it's not in the interval (n/d, (n+1)/d),
    // and so n/d - 1 < x <= n/d, which implies x = floor(n/d).
    //
    // c) For any -2^31 <= n < 0, floor(Mn/2^p) + 1 = ceil(n/d).
    // Proof: The proof is similar. Equation (2) holds as above. Using
    // M > 2^p/d (d isn't a power of 2) on the RHS and (1) on the LHS,
    //                 n/d + n/(2^31 d) - 1 < x < n/d.
    // Using n >= -2^31 and summing 1,
    //                  n/d - 1/d < x + 1 < n/d + 1.
    // Since x + 1 is an integer, this implies n/d <= x + 1 < n/d + 1.
    // In other words, x + 1 = ceil(n/d).
    //
    // Condition (1) isn't necessary for the existence of M and s with
    // the properties above. Hacker's Delight provides a slightly less
    // restrictive condition when d >= 196611, at the cost of a 3-page
    // proof of correctness.

    // Note that, since d*M - 2^p = d - (2^p)%d, (1) can be written as
    //                   2^(s+1) >= d - (2^p)%d.
    // We now compute the least s with this property...

    int32_t shift = 0;
    while ((int64_t(1) << (shift+1)) + (int64_t(1) << (shift+32)) % d < d)
        shift++;

    // ...and the corresponding M. This may not fit in a signed 32-bit
    // integer; we will compute (M - 2^32) * n + (2^32 * n) instead of
    // M * n if this is the case (cf. item (a) above).
    ReciprocalMulConstants rmc;
    rmc.multiplier = int32_t((int64_t(1) << (shift+32))/d + 1);
    rmc.shiftAmount = shift;

    return rmc;
}


#ifdef JS_TRACE_LOGGING

bool
CodeGeneratorShared::emitTracelogScript(bool isStart)
{
    RegisterSet regs = RegisterSet::Volatile();
    Register logger = regs.takeGeneral();
    Register script = regs.takeGeneral();

    masm.Push(logger);
    masm.Push(script);

    CodeOffsetLabel patchLogger = masm.movWithPatch(ImmPtr(nullptr), logger);
    if (!patchableTraceLoggers_.append(patchLogger))
        return false;

    CodeOffsetLabel patchScript = masm.movWithPatch(ImmWord(0), script);
    if (!patchableTLScripts_.append(patchScript))
        return false;

    if (isStart)
        masm.tracelogStart(logger, script);
    else
        masm.tracelogStop(logger, script);

    masm.Pop(script);
    masm.Pop(logger);
    return true;
}

bool
CodeGeneratorShared::emitTracelogTree(bool isStart, uint32_t textId)
{
    if (!TraceLogTextIdEnabled(textId))
        return true;

    RegisterSet regs = RegisterSet::Volatile();
    Register logger = regs.takeGeneral();

    masm.Push(logger);

    CodeOffsetLabel patchLocation = masm.movWithPatch(ImmPtr(nullptr), logger);
    if (!patchableTraceLoggers_.append(patchLocation))
        return false;

    if (isStart) {
        masm.tracelogStart(logger, textId);
    } else {
#ifdef DEBUG
        masm.tracelogStop(logger, textId);
#else
        masm.tracelogStop(logger);
#endif
    }

    masm.Pop(logger);
    return true;
}
#endif

} // namespace jit
} // namespace js
