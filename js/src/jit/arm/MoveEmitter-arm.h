/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_MoveEmitter_arm_h
#define jit_arm_MoveEmitter_arm_h

#include "jit/IonMacroAssembler.h"
#include "jit/MoveResolver.h"

namespace js {
namespace jit {

class CodeGenerator;

class MoveEmitterARM
{
    bool inCycle_;
    MacroAssemblerARMCompat& masm;

    // Original stack push value.
    uint32_t pushedAtStart_;

    // These store stack offsets to spill locations, snapshotting
    // codegen->framePushed_ at the time they were allocated. They are -1 if no
    // stack space has been allocated for that particular spill.
    int32_t pushedAtCycle_;
    int32_t pushedAtSpill_;

    // These are registers that are available for temporary use. They may be
    // assigned InvalidReg. If no corresponding spill space has been assigned,
    // then these registers do not need to be spilled.
    Register spilledReg_;
    FloatRegister spilledFloatReg_;

    void assertDone();
    Register tempReg();
    FloatRegister tempFloatReg();
    Operand cycleSlot() const;
    Operand spillSlot() const;
    Operand toOperand(const MoveOperand& operand, bool isFloat) const;

    void emitMove(const MoveOperand& from, const MoveOperand& to);
    void emitFloat32Move(const MoveOperand& from, const MoveOperand& to);
    void emitDoubleMove(const MoveOperand& from, const MoveOperand& to);
    void breakCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type);
    void completeCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type);
    void emit(const MoveOp& move);

  public:
    MoveEmitterARM(MacroAssemblerARMCompat& masm);
    ~MoveEmitterARM();
    void emit(const MoveResolver& moves);
    void finish();
};

typedef MoveEmitterARM MoveEmitter;

} // namespace jit
} // namespace js

#endif /* jit_arm_MoveEmitter_arm_h */
