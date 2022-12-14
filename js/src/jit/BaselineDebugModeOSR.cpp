/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineDebugModeOSR.h"

#include "mozilla/DebugOnly.h"

#include "jit/IonLinker.h"
#include "jit/PerfSpewer.h"

#include "jit/IonFrames-inl.h"
#include "vm/Stack-inl.h"

using namespace mozilla;
using namespace js;
using namespace js::jit;

struct DebugModeOSREntry
{
    JSScript* script;
    BaselineScript* oldBaselineScript;
    BaselineDebugModeOSRInfo* recompInfo;
    uint32_t pcOffset;
    ICEntry::Kind frameKind;

    // Used for sanity asserts in debug builds.
    DebugOnly<ICStub*> stub;

    DebugModeOSREntry(JSScript* script)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        recompInfo(nullptr),
        pcOffset(uint32_t(-1)),
        frameKind(ICEntry::Kind_NonOp),
        stub(nullptr)
    { }

    DebugModeOSREntry(JSScript* script, const ICEntry& icEntry)
      : script(script),
        oldBaselineScript(script->baselineScript()),
        recompInfo(nullptr),
        pcOffset(icEntry.pcOffset()),
        frameKind(icEntry.kind()),
        stub(nullptr)
    {
#ifdef DEBUG
        MOZ_ASSERT(pcOffset == icEntry.pcOffset());
        MOZ_ASSERT(frameKind == icEntry.kind());

        // Assert that if we have a NonOp ICEntry, that there are no unsynced
        // slots, since such a recompile could have only been triggered from
        // either an interrupt check or a debug trap handler.
        //
        // If triggered from an interrupt check, the stack should be fully
        // synced.
        //
        // If triggered from a debug trap handler, we must be recompiling for
        // toggling debug mode on->off, in which case the old baseline script
        // should have fully synced stack at every bytecode.
        if (frameKind == ICEntry::Kind_NonOp) {
            PCMappingSlotInfo slotInfo;
            jsbytecode* pc = script->offsetToPC(pcOffset);
            oldBaselineScript->nativeCodeForPC(script, pc, &slotInfo);
            MOZ_ASSERT(slotInfo.numUnsynced() == 0);
        }
#endif
    }

    DebugModeOSREntry(DebugModeOSREntry&& other)
      : script(other.script),
        oldBaselineScript(other.oldBaselineScript),
        recompInfo(other.recompInfo ? other.takeRecompInfo() : nullptr),
        pcOffset(other.pcOffset),
        frameKind(other.frameKind),
        stub(other.stub)
    { }

    ~DebugModeOSREntry() {
        // Note that this is nulled out when the recompInfo is taken by the
        // frame. The frame then has the responsibility of freeing the
        // recompInfo.
        js_delete(recompInfo);
    }

    bool needsRecompileInfo() const {
        return (frameKind == ICEntry::Kind_CallVM ||
                frameKind == ICEntry::Kind_DebugTrap ||
                frameKind == ICEntry::Kind_DebugPrologue ||
                frameKind == ICEntry::Kind_DebugEpilogue);
    }

    BaselineDebugModeOSRInfo* takeRecompInfo() {
        MOZ_ASSERT(recompInfo);
        BaselineDebugModeOSRInfo* tmp = recompInfo;
        recompInfo = nullptr;
        return tmp;
    }

    bool allocateRecompileInfo(JSContext* cx) {
        MOZ_ASSERT(needsRecompileInfo());

        // If we are returning to a frame which needs a continuation fixer,
        // allocate the recompile info up front so that the patching function
        // is infallible.
        jsbytecode* pc = script->offsetToPC(pcOffset);

        // XXX: Work around compiler error disallowing using bitfields
        // with the template magic of new_.
        ICEntry::Kind kind = frameKind;
        recompInfo = cx->new_<BaselineDebugModeOSRInfo>(pc, kind);
        return !!recompInfo;
    }
};

typedef js::Vector<DebugModeOSREntry> DebugModeOSREntryVector;

static bool
CollectOnStackScripts(JSContext* cx, const JitActivationIterator& activation,
                      DebugModeOSREntryVector& entries)
{
    DebugOnly<ICStub*> prevFrameStubPtr = nullptr;
    bool needsRecompileHandler = false;
    for (JitFrameIterator iter(activation); !iter.done(); ++iter) {
        switch (iter.type()) {
          case JitFrame_BaselineJS: {
            JSScript* script = iter.script();
            uint8_t* retAddr = iter.returnAddressToFp();
            ICEntry& entry = script->baselineScript()->icEntryFromReturnAddress(retAddr);

            if (!entries.append(DebugModeOSREntry(script, entry)))
                return false;

            if (entries.back().needsRecompileInfo()) {
                if (!entries.back().allocateRecompileInfo(cx))
                    return false;

                needsRecompileHandler |= true;
            }

            entries.back().stub = prevFrameStubPtr;
            prevFrameStubPtr = nullptr;
            break;
          }

          case JitFrame_BaselineStub:
            prevFrameStubPtr =
                reinterpret_cast<IonBaselineStubFrameLayout*>(iter.fp())->maybeStubPtr();
            break;

          case JitFrame_IonJS: {
            JSScript* script = iter.script();
            if (!entries.append(DebugModeOSREntry(script)))
                return false;
            for (InlineFrameIterator inlineIter(cx, &iter); inlineIter.more(); ++inlineIter) {
                if (!entries.append(DebugModeOSREntry(inlineIter.script())))
                    return false;
            }
            break;
          }

          default:;
        }
    }

    // Initialize the on-stack recompile handler, which may fail, so that
    // patching the stack is infallible.
    if (needsRecompileHandler) {
        JitRuntime* rt = cx->runtime()->jitRuntime();
        if (!rt->getBaselineDebugModeOSRHandlerAddress(cx, true))
            return false;
    }

    return true;
}

static inline uint8_t*
GetStubReturnFromStubAddress(JSContext* cx, jsbytecode* pc)
{
    JitCompartment* comp = cx->compartment()->jitCompartment();
    void* addr;
    if (IsGetPropPC(pc)) {
        addr = comp->baselineGetPropReturnFromStubAddr();
    } else if (IsSetPropPC(pc)) {
        addr = comp->baselineSetPropReturnFromStubAddr();
    } else {
        JS_ASSERT(IsCallPC(pc));
        addr = comp->baselineCallReturnFromStubAddr();
    }
    return reinterpret_cast<uint8_t*>(addr);
}

static const char*
ICEntryKindToString(ICEntry::Kind kind)
{
    switch (kind) {
      case ICEntry::Kind_Op:
        return "IC";
      case ICEntry::Kind_NonOp:
        return "non-op IC";
      case ICEntry::Kind_CallVM:
        return "callVM";
      case ICEntry::Kind_DebugTrap:
        return "debug trap";
      case ICEntry::Kind_DebugPrologue:
        return "debug prologue";
      case ICEntry::Kind_DebugEpilogue:
        return "debug epilogue";
      default:
        MOZ_ASSUME_UNREACHABLE("bad ICEntry kind");
    }
}

static void
SpewPatchBaselineFrame(uint8_t* oldReturnAddress, uint8_t* newReturnAddress,
                       JSScript* script, ICEntry::Kind frameKind, jsbytecode* pc)
{
    IonSpew(IonSpew_BaselineDebugModeOSR,
            "Patch return %#016llx -> %#016llx to BaselineJS (%s:%d) from %s at %s",
            uintptr_t(oldReturnAddress), uintptr_t(newReturnAddress),
            script->filename(), script->lineno(),
            ICEntryKindToString(frameKind), js_CodeName[(JSOp)*pc]);
}

static void
SpewPatchStubFrame(uint8_t* oldReturnAddress, uint8_t* newReturnAddress,
                   ICStub* oldStub, ICStub* newStub)
{
    IonSpew(IonSpew_BaselineDebugModeOSR,
            "Patch return %#016llx -> %#016llx",
            uintptr_t(oldReturnAddress), uintptr_t(newReturnAddress));
    IonSpew(IonSpew_BaselineDebugModeOSR,
            "Patch   stub %#016llx -> %#016llx to BaselineStub",
            uintptr_t(oldStub), uintptr_t(newStub));
}

static void
PatchBaselineFramesForDebugMode(JSContext* cx, const JitActivationIterator& activation,
                                DebugModeOSREntryVector& entries, size_t* start)
{
    //
    // Recompile Patching Overview
    //
    // When toggling debug mode with live baseline scripts on the stack, we
    // could have entered the VM via the following ways from the baseline
    // script.
    //
    // Off to On:
    //  A. From a "can call" stub.
    //  B. From a VM call (interrupt handler, debugger statement handler).
    //
    // On to Off:
    //  - All the ways above.
    //  C. From the debug trap handler.
    //  D. From the debug prologue.
    //  E. From the debug epilogue.
    //
    // In general, we patch the return address from the VM call to return to a
    // "continuation fixer" to fix up machine state (registers and stack
    // state). Specifics on what need to be done are documented below.
    //

    IonCommonFrameLayout* prev = nullptr;
    size_t entryIndex = *start;
    DebugOnly<bool> expectedDebugMode = cx->compartment()->debugMode();

    for (JitFrameIterator iter(activation); !iter.done(); ++iter) {
        switch (iter.type()) {
          case JitFrame_BaselineJS: {
            JSScript* script = entries[entryIndex].script;
            uint32_t pcOffset = entries[entryIndex].pcOffset;
            jsbytecode* pc = script->offsetToPC(pcOffset);

            MOZ_ASSERT(script == iter.script());
            MOZ_ASSERT(pcOffset < script->length());
            MOZ_ASSERT(script->baselineScript()->debugMode() == expectedDebugMode);

            BaselineScript* bl = script->baselineScript();
            ICEntry::Kind kind = entries[entryIndex].frameKind;

            if (kind == ICEntry::Kind_Op) {
                // Case A above.
                //
                // Patching this case needs to patch both the stub frame and
                // the baseline frame. The stub frame is patched below. For
                // the baseline frame here, we resume right after the IC
                // returns.
                //
                // Since we're using the IC-specific k-fixer, we can resume
                // directly to the IC resume address.
                uint8_t* retAddr = bl->returnAddressForIC(bl->icEntryFromPCOffset(pcOffset));
                SpewPatchBaselineFrame(prev->returnAddress(), retAddr, script, kind, pc);
                prev->setReturnAddress(retAddr);
                entryIndex++;
                break;
            }

            bool popFrameReg;

            // The RecompileInfo must already be allocated so that this
            // function may be infallible.
            BaselineDebugModeOSRInfo* recompInfo = entries[entryIndex].takeRecompInfo();

            switch (kind) {
              case ICEntry::Kind_CallVM:
                // Case B above.
                //
                // Patching returns from an interrupt handler or the debugger
                // statement handler is similar in that we can resume at the
                // next op.
                pc += GetBytecodeLength(pc);
                recompInfo->resumeAddr = bl->nativeCodeForPC(script, pc, &recompInfo->slotInfo);
                popFrameReg = true;
                break;

              case ICEntry::Kind_DebugTrap:
                // Case C above.
                //
                // Debug traps are emitted before each op, so we resume at the
                // same op. Calling debug trap handlers is done via a toggled
                // call to a thunk (DebugTrapHandler) that takes care tearing
                // down its own stub frame so we don't need to worry about
                // popping the frame reg.
                recompInfo->resumeAddr = bl->nativeCodeForPC(script, pc, &recompInfo->slotInfo);
                popFrameReg = false;
                break;

              case ICEntry::Kind_DebugPrologue:
                // Case D above.
                //
                // We patch a jump directly to the right place in the prologue
                // after popping the frame reg and checking for forced return.
                recompInfo->resumeAddr = bl->postDebugPrologueAddr();
                popFrameReg = true;
                break;

              default:
                // Case E above.
                //
                // We patch a jump directly to the epilogue after popping the
                // frame reg and checking for forced return.
                MOZ_ASSERT(kind == ICEntry::Kind_DebugEpilogue);
                recompInfo->resumeAddr = bl->epilogueEntryAddr();
                popFrameReg = true;
                break;
            }

            SpewPatchBaselineFrame(prev->returnAddress(), recompInfo->resumeAddr,
                                   script, kind, recompInfo->pc);

            // The recompile handler must already be created so that this
            // function may be infallible.
            JitRuntime* rt = cx->runtime()->jitRuntime();
            void* handlerAddr = rt->getBaselineDebugModeOSRHandlerAddress(cx, popFrameReg);
            MOZ_ASSERT(handlerAddr);

            prev->setReturnAddress(reinterpret_cast<uint8_t*>(handlerAddr));
            iter.baselineFrame()->setDebugModeOSRInfo(recompInfo);

            entryIndex++;
            break;
          }

          case JitFrame_BaselineStub: {
            JSScript* script = entries[entryIndex].script;
            IonBaselineStubFrameLayout* layout =
                reinterpret_cast<IonBaselineStubFrameLayout*>(iter.fp());
            MOZ_ASSERT(script->baselineScript()->debugMode() == expectedDebugMode);
            MOZ_ASSERT(layout->maybeStubPtr() == entries[entryIndex].stub);

            // Patch baseline stub frames for case A above.
            //
            // We need to patch the stub frame return address to go to the
            // k-fixer that is at the end of fallback stubs of all such
            // can-call ICs. These k-fixers share code with bailout-from-Ion
            // fixers, but in this case we are returning from VM and not
            // Ion. See e.g., JitCompartment::baselineCallReturnFromStubAddr()
            //
            // Subtlety here: the debug trap handler of case C above pushes a
            // stub frame with a null stub pointer. This handler will exist
            // across recompiling the script, so we don't patch anything for
            // such stub frames. We will return to that handler, which takes
            // care of cleaning up the stub frame.
            //
            // Note that for stub pointers that are already on the C stack
            // (i.e. fallback calls), we need to check for recompilation using
            // DebugModeOSRVolatileStub.
            if (layout->maybeStubPtr()) {
                MOZ_ASSERT(layout->maybeStubPtr() == entries[entryIndex].stub);
                uint32_t pcOffset = entries[entryIndex].pcOffset;
                uint8_t* retAddr = GetStubReturnFromStubAddress(cx, script->offsetToPC(pcOffset));

                // Get the fallback stub for the IC in the recompiled
                // script. The fallback stub is guaranteed to exist.
                ICEntry& entry = script->baselineScript()->icEntryFromPCOffset(pcOffset);
                ICStub* newStub = entry.fallbackStub();
                SpewPatchStubFrame(prev->returnAddress(), retAddr, layout->maybeStubPtr(), newStub);
                prev->setReturnAddress(retAddr);
                layout->setStubPtr(newStub);
            }

            break;
          }

          case JitFrame_IonJS:
            // Nothing to patch.
            entryIndex++;
            for (InlineFrameIterator inlineIter(cx, &iter); inlineIter.more(); ++inlineIter)
                entryIndex++;
            break;

          default:;
        }

        prev = iter.current();
    }

    *start = entryIndex;
}

static bool
RecompileBaselineScriptForDebugMode(JSContext* cx, JSScript* script)
{
    BaselineScript* oldBaselineScript = script->baselineScript();

    // If a script is on the stack multiple times, it may have already
    // been recompiled.
    bool expectedDebugMode = cx->compartment()->debugMode();
    if (oldBaselineScript->debugMode() == expectedDebugMode)
        return true;

    IonSpew(IonSpew_BaselineDebugModeOSR, "Recompiling (%s:%d) for debug mode %s",
            script->filename(), script->lineno(), expectedDebugMode ? "ON" : "OFF");

    if (script->hasIonScript())
        Invalidate(cx, script, /* resetUses = */ false);

    script->setBaselineScript(cx, nullptr);

    MethodStatus status = BaselineCompile(cx, script);
    if (status != Method_Compiled) {
        // We will only fail to recompile for debug mode due to OOM. Restore
        // the old baseline script in case something doesn't properly
        // propagate OOM.
        MOZ_ASSERT(status == Method_Error);
        script->setBaselineScript(cx, oldBaselineScript);
        return false;
    }

    // Don't destroy the old baseline script yet, since if we fail any of the
    // recompiles we need to rollback all the old baseline scripts.
    MOZ_ASSERT(script->baselineScript()->debugMode() == expectedDebugMode);
    return true;
}

static void
UndoRecompileBaselineScriptsForDebugMode(JSContext* cx,
                                         const DebugModeOSREntryVector& entries)
{
    // In case of failure, roll back the entire set of active scripts so that
    // we don't have to patch return addresses on the stack.
    for (size_t i = 0; i < entries.length(); i++) {
        JSScript* script = entries[i].script;
        BaselineScript* baselineScript = script->baselineScript();
        if (baselineScript != entries[i].oldBaselineScript) {
            script->setBaselineScript(cx, entries[i].oldBaselineScript);
            BaselineScript::Destroy(cx->runtime()->defaultFreeOp(), baselineScript);
        }
    }
}

bool
jit::RecompileOnStackBaselineScriptsForDebugMode(JSContext* cx, JSCompartment* comp)
{
    AutoCompartment ac(cx, comp);

    // First recompile the active scripts on the stack and patch the live
    // frames.
    Vector<DebugModeOSREntry> entries(cx);

    for (JitActivationIterator iter(cx->runtime()); !iter.done(); ++iter) {
        if (iter.activation()->compartment() == comp) {
            if (!CollectOnStackScripts(cx, iter, entries))
                return false;
        }
    }

#ifdef JSGC_GENERATIONAL
    // Scripts can entrain nursery things. See note in js::ReleaseAllJITCode.
    if (!entries.empty())
        MinorGC(cx->runtime(), JS::gcreason::EVICT_NURSERY);
#endif

    // Try to recompile all the scripts. If we encounter an error, we need to
    // roll back as if none of the compilations happened, so that we don't
    // crash.
    for (size_t i = 0; i < entries.length(); i++) {
        JSScript* script = entries[i].script;
        if (!RecompileBaselineScriptForDebugMode(cx, script)) {
            UndoRecompileBaselineScriptsForDebugMode(cx, entries);
            return false;
        }
    }

    // If all recompiles succeeded, destroy the old baseline scripts and patch
    // the live frames.
    //
    // After this point the function must be infallible.

    for (size_t i = 0; i < entries.length(); i++)
        BaselineScript::Destroy(cx->runtime()->defaultFreeOp(), entries[i].oldBaselineScript);

    size_t processed = 0;
    for (JitActivationIterator iter(cx->runtime()); !iter.done(); ++iter) {
        if (iter.activation()->compartment() == comp)
            PatchBaselineFramesForDebugMode(cx, iter, entries, &processed);
    }
    MOZ_ASSERT(processed == entries.length());

    return true;
}

void
BaselineDebugModeOSRInfo::popValueInto(PCMappingSlotInfo::SlotLocation loc, Value* vp)
{
    switch (loc) {
      case PCMappingSlotInfo::SlotInR0:
        valueR0 = vp[stackAdjust];
        break;
      case PCMappingSlotInfo::SlotInR1:
        valueR1 = vp[stackAdjust];
        break;
      case PCMappingSlotInfo::SlotIgnore:
        break;
      default:
        MOZ_ASSUME_UNREACHABLE("Bad slot location");
    }

    stackAdjust++;
}

static inline bool
HasForcedReturn(BaselineDebugModeOSRInfo* info, bool rv)
{
    ICEntry::Kind kind = info->frameKind;

    // The debug epilogue always checks its resumption value, so we don't need
    // to check rv.
    if (kind == ICEntry::Kind_DebugEpilogue)
        return true;

    // |rv| is the value in ReturnReg. If true, in the case of the prologue,
    // debug trap, and debugger statement handler, it means a forced return.
    if (kind == ICEntry::Kind_DebugPrologue ||
        (kind == ICEntry::Kind_CallVM && JSOp(*info->pc) == JSOP_DEBUGGER))
    {
        return rv;
    }

    // N.B. The debug trap handler handles its own forced return, so no
    // need to deal with it here.
    return false;
}

static void
SyncBaselineDebugModeOSRInfo(BaselineFrame* frame, Value* vp, bool rv)
{
    BaselineDebugModeOSRInfo* info = frame->debugModeOSRInfo();
    MOZ_ASSERT(info);
    MOZ_ASSERT(frame->script()->baselineScript()->containsCodeAddress(info->resumeAddr));

    if (HasForcedReturn(info, rv)) {
        // Load the frame's rval and overwrite the resume address to go to the
        // epilogue.
        MOZ_ASSERT(R0 == JSReturnOperand);
        info->valueR0 = frame->returnValue();
        info->resumeAddr = frame->script()->baselineScript()->epilogueEntryAddr();
        return;
    }

    // Read stack values and make sure R0 and R1 have the right values.
    unsigned numUnsynced = info->slotInfo.numUnsynced();
    MOZ_ASSERT(numUnsynced <= 2);
    if (numUnsynced > 0)
        info->popValueInto(info->slotInfo.topSlotLocation(), vp);
    if (numUnsynced > 1)
        info->popValueInto(info->slotInfo.nextSlotLocation(), vp);

    // Scale stackAdjust.
    info->stackAdjust *= sizeof(Value);
}

static void
FinishBaselineDebugModeOSR(BaselineFrame* frame)
{
    frame->deleteDebugModeOSRInfo();
}

void
BaselineFrame::deleteDebugModeOSRInfo()
{
    js_delete(getDebugModeOSRInfo());
    flags_ &= ~HAS_DEBUG_MODE_OSR_INFO;
}

JitCode*
JitRuntime::getBaselineDebugModeOSRHandler(JSContext* cx)
{
    if (!baselineDebugModeOSRHandler_) {
        AutoLockForExclusiveAccess lock(cx);
        AutoCompartment ac(cx, cx->runtime()->atomsCompartment());
        uint32_t offset;
        if (JitCode* code = generateBaselineDebugModeOSRHandler(cx, &offset)) {
            baselineDebugModeOSRHandler_ = code;
            baselineDebugModeOSRHandlerNoFrameRegPopAddr_ = code->raw() + offset;
        }
    }

    return baselineDebugModeOSRHandler_;
}

void*
JitRuntime::getBaselineDebugModeOSRHandlerAddress(JSContext* cx, bool popFrameReg)
{
    if (!getBaselineDebugModeOSRHandler(cx))
        return nullptr;
    return (popFrameReg
            ? baselineDebugModeOSRHandler_->raw()
            : baselineDebugModeOSRHandlerNoFrameRegPopAddr_);
}

JitCode*
JitRuntime::generateBaselineDebugModeOSRHandler(JSContext* cx, uint32_t* noFrameRegPopOffsetOut)
{
    MacroAssembler masm(cx);

    GeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(BaselineFrameReg);
    regs.take(ReturnReg);
    Register temp = regs.takeAny();
    Register syncedStackStart = regs.takeAny();

    // Pop the frame reg.
    masm.pop(BaselineFrameReg);

    // Not all patched baseline frames are returning from a situation where
    // the frame reg is already fixed up.
    CodeOffsetLabel noFrameRegPopOffset = masm.currentOffset();

    // Record the stack pointer for syncing.
    masm.movePtr(StackPointer, syncedStackStart);
    masm.push(BaselineFrameReg);

    // Call a stub to fully initialize the info.
    masm.setupUnalignedABICall(3, temp);
    masm.loadBaselineFramePtr(BaselineFrameReg, temp);
    masm.passABIArg(temp);
    masm.passABIArg(syncedStackStart);
    masm.passABIArg(ReturnReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, SyncBaselineDebugModeOSRInfo));

    // Discard stack values depending on how many were unsynced, as we always
    // have a fully synced stack in the recompile handler. See assert in
    // DebugModeOSREntry constructor.
    masm.pop(BaselineFrameReg);
    masm.loadPtr(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfScratchValue()), temp);
    masm.addPtr(Address(temp, offsetof(BaselineDebugModeOSRInfo, stackAdjust)), StackPointer);

    // Save real return address on the stack temporarily.
    masm.pushValue(Address(temp, offsetof(BaselineDebugModeOSRInfo, valueR0)));
    masm.pushValue(Address(temp, offsetof(BaselineDebugModeOSRInfo, valueR1)));
    masm.push(BaselineFrameReg);
    masm.push(Address(temp, offsetof(BaselineDebugModeOSRInfo, resumeAddr)));

    // Call a stub to free the allocated info.
    masm.setupUnalignedABICall(1, temp);
    masm.loadBaselineFramePtr(BaselineFrameReg, temp);
    masm.passABIArg(temp);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, FinishBaselineDebugModeOSR));

    // Restore saved values.
    GeneralRegisterSet jumpRegs(GeneralRegisterSet::All());
    jumpRegs.take(R0);
    jumpRegs.take(R1);
    jumpRegs.take(BaselineFrameReg);
    Register target = jumpRegs.takeAny();

    masm.pop(target);
    masm.pop(BaselineFrameReg);
    masm.popValue(R1);
    masm.popValue(R0);

    masm.jump(target);

    Linker linker(masm);
    AutoFlushICache afc("BaselineDebugModeOSRHandler");
    JitCode* code = linker.newCode<NoGC>(cx, JSC::OTHER_CODE);
    if (!code)
        return nullptr;

    noFrameRegPopOffset.fixup(&masm);
    *noFrameRegPopOffsetOut = noFrameRegPopOffset.offset();

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "BaselineDebugModeOSRHandler");
#endif

    return code;
}
