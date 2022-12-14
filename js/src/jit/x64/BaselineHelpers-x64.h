/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_BaselineHelpers_x64_h
#define jit_x64_BaselineHelpers_x64_h

#ifdef JS_ION
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineRegisters.h"
#include "jit/IonMacroAssembler.h"

namespace js {
namespace jit {

// Distance from Stack top to the top Value inside an IC stub (this is the return address).
static const size_t ICStackValueOffset = sizeof(void*);

inline void
EmitRestoreTailCallReg(MacroAssembler& masm)
{
    masm.pop(BaselineTailCallReg);
}

inline void
EmitRepushTailCallReg(MacroAssembler& masm)
{
    masm.push(BaselineTailCallReg);
}

inline void
EmitCallIC(CodeOffsetLabel* patchOffset, MacroAssembler& masm)
{
    // Move ICEntry offset into BaselineStubReg
    CodeOffsetLabel offset = masm.movWithPatch(ImmWord(-1), BaselineStubReg);
    *patchOffset = offset;

    // Load stub pointer into BaselineStubReg
    masm.loadPtr(Address(BaselineStubReg, (int32_t) ICEntry::offsetOfFirstStub()),
                 BaselineStubReg);

    // Call the stubcode.
    masm.call(Operand(BaselineStubReg, ICStub::offsetOfStubCode()));
}

inline void
EmitEnterTypeMonitorIC(MacroAssembler& masm,
                       size_t monitorStubOffset = ICMonitoredStub::offsetOfFirstMonitorStub())
{
    // This is expected to be called from within an IC, when BaselineStubReg
    // is properly initialized to point to the stub.
    masm.loadPtr(Address(BaselineStubReg, (int32_t) monitorStubOffset), BaselineStubReg);

    // Jump to the stubcode.
    masm.jmp(Operand(BaselineStubReg, (int32_t) ICStub::offsetOfStubCode()));
}

inline void
EmitReturnFromIC(MacroAssembler& masm)
{
    masm.ret();
}

inline void
EmitChangeICReturnAddress(MacroAssembler& masm, Register reg)
{
    masm.storePtr(reg, Address(StackPointer, 0));
}

inline void
EmitTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t argSize)
{
    // We an assume during this that R0 and R1 have been pushed.
    masm.movq(BaselineFrameReg, ScratchReg);
    masm.addq(Imm32(BaselineFrame::FramePointerOffset), ScratchReg);
    masm.subq(BaselineStackReg, ScratchReg);

    // Store frame size without VMFunction arguments for GC marking.
    masm.movq(ScratchReg, rdx);
    masm.subq(Imm32(argSize), rdx);
    masm.store32(rdx, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Push frame descriptor and perform the tail call.
    masm.makeFrameDescriptor(ScratchReg, JitFrame_BaselineJS);
    masm.push(ScratchReg);
    masm.push(BaselineTailCallReg);
    masm.jmp(target);
}

inline void
EmitCreateStubFrameDescriptor(MacroAssembler& masm, Register reg)
{
    // Compute stub frame size. We have to add two pointers: the stub reg and previous
    // frame pointer pushed by EmitEnterStubFrame.
    masm.movq(BaselineFrameReg, reg);
    masm.addq(Imm32(sizeof(void*) * 2), reg);
    masm.subq(BaselineStackReg, reg);

    masm.makeFrameDescriptor(reg, JitFrame_BaselineStub);
}

inline void
EmitCallVM(JitCode* target, MacroAssembler& masm)
{
    EmitCreateStubFrameDescriptor(masm, ScratchReg);
    masm.push(ScratchReg);
    masm.call(target);
}

// Size of vales pushed by EmitEnterStubFrame.
static const uint32_t STUB_FRAME_SIZE = 4 * sizeof(void*);
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = sizeof(void*);

inline void
EmitEnterStubFrame(MacroAssembler& masm, Register)
{
    EmitRestoreTailCallReg(masm);

    // Compute frame size.
    masm.movq(BaselineFrameReg, ScratchReg);
    masm.addq(Imm32(BaselineFrame::FramePointerOffset), ScratchReg);
    masm.subq(BaselineStackReg, ScratchReg);

    masm.store32(ScratchReg, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Note: when making changes here,  don't forget to update STUB_FRAME_SIZE
    // if needed.

    // Push frame descriptor and return address.
    masm.makeFrameDescriptor(ScratchReg, JitFrame_BaselineJS);
    masm.push(ScratchReg);
    masm.push(BaselineTailCallReg);

    // Save old frame pointer, stack pointer and stub reg.
    masm.push(BaselineStubReg);
    masm.push(BaselineFrameReg);
    masm.mov(BaselineStackReg, BaselineFrameReg);
}

inline void
EmitLeaveStubFrameHead(MacroAssembler& masm, bool calledIntoIon = false)
{
    // Ion frames do not save and restore the frame pointer. If we called
    // into Ion, we have to restore the stack pointer from the frame descriptor.
    // If we performed a VM call, the descriptor has been popped already so
    // in that case we use the frame pointer.
    if (calledIntoIon) {
        masm.pop(ScratchReg);
        masm.shrq(Imm32(FRAMESIZE_SHIFT), ScratchReg);
        masm.addq(ScratchReg, BaselineStackReg);
    } else {
        masm.mov(BaselineFrameReg, BaselineStackReg);
    }
}

inline void
EmitLeaveStubFrameCommonTail(MacroAssembler& masm)
{
    masm.pop(BaselineFrameReg);
    masm.pop(BaselineStubReg);

    // Pop return address.
    masm.pop(BaselineTailCallReg);

    // Overwrite frame descriptor with return address, so that the stack matches
    // the state before entering the stub frame.
    masm.storePtr(BaselineTailCallReg, Address(BaselineStackReg, 0));
}

inline void
EmitLeaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false)
{
    EmitLeaveStubFrameHead(masm, calledIntoIon);
    EmitLeaveStubFrameCommonTail(masm);
}

inline void
EmitStowICValues(MacroAssembler& masm, int values)
{
    JS_ASSERT(values >= 0 && values <= 2);
    switch(values) {
      case 1:
        // Stow R0
        masm.pop(BaselineTailCallReg);
        masm.pushValue(R0);
        masm.push(BaselineTailCallReg);
        break;
      case 2:
        // Stow R0 and R1
        masm.pop(BaselineTailCallReg);
        masm.pushValue(R0);
        masm.pushValue(R1);
        masm.push(BaselineTailCallReg);
        break;
    }
}

inline void
EmitUnstowICValues(MacroAssembler& masm, int values, bool discard = false)
{
    JS_ASSERT(values >= 0 && values <= 2);
    switch(values) {
      case 1:
        // Unstow R0
        masm.pop(BaselineTailCallReg);
        if (discard)
            masm.addPtr(Imm32(sizeof(Value)), BaselineStackReg);
        else
            masm.popValue(R0);
        masm.push(BaselineTailCallReg);
        break;
      case 2:
        // Unstow R0 and R1
        masm.pop(BaselineTailCallReg);
        if (discard) {
            masm.addPtr(Imm32(sizeof(Value) * 2), BaselineStackReg);
        } else {
            masm.popValue(R1);
            masm.popValue(R0);
        }
        masm.push(BaselineTailCallReg);
        break;
    }
}

inline void
EmitCallTypeUpdateIC(MacroAssembler& masm, JitCode* code, uint32_t objectOffset)
{
    // R0 contains the value that needs to be typechecked.
    // The object we're updating is a boxed Value on the stack, at offset
    // objectOffset from stack top, excluding the return address.

    // Save the current BaselineStubReg to stack
    masm.push(BaselineStubReg);

    // This is expected to be called from within an IC, when BaselineStubReg
    // is properly initialized to point to the stub.
    masm.loadPtr(Address(BaselineStubReg, (int32_t) ICUpdatedStub::offsetOfFirstUpdateStub()),
                 BaselineStubReg);

    // Call the stubcode.
    masm.call(Operand(BaselineStubReg, ICStub::offsetOfStubCode()));

    // Restore the old stub reg.
    masm.pop(BaselineStubReg);

    // The update IC will store 0 or 1 in R1.scratchReg() reflecting if the
    // value in R0 type-checked properly or not.
    Label success;
    masm.cmp32(R1.scratchReg(), Imm32(1));
    masm.j(Assembler::Equal, &success);

    // If the IC failed, then call the update fallback function.
    EmitEnterStubFrame(masm, R1.scratchReg());

    masm.loadValue(Address(BaselineStackReg, STUB_FRAME_SIZE + objectOffset), R1);

    masm.pushValue(R0);
    masm.pushValue(R1);
    masm.push(BaselineStubReg);

    // Load previous frame pointer, push BaselineFrame*.
    masm.loadPtr(Address(BaselineFrameReg, 0), R0.scratchReg());
    masm.pushBaselineFramePtr(R0.scratchReg(), R0.scratchReg());

    EmitCallVM(code, masm);
    EmitLeaveStubFrame(masm);

    // Success at end.
    masm.bind(&success);
}

template <typename AddrType>
inline void
EmitPreBarrier(MacroAssembler& masm, const AddrType& addr, MIRType type)
{
    masm.patchableCallPreBarrier(addr, type);
}

inline void
EmitStubGuardFailure(MacroAssembler& masm)
{
    // NOTE: This routine assumes that the stub guard code left the stack in the
    // same state it was in when it was entered.

    // BaselineStubEntry points to the current stub.

    // Load next stub into BaselineStubReg
    masm.loadPtr(Address(BaselineStubReg, ICStub::offsetOfNext()), BaselineStubReg);

    // Return address is already loaded, just jump to the next stubcode.
    masm.jmp(Operand(BaselineStubReg, ICStub::offsetOfStubCode()));
}


} // namespace jit
} // namespace js

#endif // JS_ION

#endif /* jit_x64_BaselineHelpers_x64_h */
