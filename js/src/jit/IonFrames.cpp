/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonFrames-inl.h"

#include "jsfun.h"
#include "jsobj.h"
#include "jsscript.h"

#include "gc/Marking.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/IonMacroAssembler.h"
#include "jit/IonSpewer.h"
#include "jit/JitCompartment.h"
#include "jit/ParallelFunctions.h"
#include "jit/PcScriptCache.h"
#include "jit/Recover.h"
#include "jit/Safepoints.h"
#include "jit/Snapshots.h"
#include "jit/VMFunctions.h"
#include "vm/ArgumentsObject.h"
#include "vm/ForkJoin.h"
#include "vm/Interpreter.h"

#include "jsscriptinlines.h"
#include "jit/JitFrameIterator-inl.h"
#include "vm/Probes-inl.h"

namespace js {
namespace jit {

// Given a slot index, returns the offset, in bytes, of that slot from an
// IonJSFrameLayout. Slot distances are uniform across architectures, however,
// the distance does depend on the size of the frame header.
static inline int32_t
OffsetOfFrameSlot(int32_t slot)
{
    return -slot;
}

static inline uintptr_t
ReadFrameSlot(IonJSFrameLayout* fp, int32_t slot)
{
    return *(uintptr_t*)((char*)fp + OffsetOfFrameSlot(slot));
}

static inline double
ReadFrameDoubleSlot(IonJSFrameLayout* fp, int32_t slot)
{
    return *(double*)((char*)fp + OffsetOfFrameSlot(slot));
}

static inline float
ReadFrameFloat32Slot(IonJSFrameLayout* fp, int32_t slot)
{
    return *(float*)((char*)fp + OffsetOfFrameSlot(slot));
}

static inline int32_t
ReadFrameInt32Slot(IonJSFrameLayout* fp, int32_t slot)
{
    return *(int32_t*)((char*)fp + OffsetOfFrameSlot(slot));
}

static inline bool
ReadFrameBooleanSlot(IonJSFrameLayout* fp, int32_t slot)
{
    return *(bool*)((char*)fp + OffsetOfFrameSlot(slot));
}

JitFrameIterator::JitFrameIterator(JSContext* cx)
  : current_(cx->mainThread().ionTop),
    type_(JitFrame_Exit),
    returnAddressToFp_(nullptr),
    frameSize_(0),
    cachedSafepointIndex_(nullptr),
    activation_(nullptr),
    mode_(SequentialExecution)
{
}

JitFrameIterator::JitFrameIterator(const ActivationIterator& activations)
    : current_(activations.jitTop()),
      type_(JitFrame_Exit),
      returnAddressToFp_(nullptr),
      frameSize_(0),
      cachedSafepointIndex_(nullptr),
      activation_(activations->asJit()),
      mode_(SequentialExecution)
{
}

JitFrameIterator::JitFrameIterator(IonJSFrameLayout* fp, ExecutionMode mode)
  : current_((uint8_t*)fp),
    type_(JitFrame_IonJS),
    returnAddressToFp_(fp->returnAddress()),
    frameSize_(fp->prevFrameLocalSize()),
    mode_(mode)
{
}

bool
JitFrameIterator::checkInvalidation() const
{
    IonScript* dummy;
    return checkInvalidation(&dummy);
}

bool
JitFrameIterator::checkInvalidation(IonScript** ionScriptOut) const
{
    uint8_t* returnAddr = returnAddressToFp();
    JSScript* script = this->script();
    // N.B. the current IonScript is not the same as the frame's
    // IonScript if the frame has since been invalidated.
    bool invalidated;
    if (mode_ == ParallelExecution) {
        // Parallel execution does not have invalidating bailouts.
        invalidated = false;
    } else {
        invalidated = !script->hasIonScript() ||
            !script->ionScript()->containsReturnAddress(returnAddr);
    }
    if (!invalidated)
        return false;

    int32_t invalidationDataOffset = ((int32_t*) returnAddr)[-1];
    uint8_t* ionScriptDataOffset = returnAddr + invalidationDataOffset;
    IonScript* ionScript = (IonScript*) Assembler::getPointer(ionScriptDataOffset);
    JS_ASSERT(ionScript->containsReturnAddress(returnAddr));
    *ionScriptOut = ionScript;
    return true;
}

CalleeToken
JitFrameIterator::calleeToken() const
{
    return ((IonJSFrameLayout*) current_)->calleeToken();
}

JSFunction*
JitFrameIterator::callee() const
{
    JS_ASSERT(isScripted());
    JS_ASSERT(isFunctionFrame());
    return CalleeTokenToFunction(calleeToken());
}

JSFunction*
JitFrameIterator::maybeCallee() const
{
    if (isScripted() && (isFunctionFrame()))
        return callee();
    return nullptr;
}

bool
JitFrameIterator::isNative() const
{
    if (type_ != JitFrame_Exit || isFakeExitFrame())
        return false;
    return exitFrame()->footer()->jitCode() == nullptr;
}

bool
JitFrameIterator::isOOLNative() const
{
    if (type_ != JitFrame_Exit)
        return false;
    return exitFrame()->footer()->jitCode() == ION_FRAME_OOL_NATIVE;
}

bool
JitFrameIterator::isOOLPropertyOp() const
{
    if (type_ != JitFrame_Exit)
        return false;
    return exitFrame()->footer()->jitCode() == ION_FRAME_OOL_PROPERTY_OP;
}

bool
JitFrameIterator::isOOLProxy() const
{
    if (type_ != JitFrame_Exit)
        return false;
    return exitFrame()->footer()->jitCode() == ION_FRAME_OOL_PROXY;
}

bool
JitFrameIterator::isDOMExit() const
{
    if (type_ != JitFrame_Exit)
        return false;
    return exitFrame()->isDomExit();
}

bool
JitFrameIterator::isFunctionFrame() const
{
    return CalleeTokenIsFunction(calleeToken());
}

JSScript*
JitFrameIterator::script() const
{
    JS_ASSERT(isScripted());
    if (isBaselineJS())
        return baselineFrame()->script();
    JSScript* script = ScriptFromCalleeToken(calleeToken());
    JS_ASSERT(script);
    return script;
}

void
JitFrameIterator::baselineScriptAndPc(JSScript** scriptRes, jsbytecode** pcRes) const
{
    JS_ASSERT(isBaselineJS());
    JSScript* script = this->script();
    if (scriptRes)
        *scriptRes = script;
    uint8_t* retAddr = returnAddressToFp();

    // If we are in the middle of a recompile handler, get the real return
    // address as stashed in the RecompileInfo.
    if (BaselineDebugModeOSRInfo* info = baselineFrame()->getDebugModeOSRInfo())
        retAddr = info->resumeAddr;

    if (pcRes) {
        // If the return address is into the prologue entry address or just
        // after the debug prologue, then assume start of script.
        if (retAddr == script->baselineScript()->prologueEntryAddr() ||
            retAddr == script->baselineScript()->postDebugPrologueAddr())
        {
            *pcRes = script->code();
            return;
        }

        // The return address _may_ be a return from a callVM or IC chain call done for
        // some op.
        ICEntry* icEntry = script->baselineScript()->maybeICEntryFromReturnAddress(retAddr);
        if (icEntry) {
            *pcRes = icEntry->pc(script);
            return;
        }

        // If not, the return address _must_ be the start address of an op, which can
        // be computed from the pc mapping table.
        *pcRes = script->baselineScript()->pcForReturnAddress(script, retAddr);
    }
}

Value*
JitFrameIterator::actualArgs() const
{
    return jsFrame()->argv() + 1;
}

static inline size_t
SizeOfFramePrefix(FrameType type)
{
    switch (type) {
      case JitFrame_Entry:
        return IonEntryFrameLayout::Size();
      case JitFrame_BaselineJS:
      case JitFrame_IonJS:
      case JitFrame_Unwound_IonJS:
        return IonJSFrameLayout::Size();
      case JitFrame_BaselineStub:
        return IonBaselineStubFrameLayout::Size();
      case JitFrame_Rectifier:
        return IonRectifierFrameLayout::Size();
      case JitFrame_Unwound_Rectifier:
        return IonUnwoundRectifierFrameLayout::Size();
      case JitFrame_Exit:
        return IonExitFrameLayout::Size();
      default:
        MOZ_ASSUME_UNREACHABLE("unknown frame type");
    }
}

uint8_t*
JitFrameIterator::prevFp() const
{
    size_t currentSize = SizeOfFramePrefix(type_);
    // This quick fix must be removed as soon as bug 717297 land.  This is
    // needed because the descriptor size of JS-to-JS frame which is just after
    // a Rectifier frame should not change. (cf EnsureExitFrame function)
    if (isFakeExitFrame()) {
        JS_ASSERT(SizeOfFramePrefix(JitFrame_BaselineJS) ==
                  SizeOfFramePrefix(JitFrame_IonJS));
        currentSize = SizeOfFramePrefix(JitFrame_IonJS);
    }
    currentSize += current()->prevFrameLocalSize();
    return current_ + currentSize;
}

JitFrameIterator&
JitFrameIterator::operator++()
{
    JS_ASSERT(type_ != JitFrame_Entry);

    frameSize_ = prevFrameLocalSize();
    cachedSafepointIndex_ = nullptr;

    // If the next frame is the entry frame, just exit. Don't update current_,
    // since the entry and first frames overlap.
    if (current()->prevType() == JitFrame_Entry) {
        type_ = JitFrame_Entry;
        return *this;
    }

    // Note: prevFp() needs the current type, so set it after computing the
    // next frame.
    uint8_t* prev = prevFp();
    type_ = current()->prevType();
    if (type_ == JitFrame_Unwound_IonJS)
        type_ = JitFrame_IonJS;
    else if (type_ == JitFrame_Unwound_BaselineStub)
        type_ = JitFrame_BaselineStub;
    returnAddressToFp_ = current()->returnAddress();
    current_ = prev;
    return *this;
}

uintptr_t*
JitFrameIterator::spillBase() const
{
    // Get the base address to where safepoint registers are spilled.
    // Out-of-line calls do not unwind the extra padding space used to
    // aggregate bailout tables, so we use frameSize instead of frameLocals,
    // which would only account for local stack slots.
    return reinterpret_cast<uintptr_t*>(fp() - ionScript()->frameSize());
}

MachineState
JitFrameIterator::machineState() const
{
    SafepointReader reader(ionScript(), safepoint());
    uintptr_t* spill = spillBase();

    MachineState machine;
    for (GeneralRegisterBackwardIterator iter(reader.allGprSpills()); iter.more(); iter++)
        machine.setRegisterLocation(*iter, --spill);

    double* floatSpill = reinterpret_cast<double*>(spill);
    for (FloatRegisterBackwardIterator iter(reader.allFloatSpills()); iter.more(); iter++)
        machine.setRegisterLocation(*iter, --floatSpill);

    return machine;
}

static uint32_t
NumArgAndLocalSlots(const InlineFrameIterator& frame)
{
    JSScript* script = frame.script();
    return CountArgSlots(script, frame.maybeCallee()) + script->nfixed();
}

static void
CloseLiveIterator(JSContext* cx, const InlineFrameIterator& frame, uint32_t stackSlot)
{
    SnapshotIterator si = frame.snapshotIterator();

    // Skip stack slots until we reach the iterator object.
    uint32_t skipSlots = NumArgAndLocalSlots(frame) + stackSlot - 1;

    for (unsigned i = 0; i < skipSlots; i++)
        si.skip();

    Value v = si.read();
    RootedObject obj(cx, &v.toObject());

    if (cx->isExceptionPending())
        UnwindIteratorForException(cx, obj);
    else
        UnwindIteratorForUncatchableException(cx, obj);
}

static void
HandleExceptionIon(JSContext* cx, const InlineFrameIterator& frame, ResumeFromException* rfe,
                   bool* overrecursed)
{
    RootedScript script(cx, frame.script());
    jsbytecode* pc = frame.pc();

    bool bailedOutForDebugMode = false;
    if (cx->compartment()->debugMode()) {
        // If we have an exception from within Ion and the debugger is active,
        // we do the following:
        //
        //   1. Bailout to baseline to reconstruct a baseline frame.
        //   2. Resume immediately into the exception tail afterwards, and
        //   handle the exception again with the top frame now a baseline
        //   frame.
        //
        // An empty exception info denotes that we're propagating an Ion
        // exception due to debug mode, which BailoutIonToBaseline needs to
        // know. This is because we might not be able to fully reconstruct up
        // to the stack depth at the snapshot, as we could've thrown in the
        // middle of a call.
        ExceptionBailoutInfo propagateInfo;
        uint32_t retval = ExceptionHandlerBailout(cx, frame, rfe, propagateInfo, overrecursed);
        bailedOutForDebugMode = retval == BAILOUT_RETURN_OK;
    }

    if (!script->hasTrynotes())
        return;

    uint32_t base = NumArgAndLocalSlots(frame);
    SnapshotIterator si = frame.snapshotIterator();
    JS_ASSERT(si.numAllocations() >= base);
    const uint32_t stackDepth = si.numAllocations() - base;

    JSTryNote* tn = script->trynotes()->vector;
    JSTryNote* tnEnd = tn + script->trynotes()->length;

    uint32_t pcOffset = uint32_t(pc - script->main());
    for (; tn != tnEnd; ++tn) {
        if (pcOffset < tn->start)
            continue;
        if (pcOffset >= tn->start + tn->length)
            continue;

        if (tn->stackDepth > stackDepth)
            continue;

        switch (tn->kind) {
          case JSTRY_ITER: {
            JS_ASSERT(JSOp(*(script->main() + tn->start + tn->length)) == JSOP_ENDITER);
            JS_ASSERT(tn->stackDepth > 0);

            uint32_t localSlot = tn->stackDepth;
            CloseLiveIterator(cx, frame, localSlot);
            break;
          }

          case JSTRY_LOOP:
            break;

          case JSTRY_CATCH:
            if (cx->isExceptionPending() && !bailedOutForDebugMode) {
                // Ion can compile try-catch, but bailing out to catch
                // exceptions is slow. Reset the use count so that if we
                // catch many exceptions we won't Ion-compile the script.
                script->resetUseCount();

                // Bailout at the start of the catch block.
                jsbytecode* catchPC = script->main() + tn->start + tn->length;
                ExceptionBailoutInfo excInfo(frame.frameNo(), catchPC, tn->stackDepth);
                uint32_t retval = ExceptionHandlerBailout(cx, frame, rfe, excInfo, overrecursed);
                if (retval == BAILOUT_RETURN_OK)
                    return;

                // Error on bailout clears pending exception.
                MOZ_ASSERT(!cx->isExceptionPending());
            }
            break;

          default:
            MOZ_ASSUME_UNREACHABLE("Unexpected try note");
        }
    }
}

static void
HandleExceptionBaseline(JSContext* cx, const JitFrameIterator& frame, ResumeFromException* rfe,
                        bool* calledDebugEpilogue)
{
    JS_ASSERT(frame.isBaselineJS());
    JS_ASSERT(!*calledDebugEpilogue);

    RootedScript script(cx);
    jsbytecode* pc;
    frame.baselineScriptAndPc(script.address(), &pc);

    if (cx->isExceptionPending() && cx->compartment()->debugMode()) {
        BaselineFrame* baselineFrame = frame.baselineFrame();
        JSTrapStatus status = DebugExceptionUnwind(cx, baselineFrame, pc);
        switch (status) {
          case JSTRAP_ERROR:
            // Uncatchable exception.
            JS_ASSERT(!cx->isExceptionPending());
            break;

          case JSTRAP_CONTINUE:
          case JSTRAP_THROW:
            JS_ASSERT(cx->isExceptionPending());
            break;

          case JSTRAP_RETURN:
            JS_ASSERT(baselineFrame->hasReturnValue());
            if (jit::DebugEpilogue(cx, baselineFrame, pc, true)) {
                rfe->kind = ResumeFromException::RESUME_FORCED_RETURN;
                rfe->framePointer = frame.fp() - BaselineFrame::FramePointerOffset;
                rfe->stackPointer = reinterpret_cast<uint8_t*>(baselineFrame);
                return;
            }

            // DebugEpilogue threw an exception. Propagate to the caller frame.
            *calledDebugEpilogue = true;
            return;

          default:
            MOZ_ASSUME_UNREACHABLE("Invalid trap status");
        }
    }

    if (!script->hasTrynotes())
        return;

    JSTryNote* tn = script->trynotes()->vector;
    JSTryNote* tnEnd = tn + script->trynotes()->length;

    uint32_t pcOffset = uint32_t(pc - script->main());
    ScopeIter si(frame.baselineFrame(), pc, cx);
    for (; tn != tnEnd; ++tn) {
        if (pcOffset < tn->start)
            continue;
        if (pcOffset >= tn->start + tn->length)
            continue;

        // Skip if the try note's stack depth exceeds the frame's stack depth.
        // See the big comment in TryNoteIter::settle for more info.
        JS_ASSERT(frame.baselineFrame()->numValueSlots() >= script->nfixed());
        size_t stackDepth = frame.baselineFrame()->numValueSlots() - script->nfixed();
        if (tn->stackDepth > stackDepth)
            continue;

        // Unwind scope chain (pop block objects).
        if (cx->isExceptionPending())
            UnwindScope(cx, si, script->main() + tn->start);

        // Compute base pointer and stack pointer.
        rfe->framePointer = frame.fp() - BaselineFrame::FramePointerOffset;
        rfe->stackPointer = rfe->framePointer - BaselineFrame::Size() -
            (script->nfixed() + tn->stackDepth) * sizeof(Value);

        switch (tn->kind) {
          case JSTRY_CATCH:
            if (cx->isExceptionPending()) {
                // Ion can compile try-catch, but bailing out to catch
                // exceptions is slow. Reset the use count so that if we
                // catch many exceptions we won't Ion-compile the script.
                script->resetUseCount();

                // Resume at the start of the catch block.
                rfe->kind = ResumeFromException::RESUME_CATCH;
                jsbytecode* catchPC = script->main() + tn->start + tn->length;
                rfe->target = script->baselineScript()->nativeCodeForPC(script, catchPC);
                return;
            }
            break;

          case JSTRY_FINALLY:
            if (cx->isExceptionPending()) {
                rfe->kind = ResumeFromException::RESUME_FINALLY;
                jsbytecode* finallyPC = script->main() + tn->start + tn->length;
                rfe->target = script->baselineScript()->nativeCodeForPC(script, finallyPC);
                // Drop the exception instead of leaking cross compartment data.
                if (!cx->getPendingException(MutableHandleValue::fromMarkedLocation(&rfe->exception)))
                    rfe->exception = UndefinedValue();
                cx->clearPendingException();
                return;
            }
            break;

          case JSTRY_ITER: {
            Value iterValue(* (Value*) rfe->stackPointer);
            RootedObject iterObject(cx, &iterValue.toObject());
            if (cx->isExceptionPending())
                UnwindIteratorForException(cx, iterObject);
            else
                UnwindIteratorForUncatchableException(cx, iterObject);
            break;
          }

          case JSTRY_LOOP:
            break;

          default:
            MOZ_ASSUME_UNREACHABLE("Invalid try note");
        }
    }

}

struct AutoDeleteDebugModeOSRInfo
{
    BaselineFrame* frame;
    AutoDeleteDebugModeOSRInfo(BaselineFrame* frame) : frame(frame) { MOZ_ASSERT(frame); }
    ~AutoDeleteDebugModeOSRInfo() { frame->deleteDebugModeOSRInfo(); }
};

void
HandleException(ResumeFromException* rfe)
{
    JSContext* cx = GetJSContextFromJitCode();

    rfe->kind = ResumeFromException::RESUME_ENTRY_FRAME;

    IonSpew(IonSpew_Invalidate, "handling exception");

    // Clear any Ion return override that's been set.
    // This may happen if a callVM function causes an invalidation (setting the
    // override), and then fails, bypassing the bailout handlers that would
    // otherwise clear the return override.
    if (cx->runtime()->hasIonReturnOverride())
        cx->runtime()->takeIonReturnOverride();

    JitFrameIterator iter(cx);
    while (!iter.isEntry()) {
        bool overrecursed = false;
        if (iter.isIonJS()) {
            // Search each inlined frame for live iterator objects, and close
            // them.
            InlineFrameIterator frames(cx, &iter);

            // Invalidation state will be the same for all inlined scripts in the frame.
            IonScript* ionScript = nullptr;
            bool invalidated = iter.checkInvalidation(&ionScript);

            for (;;) {
                HandleExceptionIon(cx, frames, rfe, &overrecursed);

                if (rfe->kind == ResumeFromException::RESUME_BAILOUT) {
                    if (invalidated)
                        ionScript->decref(cx->runtime()->defaultFreeOp());
                    return;
                }

                JS_ASSERT(rfe->kind == ResumeFromException::RESUME_ENTRY_FRAME);

                // Figure out whether SPS frame was pushed for this frame or not.
                // Even if profiler is enabled, the frame being popped might have
                // been entered prior to SPS being enabled, and thus not have
                // a pushed SPS frame.
                bool popSPSFrame = cx->runtime()->spsProfiler.enabled();
                if (invalidated)
                    popSPSFrame = ionScript->hasSPSInstrumentation();

                // If inline-frames are not profiled, then don't pop an SPS frame
                // for them.
                if (frames.more() && !js_JitOptions.profileInlineFrames)
                    popSPSFrame = false;

                // When profiling, each frame popped needs a notification that
                // the function has exited, so invoke the probe that a function
                // is exiting.
                JSScript* script = frames.script();
                probes::ExitScript(cx, script, script->functionNonDelazifying(), popSPSFrame);
                if (!frames.more())
                    break;
                ++frames;
            }

            if (invalidated)
                ionScript->decref(cx->runtime()->defaultFreeOp());

        } else if (iter.isBaselineJS()) {
            // It's invalid to call DebugEpilogue twice for the same frame.
            bool calledDebugEpilogue = false;

            HandleExceptionBaseline(cx, iter, rfe, &calledDebugEpilogue);

            // If we are propagating an exception through a frame with
            // on-stack recompile info, we should free the allocated
            // RecompileInfo struct before we leave this block, as we will not
            // be returning to the recompile handler.
            //
            // We cannot delete it immediately because of the call to
            // iter.baselineScriptAndPc below.
            AutoDeleteDebugModeOSRInfo deleteDebugModeOSRInfo(iter.baselineFrame());

            if (rfe->kind != ResumeFromException::RESUME_ENTRY_FRAME)
                return;

            // Unwind profiler pseudo-stack
            JSScript* script = iter.script();
            probes::ExitScript(cx, script, script->functionNonDelazifying(),
                               iter.baselineFrame()->hasPushedSPSFrame());
            // After this point, any pushed SPS frame would have been popped if it needed
            // to be.  Unset the flag here so that if we call DebugEpilogue below,
            // it doesn't try to pop the SPS frame again.
            iter.baselineFrame()->unsetPushedSPSFrame();

            if (cx->compartment()->debugMode() && !calledDebugEpilogue) {
                // If DebugEpilogue returns |true|, we have to perform a forced
                // return, e.g. return frame->returnValue() to the caller.
                BaselineFrame* frame = iter.baselineFrame();
                RootedScript script(cx);
                jsbytecode* pc;
                iter.baselineScriptAndPc(script.address(), &pc);
                if (jit::DebugEpilogue(cx, frame, pc, false)) {
                    JS_ASSERT(frame->hasReturnValue());
                    rfe->kind = ResumeFromException::RESUME_FORCED_RETURN;
                    rfe->framePointer = iter.fp() - BaselineFrame::FramePointerOffset;
                    rfe->stackPointer = reinterpret_cast<uint8_t*>(frame);
                    return;
                }
            }
        }

        IonJSFrameLayout* current = iter.isScripted() ? iter.jsFrame() : nullptr;

        ++iter;

        if (current) {
            // Unwind the frame by updating ionTop. This is necessary so that
            // (1) debugger exception unwind and leave frame hooks don't see this
            // frame when they use ScriptFrameIter, and (2) ScriptFrameIter does
            // not crash when accessing an IonScript that's destroyed by the
            // ionScript->decref call.
            EnsureExitFrame(current);
            cx->mainThread().ionTop = (uint8_t*)current;
        }

        if (overrecursed) {
            // We hit an overrecursion error during bailout. Report it now.
            js_ReportOverRecursed(cx);
        }
    }

    rfe->stackPointer = iter.fp();
}

void
HandleParallelFailure(ResumeFromException* rfe)
{
    ForkJoinContext* cx = ForkJoinContext::current();
    JitFrameIterator iter(cx->perThreadData->ionTop, ParallelExecution);

    parallel::Spew(parallel::SpewBailouts, "Bailing from VM reentry");

    while (!iter.isEntry()) {
        if (iter.isScripted()) {
            cx->bailoutRecord->updateCause(ParallelBailoutUnsupportedVM,
                                           iter.script(), iter.script(), nullptr);
            break;
        }
        ++iter;
    }

    while (!iter.isEntry()) {
        if (iter.isScripted())
            PropagateAbortPar(iter.script(), iter.script());
        ++iter;
    }

    rfe->kind = ResumeFromException::RESUME_ENTRY_FRAME;
    rfe->stackPointer = iter.fp();
}

void
EnsureExitFrame(IonCommonFrameLayout* frame)
{
    if (frame->prevType() == JitFrame_Unwound_IonJS ||
        frame->prevType() == JitFrame_Unwound_BaselineStub ||
        frame->prevType() == JitFrame_Unwound_Rectifier)
    {
        // Already an exit frame, nothing to do.
        return;
    }

    if (frame->prevType() == JitFrame_Entry) {
        // The previous frame type is the entry frame, so there's no actual
        // need for an exit frame.
        return;
    }

    if (frame->prevType() == JitFrame_Rectifier) {
        // The rectifier code uses the frame descriptor to discard its stack,
        // so modifying its descriptor size here would be dangerous. Instead,
        // we change the frame type, and teach the stack walking code how to
        // deal with this edge case. bug 717297 would obviate the need
        frame->changePrevType(JitFrame_Unwound_Rectifier);
        return;
    }

    if (frame->prevType() == JitFrame_BaselineStub) {
        frame->changePrevType(JitFrame_Unwound_BaselineStub);
        return;
    }

    JS_ASSERT(frame->prevType() == JitFrame_IonJS);
    frame->changePrevType(JitFrame_Unwound_IonJS);
}

CalleeToken
MarkCalleeToken(JSTracer* trc, CalleeToken token)
{
    switch (GetCalleeTokenTag(token)) {
      case CalleeToken_Function:
      {
        JSFunction* fun = CalleeTokenToFunction(token);
        MarkObjectRoot(trc, &fun, "ion-callee");
        return CalleeToToken(fun);
      }
      case CalleeToken_Script:
      {
        JSScript* script = CalleeTokenToScript(token);
        MarkScriptRoot(trc, &script, "ion-entry");
        return CalleeToToken(script);
      }
      default:
        MOZ_ASSUME_UNREACHABLE("unknown callee token type");
    }
}

#ifdef JS_NUNBOX32
static inline uintptr_t
ReadAllocation(const JitFrameIterator& frame, const LAllocation* a)
{
    if (a->isGeneralReg()) {
        Register reg = a->toGeneralReg()->reg();
        return frame.machineState().read(reg);
    }
    if (a->isStackSlot()) {
        uint32_t slot = a->toStackSlot()->slot();
        return *frame.jsFrame()->slotRef(slot);
    }
    uint32_t index = a->toArgument()->index();
    uint8_t* argv = reinterpret_cast<uint8_t*>(frame.jsFrame()->argv());
    return *reinterpret_cast<uintptr_t*>(argv + index);
}
#endif

static void
MarkActualArguments(JSTracer* trc, const JitFrameIterator& frame)
{
    IonJSFrameLayout* layout = frame.jsFrame();
    JS_ASSERT(CalleeTokenIsFunction(layout->calleeToken()));

    size_t nargs = frame.numActualArgs();

    // Trace function arguments. Note + 1 for thisv.
    Value* argv = layout->argv();
    for (size_t i = 0; i < nargs + 1; i++)
        gc::MarkValueRoot(trc, &argv[i], "ion-argv");
}

#ifdef JS_NUNBOX32
static inline void
WriteAllocation(const JitFrameIterator& frame, const LAllocation* a, uintptr_t value)
{
    if (a->isGeneralReg()) {
        Register reg = a->toGeneralReg()->reg();
        frame.machineState().write(reg, value);
        return;
    }
    if (a->isStackSlot()) {
        uint32_t slot = a->toStackSlot()->slot();
        *frame.jsFrame()->slotRef(slot) = value;
        return;
    }
    uint32_t index = a->toArgument()->index();
    uint8_t* argv = reinterpret_cast<uint8_t*>(frame.jsFrame()->argv());
    *reinterpret_cast<uintptr_t*>(argv + index) = value;
}
#endif

static void
MarkIonJSFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    IonJSFrameLayout* layout = (IonJSFrameLayout*)frame.fp();

    layout->replaceCalleeToken(MarkCalleeToken(trc, layout->calleeToken()));

    IonScript* ionScript = nullptr;
    if (frame.checkInvalidation(&ionScript)) {
        // This frame has been invalidated, meaning that its IonScript is no
        // longer reachable through the callee token (JSFunction/JSScript->ion
        // is now nullptr or recompiled). Manually trace it here.
        IonScript::Trace(trc, ionScript);
    } else if (CalleeTokenIsFunction(layout->calleeToken())) {
        ionScript = CalleeTokenToFunction(layout->calleeToken())->nonLazyScript()->ionScript();
    } else {
        ionScript = CalleeTokenToScript(layout->calleeToken())->ionScript();
    }

    if (CalleeTokenIsFunction(layout->calleeToken()))
        MarkActualArguments(trc, frame);

    const SafepointIndex* si = ionScript->getSafepointIndex(frame.returnAddressToFp());

    SafepointReader safepoint(ionScript, si);

    // Scan through slots which contain pointers (or on punboxing systems,
    // actual values).
    uint32_t slot;
    while (safepoint.getGcSlot(&slot)) {
        uintptr_t* ref = layout->slotRef(slot);
        gc::MarkGCThingRoot(trc, reinterpret_cast<void**>(ref), "ion-gc-slot");
    }

    while (safepoint.getValueSlot(&slot)) {
        Value* v = (Value*)layout->slotRef(slot);
        gc::MarkValueRoot(trc, v, "ion-gc-slot");
    }

    uintptr_t* spill = frame.spillBase();
    GeneralRegisterSet gcRegs = safepoint.gcSpills();
    GeneralRegisterSet valueRegs = safepoint.valueSpills();
    for (GeneralRegisterBackwardIterator iter(safepoint.allGprSpills()); iter.more(); iter++) {
        --spill;
        if (gcRegs.has(*iter))
            gc::MarkGCThingRoot(trc, reinterpret_cast<void**>(spill), "ion-gc-spill");
        else if (valueRegs.has(*iter))
            gc::MarkValueRoot(trc, reinterpret_cast<Value*>(spill), "ion-value-spill");
    }

#ifdef JS_NUNBOX32
    LAllocation type, payload;
    while (safepoint.getNunboxSlot(&type, &payload)) {
        jsval_layout layout;
        layout.s.tag = (JSValueTag)ReadAllocation(frame, &type);
        layout.s.payload.uintptr = ReadAllocation(frame, &payload);

        Value v = IMPL_TO_JSVAL(layout);
        gc::MarkValueRoot(trc, &v, "ion-torn-value");

        if (v != IMPL_TO_JSVAL(layout)) {
            // GC moved the value, replace the stored payload.
            layout = JSVAL_TO_IMPL(v);
            WriteAllocation(frame, &payload, layout.s.payload.uintptr);
        }
    }
#endif
}

#ifdef JSGC_GENERATIONAL
static void
UpdateIonJSFrameForMinorGC(JSTracer* trc, const JitFrameIterator& frame)
{
    // Minor GCs may move slots/elements allocated in the nursery. Update
    // any slots/elements pointers stored in this frame.

    IonJSFrameLayout* layout = (IonJSFrameLayout*)frame.fp();

    IonScript* ionScript = nullptr;
    if (frame.checkInvalidation(&ionScript)) {
        // This frame has been invalidated, meaning that its IonScript is no
        // longer reachable through the callee token (JSFunction/JSScript->ion
        // is now nullptr or recompiled).
    } else if (CalleeTokenIsFunction(layout->calleeToken())) {
        ionScript = CalleeTokenToFunction(layout->calleeToken())->nonLazyScript()->ionScript();
    } else {
        ionScript = CalleeTokenToScript(layout->calleeToken())->ionScript();
    }

    const SafepointIndex* si = ionScript->getSafepointIndex(frame.returnAddressToFp());
    SafepointReader safepoint(ionScript, si);

    GeneralRegisterSet slotsRegs = safepoint.slotsOrElementsSpills();
    uintptr_t* spill = frame.spillBase();
    for (GeneralRegisterBackwardIterator iter(safepoint.allGprSpills()); iter.more(); iter++) {
        --spill;
        if (slotsRegs.has(*iter))
            trc->runtime()->gcNursery.forwardBufferPointer(reinterpret_cast<HeapSlot**>(spill));
    }

    // Skip to the right place in the safepoint
    uint32_t slot;
    while (safepoint.getGcSlot(&slot));
    while (safepoint.getValueSlot(&slot));
#ifdef JS_NUNBOX32
    LAllocation type, payload;
    while (safepoint.getNunboxSlot(&type, &payload));
#endif

    while (safepoint.getSlotsOrElementsSlot(&slot)) {
        HeapSlot** slots = reinterpret_cast<HeapSlot**>(layout->slotRef(slot));
        trc->runtime()->gcNursery.forwardBufferPointer(slots);
    }
}
#endif

static void
MarkBaselineStubFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    // Mark the ICStub pointer stored in the stub frame. This is necessary
    // so that we don't destroy the stub code after unlinking the stub.

    JS_ASSERT(frame.type() == JitFrame_BaselineStub);
    IonBaselineStubFrameLayout* layout = (IonBaselineStubFrameLayout*)frame.fp();

    if (ICStub* stub = layout->maybeStubPtr()) {
        JS_ASSERT(ICStub::CanMakeCalls(stub->kind()));
        stub->trace(trc);
    }
}

void
JitActivationIterator::jitStackRange(uintptr_t*& min, uintptr_t*& end)
{
    JitFrameIterator frames(jitTop(), SequentialExecution);

    if (frames.isFakeExitFrame()) {
        min = reinterpret_cast<uintptr_t*>(frames.fp());
    } else {
        IonExitFrameLayout* exitFrame = frames.exitFrame();
        IonExitFooterFrame* footer = exitFrame->footer();
        const VMFunction* f = footer->function();
        if (exitFrame->isWrapperExit() && f->outParam == Type_Handle) {
            switch (f->outParamRootType) {
              case VMFunction::RootNone:
                MOZ_ASSUME_UNREACHABLE("Handle outparam must have root type");
              case VMFunction::RootObject:
              case VMFunction::RootString:
              case VMFunction::RootPropertyName:
              case VMFunction::RootFunction:
              case VMFunction::RootCell:
                // These are all handles to GCThing pointers.
                min = reinterpret_cast<uintptr_t*>(footer->outParam<void*>());
                break;
              case VMFunction::RootValue:
                min = reinterpret_cast<uintptr_t*>(footer->outParam<Value>());
                break;
            }
        } else {
            min = reinterpret_cast<uintptr_t*>(footer);
        }
    }

    while (!frames.done())
        ++frames;

    end = reinterpret_cast<uintptr_t*>(frames.prevFp());
}

static void
MarkJitExitFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    // Ignore fake exit frames created by EnsureExitFrame.
    if (frame.isFakeExitFrame())
        return;

    IonExitFooterFrame* footer = frame.exitFrame()->footer();

    // Mark the code of the code handling the exit path.  This is needed because
    // invalidated script are no longer marked because data are erased by the
    // invalidation and relocation data are no longer reliable.  So the VM
    // wrapper or the invalidation code may be GC if no JitCode keep reference
    // on them.
    JS_ASSERT(uintptr_t(footer->jitCode()) != uintptr_t(-1));

    // This correspond to the case where we have build a fake exit frame in
    // CodeGenerator.cpp which handle the case of a native function call. We
    // need to mark the argument vector of the function call.
    if (frame.isNative()) {
        IonNativeExitFrameLayout* native = frame.exitFrame()->nativeExit();
        size_t len = native->argc() + 2;
        Value* vp = native->vp();
        gc::MarkValueRootRange(trc, len, vp, "ion-native-args");
        return;
    }

    if (frame.isOOLNative()) {
        IonOOLNativeExitFrameLayout* oolnative = frame.exitFrame()->oolNativeExit();
        gc::MarkJitCodeRoot(trc, oolnative->stubCode(), "ion-ool-native-code");
        gc::MarkValueRoot(trc, oolnative->vp(), "iol-ool-native-vp");
        size_t len = oolnative->argc() + 1;
        gc::MarkValueRootRange(trc, len, oolnative->thisp(), "ion-ool-native-thisargs");
        return;
    }

    if (frame.isOOLPropertyOp()) {
        IonOOLPropertyOpExitFrameLayout* oolgetter = frame.exitFrame()->oolPropertyOpExit();
        gc::MarkJitCodeRoot(trc, oolgetter->stubCode(), "ion-ool-property-op-code");
        gc::MarkValueRoot(trc, oolgetter->vp(), "ion-ool-property-op-vp");
        gc::MarkIdRoot(trc, oolgetter->id(), "ion-ool-property-op-id");
        gc::MarkObjectRoot(trc, oolgetter->obj(), "ion-ool-property-op-obj");
        return;
    }

    if (frame.isOOLProxy()) {
        IonOOLProxyExitFrameLayout* oolproxy = frame.exitFrame()->oolProxyExit();
        gc::MarkJitCodeRoot(trc, oolproxy->stubCode(), "ion-ool-proxy-code");
        gc::MarkValueRoot(trc, oolproxy->vp(), "ion-ool-proxy-vp");
        gc::MarkIdRoot(trc, oolproxy->id(), "ion-ool-proxy-id");
        gc::MarkObjectRoot(trc, oolproxy->proxy(), "ion-ool-proxy-proxy");
        gc::MarkObjectRoot(trc, oolproxy->receiver(), "ion-ool-proxy-receiver");
        return;
    }

    if (frame.isDOMExit()) {
        IonDOMExitFrameLayout* dom = frame.exitFrame()->DOMExit();
        gc::MarkObjectRoot(trc, dom->thisObjAddress(), "ion-dom-args");
        if (dom->isMethodFrame()) {
            IonDOMMethodExitFrameLayout* method =
                reinterpret_cast<IonDOMMethodExitFrameLayout*>(dom);
            size_t len = method->argc() + 2;
            Value* vp = method->vp();
            gc::MarkValueRootRange(trc, len, vp, "ion-dom-args");
        } else {
            gc::MarkValueRoot(trc, dom->vp(), "ion-dom-args");
        }
        return;
    }

    MarkJitCodeRoot(trc, footer->addressOfJitCode(), "ion-exit-code");

    const VMFunction* f = footer->function();
    if (f == nullptr)
        return;

    // Mark arguments of the VM wrapper.
    uint8_t* argBase = frame.exitFrame()->argBase();
    for (uint32_t explicitArg = 0; explicitArg < f->explicitArgs; explicitArg++) {
        switch (f->argRootType(explicitArg)) {
          case VMFunction::RootNone:
            break;
          case VMFunction::RootObject: {
            // Sometimes we can bake in HandleObjects to nullptr.
            JSObject** pobj = reinterpret_cast<JSObject**>(argBase);
            if (*pobj)
                gc::MarkObjectRoot(trc, pobj, "ion-vm-args");
            break;
          }
          case VMFunction::RootString:
          case VMFunction::RootPropertyName:
            gc::MarkStringRoot(trc, reinterpret_cast<JSString**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootFunction:
            gc::MarkObjectRoot(trc, reinterpret_cast<JSFunction**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootValue:
            gc::MarkValueRoot(trc, reinterpret_cast<Value*>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootCell:
            gc::MarkGCThingRoot(trc, reinterpret_cast<void**>(argBase), "ion-vm-args");
            break;
        }

        switch (f->argProperties(explicitArg)) {
          case VMFunction::WordByValue:
          case VMFunction::WordByRef:
            argBase += sizeof(void*);
            break;
          case VMFunction::DoubleByValue:
          case VMFunction::DoubleByRef:
            argBase += 2 * sizeof(void*);
            break;
        }
    }

    if (f->outParam == Type_Handle) {
        switch (f->outParamRootType) {
          case VMFunction::RootNone:
            MOZ_ASSUME_UNREACHABLE("Handle outparam must have root type");
          case VMFunction::RootObject:
            gc::MarkObjectRoot(trc, footer->outParam<JSObject*>(), "ion-vm-out");
            break;
          case VMFunction::RootString:
          case VMFunction::RootPropertyName:
            gc::MarkStringRoot(trc, footer->outParam<JSString*>(), "ion-vm-out");
            break;
          case VMFunction::RootFunction:
            gc::MarkObjectRoot(trc, footer->outParam<JSFunction*>(), "ion-vm-out");
            break;
          case VMFunction::RootValue:
            gc::MarkValueRoot(trc, footer->outParam<Value>(), "ion-vm-outvp");
            break;
          case VMFunction::RootCell:
            gc::MarkGCThingRoot(trc, footer->outParam<void*>(), "ion-vm-out");
            break;
        }
    }
}

static void
MarkRectifierFrame(JSTracer* trc, const JitFrameIterator& frame)
{
    // Mark thisv.
    //
    // Baseline JIT code generated as part of the ICCall_Fallback stub may use
    // it if we're calling a constructor that returns a primitive value.
    IonRectifierFrameLayout* layout = (IonRectifierFrameLayout*)frame.fp();
    gc::MarkValueRoot(trc, &layout->argv()[0], "ion-thisv");
}

static void
MarkJitActivation(JSTracer* trc, const JitActivationIterator& activations)
{
    JitActivation* activation = activations->asJit();

#ifdef CHECK_OSIPOINT_REGISTERS
    if (js_JitOptions.checkOsiPointRegisters) {
        // GC can modify spilled registers, breaking our register checks.
        // To handle this, we disable these checks for the current VM call
        // when a GC happens.
        activation->setCheckRegs(false);
    }
#endif

    activation->markRematerializedFrames(trc);

    for (JitFrameIterator frames(activations); !frames.done(); ++frames) {
        switch (frames.type()) {
          case JitFrame_Exit:
            MarkJitExitFrame(trc, frames);
            break;
          case JitFrame_BaselineJS:
            frames.baselineFrame()->trace(trc, frames);
            break;
          case JitFrame_BaselineStub:
            MarkBaselineStubFrame(trc, frames);
            break;
          case JitFrame_IonJS:
            MarkIonJSFrame(trc, frames);
            break;
          case JitFrame_Unwound_IonJS:
            MOZ_ASSUME_UNREACHABLE("invalid");
          case JitFrame_Rectifier:
            MarkRectifierFrame(trc, frames);
            break;
          case JitFrame_Unwound_Rectifier:
            break;
          default:
            MOZ_ASSUME_UNREACHABLE("unexpected frame type");
        }
    }
}

void
MarkJitActivations(JSRuntime* rt, JSTracer* trc)
{
    for (JitActivationIterator activations(rt); !activations.done(); ++activations)
        MarkJitActivation(trc, activations);
}

#ifdef JSGC_GENERATIONAL
void
UpdateJitActivationsForMinorGC(JSRuntime* rt, JSTracer* trc)
{
    JS_ASSERT(trc->runtime()->isHeapMinorCollecting());
    for (JitActivationIterator activations(rt); !activations.done(); ++activations) {
        for (JitFrameIterator frames(activations); !frames.done(); ++frames) {
            if (frames.type() == JitFrame_IonJS)
                UpdateIonJSFrameForMinorGC(trc, frames);
        }
    }
}
#endif

void
AutoTempAllocatorRooter::trace(JSTracer* trc)
{
    for (CompilerRootNode* root = temp->rootList(); root != nullptr; root = root->next)
        gc::MarkGCThingRoot(trc, root->address(), "ion-compiler-root");
}

void
GetPcScript(JSContext* cx, JSScript** scriptRes, jsbytecode** pcRes)
{
    IonSpew(IonSpew_Snapshots, "Recover PC & Script from the last frame.");

    JSRuntime* rt = cx->runtime();

    // Recover the return address.
    JitFrameIterator it(rt->mainThread.ionTop, SequentialExecution);

    // If the previous frame is a rectifier frame (maybe unwound),
    // skip past it.
    if (it.prevType() == JitFrame_Rectifier || it.prevType() == JitFrame_Unwound_Rectifier) {
        ++it;
        JS_ASSERT(it.prevType() == JitFrame_BaselineStub ||
                  it.prevType() == JitFrame_BaselineJS ||
                  it.prevType() == JitFrame_IonJS);
    }

    // If the previous frame is a stub frame, skip the exit frame so that
    // returnAddress below gets the return address into the BaselineJS
    // frame.
    if (it.prevType() == JitFrame_BaselineStub || it.prevType() == JitFrame_Unwound_BaselineStub) {
        ++it;
        JS_ASSERT(it.prevType() == JitFrame_BaselineJS);
    }

    uint8_t* retAddr = it.returnAddress();
    uint32_t hash = PcScriptCache::Hash(retAddr);
    JS_ASSERT(retAddr != nullptr);

    // Lazily initialize the cache. The allocation may safely fail and will not GC.
    if (MOZ_UNLIKELY(rt->ionPcScriptCache == nullptr)) {
        rt->ionPcScriptCache = (PcScriptCache*)js_malloc(sizeof(struct PcScriptCache));
        if (rt->ionPcScriptCache)
            rt->ionPcScriptCache->clear(rt->gcNumber);
    }

    // Attempt to lookup address in cache.
    if (rt->ionPcScriptCache && rt->ionPcScriptCache->get(rt, hash, retAddr, scriptRes, pcRes))
        return;

    // Lookup failed: undertake expensive process to recover the innermost inlined frame.
    ++it; // Skip exit frame.
    jsbytecode* pc = nullptr;

    if (it.isIonJS()) {
        InlineFrameIterator ifi(cx, &it);
        *scriptRes = ifi.script();
        pc = ifi.pc();
    } else {
        JS_ASSERT(it.isBaselineJS());
        it.baselineScriptAndPc(scriptRes, &pc);
    }

    if (pcRes)
        *pcRes = pc;

    // Add entry to cache.
    if (rt->ionPcScriptCache)
        rt->ionPcScriptCache->add(hash, retAddr, pc, *scriptRes);
}

void
OsiIndex::fixUpOffset(MacroAssembler& masm)
{
    callPointDisplacement_ = masm.actualOffset(callPointDisplacement_);
}

uint32_t
OsiIndex::returnPointDisplacement() const
{
    // In general, pointer arithmetic on code is bad, but in this case,
    // getting the return address from a call instruction, stepping over pools
    // would be wrong.
    return callPointDisplacement_ + Assembler::patchWrite_NearCallSize();
}

SnapshotIterator::SnapshotIterator(IonScript* ionScript, SnapshotOffset snapshotOffset,
                                   IonJSFrameLayout* fp, const MachineState& machine)
  : snapshot_(ionScript->snapshots(),
              snapshotOffset,
              ionScript->snapshotsRVATableSize(),
              ionScript->snapshotsListSize()),
    recover_(snapshot_,
             ionScript->recovers(),
             ionScript->recoversSize()),
    fp_(fp),
    machine_(machine),
    ionScript_(ionScript)
{
    JS_ASSERT(snapshotOffset < ionScript->snapshotsListSize());
}

SnapshotIterator::SnapshotIterator(const JitFrameIterator& iter)
  : snapshot_(iter.ionScript()->snapshots(),
              iter.osiIndex()->snapshotOffset(),
              iter.ionScript()->snapshotsRVATableSize(),
              iter.ionScript()->snapshotsListSize()),
    recover_(snapshot_,
             iter.ionScript()->recovers(),
             iter.ionScript()->recoversSize()),
    fp_(iter.jsFrame()),
    machine_(iter.machineState()),
    ionScript_(iter.ionScript())
{
}

SnapshotIterator::SnapshotIterator()
  : snapshot_(nullptr, 0, 0, 0),
    recover_(snapshot_, nullptr, 0),
    fp_(nullptr),
    ionScript_(nullptr)
{
}

uintptr_t
SnapshotIterator::fromStack(int32_t offset) const
{
    return ReadFrameSlot(fp_, offset);
}

static Value
FromObjectPayload(uintptr_t payload)
{
    return ObjectValue(*reinterpret_cast<JSObject*>(payload));
}

static Value
FromStringPayload(uintptr_t payload)
{
    return StringValue(reinterpret_cast<JSString*>(payload));
}

static Value
FromTypedPayload(JSValueType type, uintptr_t payload)
{
    switch (type) {
      case JSVAL_TYPE_INT32:
        return Int32Value(payload);
      case JSVAL_TYPE_BOOLEAN:
        return BooleanValue(!!payload);
      case JSVAL_TYPE_STRING:
        return FromStringPayload(payload);
      case JSVAL_TYPE_OBJECT:
        return FromObjectPayload(payload);
      default:
        MOZ_ASSUME_UNREACHABLE("unexpected type - needs payload");
    }
}

bool
SnapshotIterator::allocationReadable(const RValueAllocation& alloc)
{
    switch (alloc.mode()) {
      case RValueAllocation::DOUBLE_REG:
        return hasRegister(alloc.fpuReg());

      case RValueAllocation::TYPED_REG:
        return hasRegister(alloc.reg2());

#if defined(JS_NUNBOX32)
      case RValueAllocation::UNTYPED_REG_REG:
        return hasRegister(alloc.reg()) && hasRegister(alloc.reg2());
      case RValueAllocation::UNTYPED_REG_STACK:
        return hasRegister(alloc.reg()) && hasStack(alloc.stackOffset2());
      case RValueAllocation::UNTYPED_STACK_REG:
        return hasStack(alloc.stackOffset()) && hasRegister(alloc.reg2());
      case RValueAllocation::UNTYPED_STACK_STACK:
        return hasStack(alloc.stackOffset()) && hasStack(alloc.stackOffset2());
#elif defined(JS_PUNBOX64)
      case RValueAllocation::UNTYPED_REG:
        return hasRegister(alloc.reg());
      case RValueAllocation::UNTYPED_STACK:
        return hasStack(alloc.stackOffset());
#endif

      default:
        return true;
    }
}

Value
SnapshotIterator::allocationValue(const RValueAllocation& alloc)
{
    switch (alloc.mode()) {
      case RValueAllocation::CONSTANT:
        return ionScript_->getConstant(alloc.index());

      case RValueAllocation::CST_UNDEFINED:
        return UndefinedValue();

      case RValueAllocation::CST_NULL:
        return NullValue();

      case RValueAllocation::DOUBLE_REG:
        return DoubleValue(fromRegister(alloc.fpuReg()));

      case RValueAllocation::FLOAT32_REG:
      {
        union {
            double d;
            float f;
        } pun;
        pun.d = fromRegister(alloc.fpuReg());
        // The register contains the encoding of a float32. We just read
        // the bits without making any conversion.
        return Float32Value(pun.f);
      }

      case RValueAllocation::FLOAT32_STACK:
        return Float32Value(ReadFrameFloat32Slot(fp_, alloc.stackOffset()));

      case RValueAllocation::TYPED_REG:
        return FromTypedPayload(alloc.knownType(), fromRegister(alloc.reg2()));

      case RValueAllocation::TYPED_STACK:
      {
        switch (alloc.knownType()) {
          case JSVAL_TYPE_DOUBLE:
            return DoubleValue(ReadFrameDoubleSlot(fp_, alloc.stackOffset2()));
          case JSVAL_TYPE_INT32:
            return Int32Value(ReadFrameInt32Slot(fp_, alloc.stackOffset2()));
          case JSVAL_TYPE_BOOLEAN:
            return BooleanValue(ReadFrameBooleanSlot(fp_, alloc.stackOffset2()));
          case JSVAL_TYPE_STRING:
            return FromStringPayload(fromStack(alloc.stackOffset2()));
          case JSVAL_TYPE_OBJECT:
            return FromObjectPayload(fromStack(alloc.stackOffset2()));
          default:
            MOZ_ASSUME_UNREACHABLE("Unexpected type");
        }
      }

#if defined(JS_NUNBOX32)
      case RValueAllocation::UNTYPED_REG_REG:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromRegister(alloc.reg());
        layout.s.payload.word = fromRegister(alloc.reg2());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_REG_STACK:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromRegister(alloc.reg());
        layout.s.payload.word = fromStack(alloc.stackOffset2());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_STACK_REG:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromStack(alloc.stackOffset());
        layout.s.payload.word = fromRegister(alloc.reg2());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_STACK_STACK:
      {
        jsval_layout layout;
        layout.s.tag = (JSValueTag) fromStack(alloc.stackOffset());
        layout.s.payload.word = fromStack(alloc.stackOffset2());
        return IMPL_TO_JSVAL(layout);
      }
#elif defined(JS_PUNBOX64)
      case RValueAllocation::UNTYPED_REG:
      {
        jsval_layout layout;
        layout.asBits = fromRegister(alloc.reg());
        return IMPL_TO_JSVAL(layout);
      }

      case RValueAllocation::UNTYPED_STACK:
      {
        jsval_layout layout;
        layout.asBits = fromStack(alloc.stackOffset());
        return IMPL_TO_JSVAL(layout);
      }
#endif

      default:
        MOZ_ASSUME_UNREACHABLE("huh?");
    }
}

const RResumePoint*
SnapshotIterator::resumePoint() const
{
    return instruction()->toResumePoint();
}

uint32_t
SnapshotIterator::numAllocations() const
{
    return resumePoint()->numOperands();
}

uint32_t
SnapshotIterator::pcOffset() const
{
    return resumePoint()->pcOffset();
}

void
SnapshotIterator::skipInstruction()
{
    MOZ_ASSERT(snapshot_.numAllocationsRead() == 0);
    size_t numOperands = instruction()->numOperands();
    for (size_t i = 0; i < numOperands; i++)
        skip();
    nextInstruction();
}

void
SnapshotIterator::nextFrame()
{
    nextInstruction();
    while (!instruction()->isResumePoint())
        skipInstruction();
}

IonScript*
JitFrameIterator::ionScript() const
{
    JS_ASSERT(type() == JitFrame_IonJS);

    IonScript* ionScript = nullptr;
    if (checkInvalidation(&ionScript))
        return ionScript;
    switch (GetCalleeTokenTag(calleeToken())) {
      case CalleeToken_Function:
      case CalleeToken_Script:
        return mode_ == ParallelExecution ? script()->parallelIonScript() : script()->ionScript();
      default:
        MOZ_ASSUME_UNREACHABLE("unknown callee token type");
    }
}

const SafepointIndex*
JitFrameIterator::safepoint() const
{
    if (!cachedSafepointIndex_)
        cachedSafepointIndex_ = ionScript()->getSafepointIndex(returnAddressToFp());
    return cachedSafepointIndex_;
}

const OsiIndex*
JitFrameIterator::osiIndex() const
{
    SafepointReader reader(ionScript(), safepoint());
    return ionScript()->getOsiIndex(reader.osiReturnPointOffset());
}

template <AllowGC allowGC>
void
InlineFrameIteratorMaybeGC<allowGC>::resetOn(const JitFrameIterator* iter)
{
    frame_ = iter;
    framesRead_ = 0;
    frameCount_ = UINT32_MAX;

    if (iter) {
        start_ = SnapshotIterator(*iter);
        findNextFrame();
    }
}
template void InlineFrameIteratorMaybeGC<NoGC>::resetOn(const JitFrameIterator* iter);
template void InlineFrameIteratorMaybeGC<CanGC>::resetOn(const JitFrameIterator* iter);

template <AllowGC allowGC>
void
InlineFrameIteratorMaybeGC<allowGC>::findNextFrame()
{
    JS_ASSERT(more());

    si_ = start_;

    // Read the initial frame out of the C stack.
    callee_ = frame_->maybeCallee();
    script_ = frame_->script();
    MOZ_ASSERT(script_->hasBaselineScript());

    // Settle on the outermost frame without evaluating any instructions before
    // looking for a pc.
    if (!si_.instruction()->isResumePoint())
        si_.nextFrame();

    pc_ = script_->offsetToPC(si_.pcOffset());
#ifdef DEBUG
    numActualArgs_ = 0xbadbad;
#endif

    // This unfortunately is O(n*m), because we must skip over outer frames
    // before reading inner ones.

    // The first time (frameCount_ == UINT32_MAX) we do not know the number of
    // frames that we are going to inspect.  So we are iterating until there is
    // no more frames, to settle on the inner most frame and to count the number
    // of frames.
    size_t remaining = (frameCount_ != UINT32_MAX) ? frameNo() - 1 : SIZE_MAX;

    size_t i = 1;
    for (; i <= remaining && si_.moreFrames(); i++) {
        JS_ASSERT(IsIonInlinablePC(pc_));

        // Recover the number of actual arguments from the script.
        if (JSOp(*pc_) != JSOP_FUNAPPLY)
            numActualArgs_ = GET_ARGC(pc_);
        if (JSOp(*pc_) == JSOP_FUNCALL) {
            JS_ASSERT(GET_ARGC(pc_) > 0);
            numActualArgs_ = GET_ARGC(pc_) - 1;
        } else if (IsGetPropPC(pc_)) {
            numActualArgs_ = 0;
        } else if (IsSetPropPC(pc_)) {
            numActualArgs_ = 1;
        }

        JS_ASSERT(numActualArgs_ != 0xbadbad);

        // Skip over non-argument slots, as well as |this|.
        unsigned skipCount = (si_.numAllocations() - 1) - numActualArgs_ - 1;
        for (unsigned j = 0; j < skipCount; j++)
            si_.skip();

        // The JSFunction is a constant, otherwise we would not have inlined it.
        Value funval = si_.read();

        // Skip extra value allocations.
        while (si_.moreAllocations())
            si_.skip();

        si_.nextFrame();

        callee_ = &funval.toObject().as<JSFunction>();

        // Inlined functions may be clones that still point to the lazy script
        // for the executed script, if they are clones. The actual script
        // exists though, just make sure the function points to it.
        script_ = callee_->existingScriptForInlinedFunction();
        MOZ_ASSERT(script_->hasBaselineScript());

        pc_ = script_->offsetToPC(si_.pcOffset());
    }

    // The first time we do not know the number of frames, we only settle on the
    // last frame, and update the number of frames based on the number of
    // iteration that we have done.
    if (frameCount_ == UINT32_MAX) {
        MOZ_ASSERT(!si_.moreFrames());
        frameCount_ = i;
    }

    framesRead_++;
}
template void InlineFrameIteratorMaybeGC<NoGC>::findNextFrame();
template void InlineFrameIteratorMaybeGC<CanGC>::findNextFrame();

template <AllowGC allowGC>
bool
InlineFrameIteratorMaybeGC<allowGC>::isFunctionFrame() const
{
    return !!callee_;
}
template bool InlineFrameIteratorMaybeGC<NoGC>::isFunctionFrame() const;
template bool InlineFrameIteratorMaybeGC<CanGC>::isFunctionFrame() const;

MachineState
MachineState::FromBailout(mozilla::Array<uintptr_t, Registers::Total>& regs,
                          mozilla::Array<double, FloatRegisters::Total>& fpregs)
{
    MachineState machine;

    for (unsigned i = 0; i < Registers::Total; i++)
        machine.setRegisterLocation(Register::FromCode(i), &regs[i]);
    for (unsigned i = 0; i < FloatRegisters::Total; i++)
        machine.setRegisterLocation(FloatRegister::FromCode(i), &fpregs[i]);

    return machine;
}

template <AllowGC allowGC>
bool
InlineFrameIteratorMaybeGC<allowGC>::isConstructing() const
{
    // Skip the current frame and look at the caller's.
    if (more()) {
        InlineFrameIteratorMaybeGC<allowGC> parent(GetJSContextFromJitCode(), this);
        ++parent;

        // Inlined Getters and Setters are never constructing.
        if (IsGetPropPC(parent.pc()) || IsSetPropPC(parent.pc()))
            return false;

        // In the case of a JS frame, look up the pc from the snapshot.
        JS_ASSERT(IsCallPC(parent.pc()));

        return (JSOp)*parent.pc() == JSOP_NEW;
    }

    return frame_->isConstructing();
}
template bool InlineFrameIteratorMaybeGC<NoGC>::isConstructing() const;
template bool InlineFrameIteratorMaybeGC<CanGC>::isConstructing() const;

bool
JitFrameIterator::isConstructing() const
{
    JitFrameIterator parent(*this);

    // Skip the current frame and look at the caller's.
    do {
        ++parent;
    } while (!parent.done() && !parent.isScripted());

    if (parent.isIonJS()) {
        // In the case of a JS frame, look up the pc from the snapshot.
        InlineFrameIterator inlinedParent(GetJSContextFromJitCode(), &parent);

        //Inlined Getters and Setters are never constructing.
        if (IsGetPropPC(inlinedParent.pc()) || IsSetPropPC(inlinedParent.pc()))
            return false;

        JS_ASSERT(IsCallPC(inlinedParent.pc()));

        return (JSOp)*inlinedParent.pc() == JSOP_NEW;
    }

    if (parent.isBaselineJS()) {
        jsbytecode* pc;
        parent.baselineScriptAndPc(nullptr, &pc);

        // Inlined Getters and Setters are never constructing.
        // Baseline may call getters from [GET|SET]PROP or [GET|SET]ELEM ops.
        if (IsGetPropPC(pc) || IsSetPropPC(pc) || IsGetElemPC(pc) || IsSetElemPC(pc))
            return false;

        JS_ASSERT(IsCallPC(pc));

        return JSOp(*pc) == JSOP_NEW;
    }

    JS_ASSERT(parent.done());
    return activation_->firstFrameIsConstructing();
}

unsigned
JitFrameIterator::numActualArgs() const
{
    if (isScripted())
        return jsFrame()->numActualArgs();

    JS_ASSERT(isNative());
    return exitFrame()->nativeExit()->argc();
}

void
SnapshotIterator::warnUnreadableAllocation()
{
    fprintf(stderr, "Warning! Tried to access unreadable value allocation (possible f.arguments).\n");
}

struct DumpOp {
    DumpOp(unsigned int i) : i_(i) {}

    unsigned int i_;
    void operator()(const Value& v) {
        fprintf(stderr, "  actual (arg %d): ", i_);
#ifdef DEBUG
        js_DumpValue(v);
#else
        fprintf(stderr, "?\n");
#endif
        i_++;
    }
};

void
JitFrameIterator::dumpBaseline() const
{
    JS_ASSERT(isBaselineJS());

    fprintf(stderr, " JS Baseline frame\n");
    if (isFunctionFrame()) {
        fprintf(stderr, "  callee fun: ");
#ifdef DEBUG
        js_DumpObject(callee());
#else
        fprintf(stderr, "?\n");
#endif
    } else {
        fprintf(stderr, "  global frame, no callee\n");
    }

    fprintf(stderr, "  file %s line %u\n",
            script()->filename(), (unsigned) script()->lineno());

    JSContext* cx = GetJSContextFromJitCode();
    RootedScript script(cx);
    jsbytecode* pc;
    baselineScriptAndPc(script.address(), &pc);

    fprintf(stderr, "  script = %p, pc = %p (offset %u)\n", (void*)script, pc, uint32_t(script->pcToOffset(pc)));
    fprintf(stderr, "  current op: %s\n", js_CodeName[*pc]);

    fprintf(stderr, "  actual args: %d\n", numActualArgs());

    BaselineFrame* frame = baselineFrame();

    for (unsigned i = 0; i < frame->numValueSlots(); i++) {
        fprintf(stderr, "  slot %u: ", i);
#ifdef DEBUG
        Value* v = frame->valueSlot(i);
        js_DumpValue(*v);
#else
        fprintf(stderr, "?\n");
#endif
    }
}

template <AllowGC allowGC>
void
InlineFrameIteratorMaybeGC<allowGC>::dump() const
{
    if (more())
        fprintf(stderr, " JS frame (inlined)\n");
    else
        fprintf(stderr, " JS frame\n");

    bool isFunction = false;
    if (isFunctionFrame()) {
        isFunction = true;
        fprintf(stderr, "  callee fun: ");
#ifdef DEBUG
        js_DumpObject(callee());
#else
        fprintf(stderr, "?\n");
#endif
    } else {
        fprintf(stderr, "  global frame, no callee\n");
    }

    fprintf(stderr, "  file %s line %u\n",
            script()->filename(), (unsigned) script()->lineno());

    fprintf(stderr, "  script = %p, pc = %p\n", (void*) script(), pc());
    fprintf(stderr, "  current op: %s\n", js_CodeName[*pc()]);

    if (!more()) {
        numActualArgs();
    }

    SnapshotIterator si = snapshotIterator();
    fprintf(stderr, "  slots: %u\n", si.numAllocations() - 1);
    for (unsigned i = 0; i < si.numAllocations() - 1; i++) {
        if (isFunction) {
            if (i == 0)
                fprintf(stderr, "  scope chain: ");
            else if (i == 1)
                fprintf(stderr, "  this: ");
            else if (i - 2 < callee()->nargs())
                fprintf(stderr, "  formal (arg %d): ", i - 2);
            else {
                if (i - 2 == callee()->nargs() && numActualArgs() > callee()->nargs()) {
                    DumpOp d(callee()->nargs());
                    unaliasedForEachActual(GetJSContextFromJitCode(), d, ReadFrame_Overflown);
                }

                fprintf(stderr, "  slot %d: ", int(i - 2 - callee()->nargs()));
            }
        } else
            fprintf(stderr, "  slot %u: ", i);
#ifdef DEBUG
        js_DumpValue(si.maybeRead());
#else
        fprintf(stderr, "?\n");
#endif
    }

    fputc('\n', stderr);
}
template void InlineFrameIteratorMaybeGC<NoGC>::dump() const;
template void InlineFrameIteratorMaybeGC<CanGC>::dump() const;

void
JitFrameIterator::dump() const
{
    switch (type_) {
      case JitFrame_Entry:
        fprintf(stderr, " Entry frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_BaselineJS:
        dumpBaseline();
        break;
      case JitFrame_BaselineStub:
      case JitFrame_Unwound_BaselineStub:
        fprintf(stderr, " Baseline stub frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_IonJS:
      {
        InlineFrameIterator frames(GetJSContextFromJitCode(), this);
        for (;;) {
            frames.dump();
            if (!frames.more())
                break;
            ++frames;
        }
        break;
      }
      case JitFrame_Rectifier:
      case JitFrame_Unwound_Rectifier:
        fprintf(stderr, " Rectifier frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case JitFrame_Unwound_IonJS:
        fprintf(stderr, "Warning! Unwound JS frames are not observable.\n");
        break;
      case JitFrame_Exit:
        break;
    };
    fputc('\n', stderr);
}

IonJSFrameLayout*
InvalidationBailoutStack::fp() const
{
    return (IonJSFrameLayout*) (sp() + ionScript_->frameSize());
}

void
InvalidationBailoutStack::checkInvariants() const
{
#ifdef DEBUG
    IonJSFrameLayout* frame = fp();
    CalleeToken token = frame->calleeToken();
    JS_ASSERT(token);

    uint8_t* rawBase = ionScript()->method()->raw();
    uint8_t* rawLimit = rawBase + ionScript()->method()->instructionsSize();
    uint8_t* osiPoint = osiPointReturnAddress();
    JS_ASSERT(rawBase <= osiPoint && osiPoint <= rawLimit);
#endif
}

} // namespace jit
} // namespace js
