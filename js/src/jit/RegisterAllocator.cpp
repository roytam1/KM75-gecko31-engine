/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/RegisterAllocator.h"

using namespace js;
using namespace js::jit;

bool
AllocationIntegrityState::record()
{
    // Ignore repeated record() calls.
    if (!instructions.empty())
        return true;

    if (!instructions.appendN(InstructionInfo(), graph.numInstructions()))
        return false;

    if (!virtualRegisters.appendN((LDefinition*)nullptr, graph.numVirtualRegisters()))
        return false;

    if (!blocks.reserve(graph.numBlocks()))
        return false;
    for (size_t i = 0; i < graph.numBlocks(); i++) {
        blocks.infallibleAppend(BlockInfo());
        LBlock* block = graph.getBlock(i);
        JS_ASSERT(block->mir()->id() == i);

        BlockInfo& blockInfo = blocks[i];
        if (!blockInfo.phis.reserve(block->numPhis()))
            return false;

        for (size_t j = 0; j < block->numPhis(); j++) {
            blockInfo.phis.infallibleAppend(InstructionInfo());
            InstructionInfo& info = blockInfo.phis[j];
            LPhi* phi = block->getPhi(j);
            JS_ASSERT(phi->numDefs() == 1);
            uint32_t vreg = phi->getDef(0)->virtualRegister();
            virtualRegisters[vreg] = phi->getDef(0);
            if (!info.outputs.append(*phi->getDef(0)))
                return false;
            for (size_t k = 0, kend = phi->numOperands(); k < kend; k++) {
                if (!info.inputs.append(*phi->getOperand(k)))
                    return false;
            }
        }

        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;
            InstructionInfo& info = instructions[ins->id()];

            for (size_t k = 0; k < ins->numTemps(); k++) {
                uint32_t vreg = ins->getTemp(k)->virtualRegister();
                virtualRegisters[vreg] = ins->getTemp(k);
                if (!info.temps.append(*ins->getTemp(k)))
                    return false;
            }
            for (size_t k = 0; k < ins->numDefs(); k++) {
                uint32_t vreg = ins->getDef(k)->virtualRegister();
                virtualRegisters[vreg] = ins->getDef(k);
                if (!info.outputs.append(*ins->getDef(k)))
                    return false;
            }
            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
                if (!info.inputs.append(**alloc))
                    return false;
            }
        }
    }

    return seen.init();
}

bool
AllocationIntegrityState::check(bool populateSafepoints)
{
    JS_ASSERT(!instructions.empty());

#ifdef DEBUG
    if (IonSpewEnabled(IonSpew_RegAlloc))
        dump();

    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);

        // Check that all instruction inputs and outputs have been assigned an allocation.
        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;

            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next())
                JS_ASSERT(!alloc->isUse());

            for (size_t i = 0; i < ins->numDefs(); i++) {
                LDefinition* def = ins->getDef(i);
                JS_ASSERT_IF(def->policy() != LDefinition::PASSTHROUGH, !def->output()->isUse());

                LDefinition oldDef = instructions[ins->id()].outputs[i];
                JS_ASSERT_IF(oldDef.policy() == LDefinition::MUST_REUSE_INPUT,
                             *def->output() == *ins->getOperand(oldDef.getReusedInput()));
            }

            for (size_t i = 0; i < ins->numTemps(); i++) {
                LDefinition* temp = ins->getTemp(i);
                JS_ASSERT_IF(!temp->isBogusTemp(), temp->output()->isRegister());

                LDefinition oldTemp = instructions[ins->id()].temps[i];
                JS_ASSERT_IF(oldTemp.policy() == LDefinition::MUST_REUSE_INPUT,
                             *temp->output() == *ins->getOperand(oldTemp.getReusedInput()));
            }
        }
    }
#endif

    // Check that the register assignment and move groups preserve the original
    // semantics of the virtual registers. Each virtual register has a single
    // write (owing to the SSA representation), but the allocation may move the
    // written value around between registers and memory locations along
    // different paths through the script.
    //
    // For each use of an allocation, follow the physical value which is read
    // backward through the script, along all paths to the value's virtual
    // register's definition.
    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);
        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;
            const InstructionInfo& info = instructions[ins->id()];

            LSafepoint* safepoint = ins->safepoint();
            if (safepoint) {
                for (size_t i = 0; i < ins->numTemps(); i++) {
                    uint32_t vreg = info.temps[i].virtualRegister();
                    LAllocation* alloc = ins->getTemp(i)->output();
                    if (!checkSafepointAllocation(ins, vreg, *alloc, populateSafepoints))
                        return false;
                }
                JS_ASSERT_IF(ins->isCall() && !populateSafepoints,
                             safepoint->liveRegs().empty(true) &&
                             safepoint->liveRegs().empty(false));
            }

            size_t inputIndex = 0;
            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
                LAllocation oldInput = info.inputs[inputIndex++];
                if (!oldInput.isUse())
                    continue;

                uint32_t vreg = oldInput.toUse()->virtualRegister();

                if (safepoint && !oldInput.toUse()->usedAtStart()) {
                    if (!checkSafepointAllocation(ins, vreg, **alloc, populateSafepoints))
                        return false;
                }

                // Start checking at the previous instruction, in case this
                // instruction reuses its input register for an output.
                LInstructionReverseIterator riter = block->rbegin(ins);
                riter++;
                checkIntegrity(block, *riter, vreg, **alloc, populateSafepoints);

                while (!worklist.empty()) {
                    IntegrityItem item = worklist.popCopy();
                    checkIntegrity(item.block, *item.block->rbegin(), item.vreg, item.alloc, populateSafepoints);
                }
            }
        }
    }

    return true;
}

bool
AllocationIntegrityState::checkIntegrity(LBlock* block, LInstruction* ins,
                                         uint32_t vreg, LAllocation alloc, bool populateSafepoints)
{
    for (LInstructionReverseIterator iter(block->rbegin(ins)); iter != block->rend(); iter++) {
        ins = *iter;

        // Follow values through assignments in move groups. All assignments in
        // a move group are considered to happen simultaneously, so stop after
        // the first matching move is found.
        if (ins->isMoveGroup()) {
            LMoveGroup* group = ins->toMoveGroup();
            for (int i = group->numMoves() - 1; i >= 0; i--) {
                if (*group->getMove(i).to() == alloc) {
                    alloc = *group->getMove(i).from();
                    break;
                }
            }
        }

        const InstructionInfo& info = instructions[ins->id()];

        // Make sure the physical location being tracked is not clobbered by
        // another instruction, and that if the originating vreg definition is
        // found that it is writing to the tracked location.

        for (size_t i = 0; i < ins->numDefs(); i++) {
            LDefinition* def = ins->getDef(i);
            if (def->policy() == LDefinition::PASSTHROUGH)
                continue;
            if (info.outputs[i].virtualRegister() == vreg) {
                JS_ASSERT(*def->output() == alloc);

                // Found the original definition, done scanning.
                return true;
            } else {
                JS_ASSERT(*def->output() != alloc);
            }
        }

        for (size_t i = 0; i < ins->numTemps(); i++) {
            LDefinition* temp = ins->getTemp(i);
            if (!temp->isBogusTemp())
                JS_ASSERT(*temp->output() != alloc);
        }

        if (ins->safepoint()) {
            if (!checkSafepointAllocation(ins, vreg, alloc, populateSafepoints))
                return false;
        }
    }

    // Phis are effectless, but change the vreg we are tracking. Check if there
    // is one which produced this vreg. We need to follow back through the phi
    // inputs as it is not guaranteed the register allocator filled in physical
    // allocations for the inputs and outputs of the phis.
    for (size_t i = 0; i < block->numPhis(); i++) {
        const InstructionInfo& info = blocks[block->mir()->id()].phis[i];
        LPhi* phi = block->getPhi(i);
        if (info.outputs[0].virtualRegister() == vreg) {
            for (size_t j = 0, jend = phi->numOperands(); j < jend; j++) {
                uint32_t newvreg = info.inputs[j].toUse()->virtualRegister();
                LBlock* predecessor = graph.getBlock(block->mir()->getPredecessor(j)->id());
                if (!addPredecessor(predecessor, newvreg, alloc))
                    return false;
            }
            return true;
        }
    }

    // No phi which defined the vreg we are tracking, follow back through all
    // predecessors with the existing vreg.
    for (size_t i = 0, iend = block->mir()->numPredecessors(); i < iend; i++) {
        LBlock* predecessor = graph.getBlock(block->mir()->getPredecessor(i)->id());
        if (!addPredecessor(predecessor, vreg, alloc))
            return false;
    }

    return true;
}

bool
AllocationIntegrityState::checkSafepointAllocation(LInstruction* ins,
                                                   uint32_t vreg, LAllocation alloc,
                                                   bool populateSafepoints)
{
    LSafepoint* safepoint = ins->safepoint();
    JS_ASSERT(safepoint);

    if (ins->isCall() && alloc.isRegister())
        return true;

    if (alloc.isRegister()) {
        AnyRegister reg = alloc.toRegister();
        if (populateSafepoints)
            safepoint->addLiveRegister(reg);
        JS_ASSERT(safepoint->liveRegs().has(reg));
    }

    LDefinition::Type type = virtualRegisters[vreg]
                             ? virtualRegisters[vreg]->type()
                             : LDefinition::GENERAL;

    switch (type) {
      case LDefinition::OBJECT:
        if (populateSafepoints) {
            IonSpew(IonSpew_RegAlloc, "Safepoint object v%u i%u %s",
                    vreg, ins->id(), alloc.toString());
            if (!safepoint->addGcPointer(alloc))
                return false;
        }
        JS_ASSERT(safepoint->hasGcPointer(alloc));
        break;
      case LDefinition::SLOTS:
        if (populateSafepoints) {
            IonSpew(IonSpew_RegAlloc, "Safepoint slots v%u i%u %s",
                    vreg, ins->id(), alloc.toString());
            if (!safepoint->addSlotsOrElementsPointer(alloc))
                return false;
        }
        JS_ASSERT(safepoint->hasSlotsOrElementsPointer(alloc));
        break;
#ifdef JS_NUNBOX32
      // Do not assert that safepoint information for nunbox types is complete,
      // as if a vreg for a value's components are copied in multiple places
      // then the safepoint information may not reflect all copies. All copies
      // of payloads must be reflected, however, for generational GC.
      case LDefinition::TYPE:
        if (populateSafepoints) {
            IonSpew(IonSpew_RegAlloc, "Safepoint type v%u i%u %s",
                    vreg, ins->id(), alloc.toString());
            if (!safepoint->addNunboxType(vreg, alloc))
                return false;
        }
        break;
      case LDefinition::PAYLOAD:
        if (populateSafepoints) {
            IonSpew(IonSpew_RegAlloc, "Safepoint payload v%u i%u %s",
                    vreg, ins->id(), alloc.toString());
            if (!safepoint->addNunboxPayload(vreg, alloc))
                return false;
        }
        JS_ASSERT(safepoint->hasNunboxPayload(alloc));
        break;
#else
      case LDefinition::BOX:
        if (populateSafepoints) {
            IonSpew(IonSpew_RegAlloc, "Safepoint boxed value v%u i%u %s",
                    vreg, ins->id(), alloc.toString());
            if (!safepoint->addBoxedValue(alloc))
                return false;
        }
        JS_ASSERT(safepoint->hasBoxedValue(alloc));
        break;
#endif
      default:
        break;
    }

    return true;
}

bool
AllocationIntegrityState::addPredecessor(LBlock* block, uint32_t vreg, LAllocation alloc)
{
    // There is no need to reanalyze if we have already seen this predecessor.
    // We share the seen allocations across analysis of each use, as there will
    // likely be common ground between different uses of the same vreg.
    IntegrityItem item;
    item.block = block;
    item.vreg = vreg;
    item.alloc = alloc;
    item.index = seen.count();

    IntegrityItemSet::AddPtr p = seen.lookupForAdd(item);
    if (p)
        return true;
    if (!seen.add(p, item))
        return false;

    return worklist.append(item);
}

void
AllocationIntegrityState::dump()
{
#ifdef DEBUG
    fprintf(stderr, "Register Allocation:\n");

    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);
        MBasicBlock* mir = block->mir();

        fprintf(stderr, "\nBlock %lu", static_cast<unsigned long>(blockIndex));
        for (size_t i = 0; i < mir->numSuccessors(); i++)
            fprintf(stderr, " [successor %u]", mir->getSuccessor(i)->id());
        fprintf(stderr, "\n");

        for (size_t i = 0; i < block->numPhis(); i++) {
            const InstructionInfo& info = blocks[blockIndex].phis[i];
            LPhi* phi = block->getPhi(i);
            CodePosition output(phi->id(), CodePosition::OUTPUT);

            // Don't print the inputOf for phi nodes, since it's never used.
            fprintf(stderr, "[,%u Phi [def v%u %s] <-",
                    output.pos(),
                    info.outputs[0].virtualRegister(),
                    phi->getDef(0)->output()->toString());
            for (size_t j = 0; j < phi->numOperands(); j++)
                fprintf(stderr, " %s", info.inputs[j].toString());
            fprintf(stderr, "]\n");
        }

        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;
            const InstructionInfo& info = instructions[ins->id()];

            CodePosition input(ins->id(), CodePosition::INPUT);
            CodePosition output(ins->id(), CodePosition::OUTPUT);

            fprintf(stderr, "[");
            if (input != CodePosition::MIN)
                fprintf(stderr, "%u,%u ", input.pos(), output.pos());
            fprintf(stderr, "%s]", ins->opName());

            if (ins->isMoveGroup()) {
                LMoveGroup* group = ins->toMoveGroup();
                for (int i = group->numMoves() - 1; i >= 0; i--) {
                    // Use two printfs, as LAllocation::toString is not reentant.
                    fprintf(stderr, " [%s", group->getMove(i).from()->toString());
                    fprintf(stderr, " -> %s]", group->getMove(i).to()->toString());
                }
                fprintf(stderr, "\n");
                continue;
            }

            for (size_t i = 0; i < ins->numTemps(); i++) {
                LDefinition* temp = ins->getTemp(i);
                if (!temp->isBogusTemp())
                    fprintf(stderr, " [temp v%u %s]", info.temps[i].virtualRegister(),
                           temp->output()->toString());
            }

            for (size_t i = 0; i < ins->numDefs(); i++) {
                LDefinition* def = ins->getDef(i);
                fprintf(stderr, " [def v%u %s]", info.outputs[i].virtualRegister(),
                       def->output()->toString());
            }

            size_t index = 0;
            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
                fprintf(stderr, " [use %s", info.inputs[index++].toString());
                fprintf(stderr, " %s]", alloc->toString());
            }

            fprintf(stderr, "\n");
        }
    }

    fprintf(stderr, "\nIntermediate Allocations:\n\n");

    // Print discovered allocations at the ends of blocks, in the order they
    // were discovered.

    Vector<IntegrityItem, 20, SystemAllocPolicy> seenOrdered;
    seenOrdered.appendN(IntegrityItem(), seen.count());

    for (IntegrityItemSet::Enum iter(seen); !iter.empty(); iter.popFront()) {
        IntegrityItem item = iter.front();
        seenOrdered[item.index] = item;
    }

    for (size_t i = 0; i < seenOrdered.length(); i++) {
        IntegrityItem item = seenOrdered[i];
        fprintf(stderr, "block %u reg v%u alloc %s\n",
               item.block->mir()->id(), item.vreg, item.alloc.toString());
    }

    fprintf(stderr, "\n");
#endif
}

const CodePosition CodePosition::MAX(UINT_MAX);
const CodePosition CodePosition::MIN(0);

bool
RegisterAllocator::init()
{
    if (!insData.init(mir, graph.numInstructions()))
        return false;

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        LBlock* block = graph.getBlock(i);
        for (LInstructionIterator ins = block->begin(); ins != block->end(); ins++)
            insData[*ins].init(*ins, block);
        for (size_t j = 0; j < block->numPhis(); j++) {
            LPhi* phi = block->getPhi(j);
            insData[phi].init(phi, block);
        }
    }

    return true;
}

LMoveGroup*
RegisterAllocator::getInputMoveGroup(uint32_t ins)
{
    InstructionData* data = &insData[ins];
    JS_ASSERT(!data->ins()->isPhi());
    JS_ASSERT(!data->ins()->isLabel());

    if (data->inputMoves())
        return data->inputMoves();

    LMoveGroup* moves = LMoveGroup::New(alloc());
    data->setInputMoves(moves);
    data->block()->insertBefore(data->ins(), moves);

    return moves;
}

LMoveGroup*
RegisterAllocator::getMoveGroupAfter(uint32_t ins)
{
    InstructionData* data = &insData[ins];
    JS_ASSERT(!data->ins()->isPhi());

    if (data->movesAfter())
        return data->movesAfter();

    LMoveGroup* moves = LMoveGroup::New(alloc());
    data->setMovesAfter(moves);

    if (data->ins()->isLabel())
        data->block()->insertAfter(data->block()->getEntryMoveGroup(alloc()), moves);
    else
        data->block()->insertAfter(data->ins(), moves);
    return moves;
}
