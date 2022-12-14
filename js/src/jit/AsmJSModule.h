/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AsmJSModule_h
#define jit_AsmJSModule_h

#ifdef JS_ION

#include "mozilla/Move.h"
#include "mozilla/PodOperations.h"

#include "jsscript.h"

#include "gc/Marking.h"
#include "jit/AsmJS.h"
#include "jit/IonMacroAssembler.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/RegisterSets.h"
#include "vm/TypedArrayObject.h"

namespace js {

// These EcmaScript-defined coercions form the basis of the asm.js type system.
enum AsmJSCoercion
{
    AsmJS_ToInt32,
    AsmJS_ToNumber,
    AsmJS_FRound
};

// The asm.js spec recognizes this set of builtin Math functions.
enum AsmJSMathBuiltinFunction
{
    AsmJSMathBuiltin_sin, AsmJSMathBuiltin_cos, AsmJSMathBuiltin_tan,
    AsmJSMathBuiltin_asin, AsmJSMathBuiltin_acos, AsmJSMathBuiltin_atan,
    AsmJSMathBuiltin_ceil, AsmJSMathBuiltin_floor, AsmJSMathBuiltin_exp,
    AsmJSMathBuiltin_log, AsmJSMathBuiltin_pow, AsmJSMathBuiltin_sqrt,
    AsmJSMathBuiltin_abs, AsmJSMathBuiltin_atan2, AsmJSMathBuiltin_imul,
    AsmJSMathBuiltin_fround, AsmJSMathBuiltin_min, AsmJSMathBuiltin_max
};

// An asm.js module represents the collection of functions nested inside a
// single outer "use asm" function. For example, this asm.js module:
//   function() { "use asm"; function f() {} function g() {} return f }
// contains the functions 'f' and 'g'.
//
// An asm.js module contains both the jit-code produced by compiling all the
// functions in the module as well all the data required to perform the
// link-time validation step in the asm.js spec.
//
// NB: this means that AsmJSModule must be GC-safe.
class AsmJSModule
{
  public:
    class Global
    {
      public:
        enum Which { Variable, FFI, ArrayView, MathBuiltinFunction, Constant };
        enum VarInitKind { InitConstant, InitImport };
        enum ConstantKind { GlobalConstant, MathConstant };

      private:
        struct Pod {
            Which which_;
            union {
                struct {
                    uint32_t index_;
                    VarInitKind initKind_;
                    AsmJSCoercion coercion_;
                    union {
                        Value constant_; // will only contain int32/double
                    } init;
                } var;
                uint32_t ffiIndex_;
                ArrayBufferView::ViewType viewType_;
                AsmJSMathBuiltinFunction mathBuiltinFunc_;
                struct {
                    ConstantKind kind_;
                    double value_;
                } constant;
            } u;
        } pod;
        PropertyName* name_;

        friend class AsmJSModule;

        Global(Which which, PropertyName* name) {
            pod.which_ = which;
            name_ = name;
            JS_ASSERT_IF(name_, name_->isTenured());
        }

        void trace(JSTracer* trc) {
            if (name_)
                MarkStringUnbarriered(trc, &name_, "asm.js global name");
            JS_ASSERT_IF(pod.which_ == Variable && pod.u.var.initKind_ == InitConstant,
                         !pod.u.var.init.constant_.isMarkable());
        }

      public:
        Global() {}
        Which which() const {
            return pod.which_;
        }
        uint32_t varIndex() const {
            JS_ASSERT(pod.which_ == Variable);
            return pod.u.var.index_;
        }
        VarInitKind varInitKind() const {
            JS_ASSERT(pod.which_ == Variable);
            return pod.u.var.initKind_;
        }
        const Value& varInitConstant() const {
            JS_ASSERT(pod.which_ == Variable);
            JS_ASSERT(pod.u.var.initKind_ == InitConstant);
            return pod.u.var.init.constant_;
        }
        AsmJSCoercion varInitCoercion() const {
            JS_ASSERT(pod.which_ == Variable);
            return pod.u.var.coercion_;
        }
        PropertyName* varImportField() const {
            JS_ASSERT(pod.which_ == Variable);
            JS_ASSERT(pod.u.var.initKind_ == InitImport);
            return name_;
        }
        PropertyName* ffiField() const {
            JS_ASSERT(pod.which_ == FFI);
            return name_;
        }
        uint32_t ffiIndex() const {
            JS_ASSERT(pod.which_ == FFI);
            return pod.u.ffiIndex_;
        }
        PropertyName* viewName() const {
            JS_ASSERT(pod.which_ == ArrayView);
            return name_;
        }
        ArrayBufferView::ViewType viewType() const {
            JS_ASSERT(pod.which_ == ArrayView);
            return pod.u.viewType_;
        }
        PropertyName* mathName() const {
            JS_ASSERT(pod.which_ == MathBuiltinFunction);
            return name_;
        }
        AsmJSMathBuiltinFunction mathBuiltinFunction() const {
            JS_ASSERT(pod.which_ == MathBuiltinFunction);
            return pod.u.mathBuiltinFunc_;
        }
        PropertyName* constantName() const {
            JS_ASSERT(pod.which_ == Constant);
            return name_;
        }
        ConstantKind constantKind() const {
            JS_ASSERT(pod.which_ == Constant);
            return pod.u.constant.kind_;
        }
        double constantValue() const {
            JS_ASSERT(pod.which_ == Constant);
            return pod.u.constant.value_;
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, Global* out) const;
    };

    class Exit
    {
        unsigned ffiIndex_;
        unsigned globalDataOffset_;
        unsigned interpCodeOffset_;
        unsigned ionCodeOffset_;

        friend class AsmJSModule;

      public:
        Exit() {}
        Exit(unsigned ffiIndex, unsigned globalDataOffset)
          : ffiIndex_(ffiIndex), globalDataOffset_(globalDataOffset),
            interpCodeOffset_(0), ionCodeOffset_(0)
        {}
        unsigned ffiIndex() const {
            return ffiIndex_;
        }
        unsigned globalDataOffset() const {
            return globalDataOffset_;
        }
        void initInterpOffset(unsigned off) {
            JS_ASSERT(!interpCodeOffset_);
            interpCodeOffset_ = off;
        }
        void initIonOffset(unsigned off) {
            JS_ASSERT(!ionCodeOffset_);
            ionCodeOffset_ = off;
        }
        void updateOffsets(jit::MacroAssembler& masm) {
            interpCodeOffset_ = masm.actualOffset(interpCodeOffset_);
            ionCodeOffset_ = masm.actualOffset(ionCodeOffset_);
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, Exit* out) const;
    };
    typedef int32_t (*CodePtr)(uint64_t* args, uint8_t* global);

    typedef Vector<AsmJSCoercion, 0, SystemAllocPolicy> ArgCoercionVector;

    enum ReturnType { Return_Int32, Return_Double, Return_Void };

    class ExportedFunction
    {
        PropertyName* name_;
        PropertyName* maybeFieldName_;
        ArgCoercionVector argCoercions_;
        struct Pod {
            ReturnType returnType_;
            uint32_t codeOffset_;
            // These two fields are offsets to the beginning of the ScriptSource
            // of the module, and thus invariant under serialization (unlike
            // absolute offsets into ScriptSource).
            uint32_t startOffsetInModule_;
            uint32_t endOffsetInModule_;
        } pod;

        friend class AsmJSModule;

        ExportedFunction(PropertyName* name,
                         uint32_t startOffsetInModule, uint32_t endOffsetInModule,
                         PropertyName* maybeFieldName,
                         ArgCoercionVector&& argCoercions,
                         ReturnType returnType)
        {
            name_ = name;
            maybeFieldName_ = maybeFieldName;
            argCoercions_ = mozilla::Move(argCoercions);
            pod.returnType_ = returnType;
            pod.codeOffset_ = UINT32_MAX;
            pod.startOffsetInModule_ = startOffsetInModule;
            pod.endOffsetInModule_ = endOffsetInModule;
            JS_ASSERT_IF(maybeFieldName_, name_->isTenured());
        }

        void trace(JSTracer* trc) {
            MarkStringUnbarriered(trc, &name_, "asm.js export name");
            if (maybeFieldName_)
                MarkStringUnbarriered(trc, &maybeFieldName_, "asm.js export field");
        }

      public:
        ExportedFunction() {}
        ExportedFunction(ExportedFunction&& rhs) {
            name_ = rhs.name_;
            maybeFieldName_ = rhs.maybeFieldName_;
            argCoercions_ = mozilla::Move(rhs.argCoercions_);
            pod = rhs.pod;
        }
        void updateCodeOffset(jit::MacroAssembler& masm) {
            pod.codeOffset_ = masm.actualOffset(pod.codeOffset_);
        }

        void initCodeOffset(unsigned off) {
            JS_ASSERT(pod.codeOffset_ == UINT32_MAX);
            pod.codeOffset_ = off;
        }

        PropertyName* name() const {
            return name_;
        }
        uint32_t startOffsetInModule() const {
            return pod.startOffsetInModule_;
        }
        uint32_t endOffsetInModule() const {
            return pod.endOffsetInModule_;
        }
        PropertyName* maybeFieldName() const {
            return maybeFieldName_;
        }
        unsigned numArgs() const {
            return argCoercions_.length();
        }
        AsmJSCoercion argCoercion(unsigned i) const {
            return argCoercions_[i];
        }
        ReturnType returnType() const {
            return pod.returnType_;
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, ExportedFunction* out) const;
    };

    class Name
    {
        PropertyName* name_;
      public:
        Name() : name_(nullptr) {}
        Name(PropertyName* name) : name_(name) {}
        PropertyName* name() const { return name_; }
        PropertyName*& name() { return name_; }
        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, Name* out) const;
    };

#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    // Function information to add to the VTune JIT profiler following linking.
    struct ProfiledFunction
    {
        PropertyName* name;
        struct Pod {
            unsigned startCodeOffset;
            unsigned endCodeOffset;
            unsigned lineno;
            unsigned columnIndex;
        } pod;

        explicit ProfiledFunction()
          : name(nullptr)
        { }

        ProfiledFunction(PropertyName* name, unsigned start, unsigned end,
                         unsigned line = 0, unsigned column = 0)
          : name(name)
        {
            JS_ASSERT(name->isTenured());

            pod.startCodeOffset = start;
            pod.endCodeOffset = end;
            pod.lineno = line;
            pod.columnIndex = column;
        }

        void trace(JSTracer* trc) {
            if (name)
                MarkStringUnbarriered(trc, &name, "asm.js profiled function name");
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
    };
#endif

#if defined(JS_ION_PERF)
    struct ProfiledBlocksFunction : public ProfiledFunction
    {
        unsigned endInlineCodeOffset;
        jit::BasicBlocksVector blocks;

        ProfiledBlocksFunction(PropertyName* name, unsigned start, unsigned endInline, unsigned end,
                               jit::BasicBlocksVector& blocksVector)
          : ProfiledFunction(name, start, end), endInlineCodeOffset(endInline),
            blocks(mozilla::Move(blocksVector))
        {
            JS_ASSERT(name->isTenured());
        }

        ProfiledBlocksFunction(ProfiledBlocksFunction&& copy)
          : ProfiledFunction(copy.name, copy.pod.startCodeOffset, copy.pod.endCodeOffset),
            endInlineCodeOffset(copy.endInlineCodeOffset), blocks(mozilla::Move(copy.blocks))
        { }
    };
#endif

    struct RelativeLink
    {
        uint32_t patchAtOffset;
        uint32_t targetOffset;
    };

    typedef Vector<RelativeLink, 0, SystemAllocPolicy> RelativeLinkVector;

    struct AbsoluteLink
    {
        jit::CodeOffsetLabel patchAt;
        jit::AsmJSImmKind target;
    };

    typedef Vector<AbsoluteLink, 0, SystemAllocPolicy> AbsoluteLinkVector;

    // Static-link data is used to patch a module either after it has been
    // compiled or deserialized with various absolute addresses (of code or
    // data in the process) or relative addresses (of code or data in the same
    // AsmJSModule).
    struct StaticLinkData
    {
        uint32_t interruptExitOffset;
        RelativeLinkVector relativeLinks;
        AbsoluteLinkVector absoluteLinks;

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, StaticLinkData* out) const;

        size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
    };

  private:
    typedef Vector<Global, 0, SystemAllocPolicy> GlobalVector;
    typedef Vector<Exit, 0, SystemAllocPolicy> ExitVector;
    typedef Vector<ExportedFunction, 0, SystemAllocPolicy> ExportedFunctionVector;
    typedef Vector<jit::CallSite, 0, SystemAllocPolicy> CallSiteVector;
    typedef Vector<Name, 0, SystemAllocPolicy> FunctionNameVector;
    typedef Vector<jit::AsmJSHeapAccess, 0, SystemAllocPolicy> HeapAccessVector;
    typedef Vector<jit::IonScriptCounts*, 0, SystemAllocPolicy> FunctionCountsVector;
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    typedef Vector<ProfiledFunction, 0, SystemAllocPolicy> ProfiledFunctionVector;
#endif
#if defined(JS_ION_PERF)
    typedef Vector<ProfiledBlocksFunction, 0, SystemAllocPolicy> ProfiledBlocksFunctionVector;
#endif

  private:
    PropertyName *                        globalArgumentName_;
    PropertyName *                        importArgumentName_;
    PropertyName *                        bufferArgumentName_;

    GlobalVector                          globals_;
    ExitVector                            exits_;
    ExportedFunctionVector                exports_;
    CallSiteVector                        callSites_;
    FunctionNameVector                    functionNames_;
    HeapAccessVector                      heapAccesses_;
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    ProfiledFunctionVector                profiledFunctions_;
#endif
#if defined(JS_ION_PERF)
    ProfiledBlocksFunctionVector          perfProfiledBlocksFunctions_;
#endif

    struct Pod {
        uint32_t                          funcLength_;
        uint32_t                          funcLengthWithRightBrace_;
        bool                              strict_;
        uint32_t                          numGlobalVars_;
        uint32_t                          numFFIs_;
        size_t                            funcPtrTableAndExitBytes_;
        bool                              hasArrayView_;
        size_t                            functionBytes_; // just the function bodies, no stubs
        size_t                            codeBytes_;     // function bodies and stubs
        size_t                            totalBytes_;    // function bodies, stubs, and global data
        uint32_t                          minHeapLength_;
    } pod;

    uint8_t *                             code_;
    uint8_t *                             interruptExit_;

    StaticLinkData                        staticLinkData_;
    bool                                  dynamicallyLinked_;
    bool                                  loadedFromCache_;
    HeapPtr<ArrayBufferObject>            maybeHeap_;

    // The next two fields need to be kept out of the Pod as they depend on the
    // position of the module within the ScriptSource and thus aren't invariant
    // with caching.
    uint32_t                              funcStart_;
    uint32_t                              offsetToEndOfUseAsm_;

    ScriptSource *                        scriptSource_;

    // This field is accessed concurrently when requesting an interrupt.
    // Access must be synchronized via the runtime's interrupt lock.
    mutable bool                          codeIsProtected_;

  public:
    explicit AsmJSModule(ScriptSource* scriptSource, uint32_t functStart,
                         uint32_t offsetToEndOfUseAsm, bool strict);
    ~AsmJSModule();

    void trace(JSTracer* trc) {
        for (unsigned i = 0; i < globals_.length(); i++)
            globals_[i].trace(trc);
        for (unsigned i = 0; i < exports_.length(); i++)
            exports_[i].trace(trc);
        for (unsigned i = 0; i < exits_.length(); i++) {
            if (exitIndexToGlobalDatum(i).fun)
                MarkObject(trc, &exitIndexToGlobalDatum(i).fun, "asm.js imported function");
        }
        for (unsigned i = 0; i < functionNames_.length(); i++)
            MarkStringUnbarriered(trc, &functionNames_[i].name(), "asm.js module function name");
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
        for (unsigned i = 0; i < profiledFunctions_.length(); i++)
            profiledFunctions_[i].trace(trc);
#endif
#if defined(JS_ION_PERF)
        for (unsigned i = 0; i < perfProfiledBlocksFunctions_.length(); i++)
            perfProfiledBlocksFunctions_[i].trace(trc);
#endif
        if (maybeHeap_)
            gc::MarkObject(trc, &maybeHeap_, "asm.js heap");

        if (globalArgumentName_)
            MarkStringUnbarriered(trc, &globalArgumentName_, "asm.js global argument name");
        if (importArgumentName_)
            MarkStringUnbarriered(trc, &importArgumentName_, "asm.js import argument name");
        if (bufferArgumentName_)
            MarkStringUnbarriered(trc, &bufferArgumentName_, "asm.js buffer argument name");
    }

    ScriptSource* scriptSource() const {
        JS_ASSERT(scriptSource_ != nullptr);
        return scriptSource_;
    }

    /*
     * funcStart() refers to the offset in the ScriptSource to the beginning
     * of the function. If the function has been created with the Function
     * constructor, this will be the first character in the function source.
     * Otherwise, it will be the opening parenthesis of the arguments list.
     */
    uint32_t funcStart() const {
        return funcStart_;
    }
    uint32_t offsetToEndOfUseAsm() const {
        return offsetToEndOfUseAsm_;
    }
    void initFuncEnd(uint32_t endBeforeCurly, uint32_t endAfterCurly) {
        JS_ASSERT(endBeforeCurly >= offsetToEndOfUseAsm_);
        JS_ASSERT(endAfterCurly >= offsetToEndOfUseAsm_);
        pod.funcLength_ = endBeforeCurly - funcStart_;
        pod.funcLengthWithRightBrace_ = endAfterCurly - funcStart_;
    }
    uint32_t funcEndBeforeCurly() const {
        return funcStart_ + pod.funcLength_;
    }
    uint32_t funcEndAfterCurly() const {
        return funcStart_ + pod.funcLengthWithRightBrace_;
    }
    bool strict() const {
        return pod.strict_;
    }

    bool addGlobalVarInit(const Value& v, AsmJSCoercion coercion, uint32_t* globalIndex) {
        JS_ASSERT(pod.funcPtrTableAndExitBytes_ == 0);
        if (pod.numGlobalVars_ == UINT32_MAX)
            return false;
        Global g(Global::Variable, nullptr);
        g.pod.u.var.initKind_ = Global::InitConstant;
        g.pod.u.var.init.constant_ = v;
        g.pod.u.var.coercion_ = coercion;
        g.pod.u.var.index_ = *globalIndex = pod.numGlobalVars_++;
        return globals_.append(g);
    }
    bool addGlobalVarImport(PropertyName* name, AsmJSCoercion coercion, uint32_t* globalIndex) {
        JS_ASSERT(pod.funcPtrTableAndExitBytes_ == 0);
        Global g(Global::Variable, name);
        g.pod.u.var.initKind_ = Global::InitImport;
        g.pod.u.var.coercion_ = coercion;
        g.pod.u.var.index_ = *globalIndex = pod.numGlobalVars_++;
        return globals_.append(g);
    }
    bool addFFI(PropertyName* field, uint32_t* ffiIndex) {
        if (pod.numFFIs_ == UINT32_MAX)
            return false;
        Global g(Global::FFI, field);
        g.pod.u.ffiIndex_ = *ffiIndex = pod.numFFIs_++;
        return globals_.append(g);
    }
    bool addArrayView(ArrayBufferView::ViewType vt, PropertyName* field) {
        pod.hasArrayView_ = true;
        Global g(Global::ArrayView, field);
        g.pod.u.viewType_ = vt;
        return globals_.append(g);
    }
    bool addMathBuiltinFunction(AsmJSMathBuiltinFunction func, PropertyName* field) {
        Global g(Global::MathBuiltinFunction, field);
        g.pod.u.mathBuiltinFunc_ = func;
        return globals_.append(g);
    }
    bool addMathBuiltinConstant(double value, PropertyName* field) {
        Global g(Global::Constant, field);
        g.pod.u.constant.value_ = value;
        g.pod.u.constant.kind_ = Global::MathConstant;
        return globals_.append(g);
    }
    bool addGlobalConstant(double value, PropertyName* name) {
        Global g(Global::Constant, name);
        g.pod.u.constant.value_ = value;
        g.pod.u.constant.kind_ = Global::GlobalConstant;
        return globals_.append(g);
    }
    bool addFuncPtrTable(unsigned numElems, uint32_t* globalDataOffset) {
        JS_ASSERT(IsPowerOfTwo(numElems));
        if (SIZE_MAX - pod.funcPtrTableAndExitBytes_ < numElems * sizeof(void*))
            return false;
        *globalDataOffset = globalDataBytes();
        pod.funcPtrTableAndExitBytes_ += numElems * sizeof(void*);
        return true;
    }
    bool addExit(unsigned ffiIndex, unsigned* exitIndex) {
        if (SIZE_MAX - pod.funcPtrTableAndExitBytes_ < sizeof(ExitDatum))
            return false;
        uint32_t globalDataOffset = globalDataBytes();
        JS_STATIC_ASSERT(sizeof(ExitDatum) % sizeof(void*) == 0);
        pod.funcPtrTableAndExitBytes_ += sizeof(ExitDatum);
        *exitIndex = unsigned(exits_.length());
        return exits_.append(Exit(ffiIndex, globalDataOffset));
    }

    bool addExportedFunction(PropertyName* name, uint32_t srcStart, uint32_t srcEnd,
                             PropertyName* maybeFieldName,
                             ArgCoercionVector&& argCoercions,
                             ReturnType returnType)
    {
        ExportedFunction func(name, srcStart, srcEnd, maybeFieldName,
                              mozilla::Move(argCoercions), returnType);
        if (exports_.length() >= UINT32_MAX)
            return false;
        return exports_.append(mozilla::Move(func));
    }
    unsigned numExportedFunctions() const {
        return exports_.length();
    }
    const ExportedFunction& exportedFunction(unsigned i) const {
        return exports_[i];
    }
    ExportedFunction& exportedFunction(unsigned i) {
        return exports_[i];
    }
    CodePtr entryTrampoline(const ExportedFunction& func) const {
        JS_ASSERT(func.pod.codeOffset_ != UINT32_MAX);
        return JS_DATA_TO_FUNC_PTR(CodePtr, code_ + func.pod.codeOffset_);
    }

    bool addFunctionName(PropertyName* name, uint32_t* nameIndex) {
        JS_ASSERT(name->isTenured());
        if (functionNames_.length() > jit::CallSiteDesc::FUNCTION_NAME_INDEX_MAX)
            return false;
        *nameIndex = functionNames_.length();
        return functionNames_.append(name);
    }
    PropertyName* functionName(uint32_t i) const {
        return functionNames_[i].name();
    }

#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    bool trackProfiledFunction(PropertyName* name, unsigned startCodeOffset, unsigned endCodeOffset,
                               unsigned line, unsigned column)
    {
        ProfiledFunction func(name, startCodeOffset, endCodeOffset, line, column);
        return profiledFunctions_.append(func);
    }
    unsigned numProfiledFunctions() const {
        return profiledFunctions_.length();
    }
    ProfiledFunction& profiledFunction(unsigned i) {
        return profiledFunctions_[i];
    }
#endif

#ifdef JS_ION_PERF
    bool trackPerfProfiledBlocks(PropertyName* name, unsigned startCodeOffset, unsigned endInlineCodeOffset,
                                 unsigned endCodeOffset, jit::BasicBlocksVector& basicBlocks) {
        ProfiledBlocksFunction func(name, startCodeOffset, endInlineCodeOffset, endCodeOffset, basicBlocks);
        return perfProfiledBlocksFunctions_.append(mozilla::Move(func));
    }
    unsigned numPerfBlocksFunctions() const {
        return perfProfiledBlocksFunctions_.length();
    }
    ProfiledBlocksFunction& perfProfiledBlocksFunction(unsigned i) {
        return perfProfiledBlocksFunctions_[i];
    }
#endif

    bool hasArrayView() const {
        return pod.hasArrayView_;
    }
    unsigned numFFIs() const {
        return pod.numFFIs_;
    }
    unsigned numGlobalVars() const {
        return pod.numGlobalVars_;
    }
    unsigned numGlobals() const {
        return globals_.length();
    }
    Global& global(unsigned i) {
        return globals_[i];
    }
    unsigned numExits() const {
        return exits_.length();
    }
    Exit& exit(unsigned i) {
        return exits_[i];
    }
    const Exit& exit(unsigned i) const {
        return exits_[i];
    }
    uint8_t* interpExitTrampoline(const Exit& exit) const {
        JS_ASSERT(exit.interpCodeOffset_);
        return code_ + exit.interpCodeOffset_;
    }
    uint8_t* ionExitTrampoline(const Exit& exit) const {
        JS_ASSERT(exit.ionCodeOffset_);
        return code_ + exit.ionCodeOffset_;
    }

    // An Exit holds bookkeeping information about an exit; the ExitDatum
    // struct overlays the actual runtime data stored in the global data
    // section.
    struct ExitDatum
    {
        uint8_t* exit;
        HeapPtrFunction fun;
    };

    // Global data section
    //
    // The global data section is placed after the executable code (i.e., at
    // offset codeBytes_) in the module's linear allocation. The global data
    // are laid out in this order:
    //   0. a pointer/descriptor for the heap that was linked to the module
    //   1. global variable state (elements are sizeof(uint64_t))
    //   2. interleaved function-pointer tables and exits. These are allocated
    //      while type checking function bodies (as exits and uses of
    //      function-pointer tables are encountered).
    size_t offsetOfGlobalData() const {
        JS_ASSERT(code_);
        return pod.codeBytes_;
    }
    uint8_t* globalData() const {
        return code_ + offsetOfGlobalData();
    }
    size_t globalDataBytes() const {
        return sizeof(void*) +
               pod.numGlobalVars_ * sizeof(uint64_t) +
               pod.funcPtrTableAndExitBytes_;
    }
    unsigned heapOffset() const {
        return 0;
    }
    uint8_t*& heapDatum() const {
        return *(uint8_t**)(globalData() + heapOffset());
    }
    unsigned globalVarIndexToGlobalDataOffset(unsigned i) const {
        JS_ASSERT(i < pod.numGlobalVars_);
        return sizeof(void*) +
               i * sizeof(uint64_t);
    }
    void* globalVarIndexToGlobalDatum(unsigned i) const {
        return (void*)(globalData() + globalVarIndexToGlobalDataOffset(i));
    }
    uint8_t** globalDataOffsetToFuncPtrTable(unsigned globalDataOffset) const {
        JS_ASSERT(globalDataOffset < globalDataBytes());
        return (uint8_t**)(globalData() + globalDataOffset);
    }
    unsigned exitIndexToGlobalDataOffset(unsigned exitIndex) const {
        return exits_[exitIndex].globalDataOffset();
    }
    ExitDatum& exitIndexToGlobalDatum(unsigned exitIndex) const {
        return *(ExitDatum*)(globalData() + exitIndexToGlobalDataOffset(exitIndex));
    }

    void initFunctionBytes(size_t functionBytes) {
        JS_ASSERT(pod.functionBytes_ == 0);
        pod.functionBytes_ = functionBytes;
    }
    void updateFunctionBytes(jit::MacroAssembler& masm) {
        pod.functionBytes_ = masm.actualOffset(pod.functionBytes_);
        JS_ASSERT(pod.functionBytes_ % AsmJSPageSize == 0);
    }
    size_t functionBytes() const {
        JS_ASSERT(pod.functionBytes_);
        JS_ASSERT(pod.functionBytes_ % AsmJSPageSize == 0);
        return pod.functionBytes_;
    }
    bool containsPC(void* pc) const {
        return pc >= code_ && pc < (code_ + functionBytes());
    }

    void assignHeapAccesses(jit::AsmJSHeapAccessVector&& accesses) {
        heapAccesses_ = Move(accesses);
    }
    unsigned numHeapAccesses() const {
        return heapAccesses_.length();
    }
    const jit::AsmJSHeapAccess& heapAccess(unsigned i) const {
        return heapAccesses_[i];
    }
    jit::AsmJSHeapAccess& heapAccess(unsigned i) {
        return heapAccesses_[i];
    }

    void assignCallSites(jit::CallSiteVector&& callsites) {
        callSites_ = Move(callsites);
    }
    unsigned numCallSites() const {
        return callSites_.length();
    }
    const jit::CallSite& callSite(unsigned i) const {
        return callSites_[i];
    }
    jit::CallSite& callSite(unsigned i) {
        return callSites_[i];
    }

    void initHeap(Handle<ArrayBufferObject*> heap, JSContext* cx);

    void requireHeapLengthToBeAtLeast(uint32_t len) {
        if (len > pod.minHeapLength_)
            pod.minHeapLength_ = len;
    }
    uint32_t minHeapLength() const {
        return pod.minHeapLength_;
    }

    bool allocateAndCopyCode(ExclusiveContext* cx, jit::MacroAssembler& masm);

    // StaticLinkData setters (called after finishing compilation, before
    // staticLink).
    bool addRelativeLink(RelativeLink link) {
        return staticLinkData_.relativeLinks.append(link);
    }
    bool addAbsoluteLink(AbsoluteLink link) {
        return staticLinkData_.absoluteLinks.append(link);
    }
    void setInterruptOffset(uint32_t offset) {
        staticLinkData_.interruptExitOffset = offset;
    }

    void restoreToInitialState(ArrayBufferObject* maybePrevBuffer, ExclusiveContext* cx);
    void setAutoFlushICacheRange();
    void staticallyLink(ExclusiveContext* cx);

    uint8_t* codeBase() const {
        JS_ASSERT(code_);
        JS_ASSERT(uintptr_t(code_) % AsmJSPageSize == 0);
        return code_;
    }

    uint8_t* interruptExit() const {
        return interruptExit_;
    }

    void setIsDynamicallyLinked() {
        JS_ASSERT(!dynamicallyLinked_);
        dynamicallyLinked_ = true;
    }
    bool isDynamicallyLinked() const {
        return dynamicallyLinked_;
    }
    uint8_t* maybeHeap() const {
        JS_ASSERT(dynamicallyLinked_);
        return heapDatum();
    }
    ArrayBufferObject* maybeHeapBufferObject() const {
        JS_ASSERT(dynamicallyLinked_);
        return maybeHeap_;
    }
    size_t heapLength() const {
        JS_ASSERT(dynamicallyLinked_);
        return maybeHeap_ ? maybeHeap_->byteLength() : 0;
    }

    void initGlobalArgumentName(PropertyName* n) {
        JS_ASSERT_IF(n, n->isTenured());
        globalArgumentName_ = n;
    }
    void initImportArgumentName(PropertyName* n) {
        JS_ASSERT_IF(n, n->isTenured());
        importArgumentName_ = n;
    }
    void initBufferArgumentName(PropertyName* n) {
        JS_ASSERT_IF(n, n->isTenured());
        bufferArgumentName_ = n;
    }

    PropertyName* globalArgumentName() const {
        return globalArgumentName_;
    }
    PropertyName* importArgumentName() const {
        return importArgumentName_;
    }
    PropertyName* bufferArgumentName() const {
        return bufferArgumentName_;
    }

    void detachIonCompilation(size_t exitIndex) const {
        exitIndexToGlobalDatum(exitIndex).exit = interpExitTrampoline(exit(exitIndex));
    }

    void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* asmJSModuleCode,
                       size_t* asmJSModuleData);

    size_t serializedSize() const;
    uint8_t* serialize(uint8_t* cursor) const;
    const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
    bool loadedFromCache() const { return loadedFromCache_; }

    bool clone(JSContext* cx, ScopedJSDeletePtr<AsmJSModule>* moduleOut) const;

    // These methods may only be called while holding the Runtime's interrupt
    // lock.
    void protectCode(JSRuntime* rt) const;
    void unprotectCode(JSRuntime* rt) const;
    bool codeIsProtected(JSRuntime* rt) const;
};

// Store the just-parsed module in the cache using AsmJSCacheOps.
extern bool
StoreAsmJSModuleInCache(AsmJSParser& parser,
                        const AsmJSModule& module,
                        ExclusiveContext* cx);

// Attempt to load the asm.js module that is about to be parsed from the cache
// using AsmJSCacheOps. On cache hit, *module will be non-null. Note: the
// return value indicates whether or not an error was encountered, not whether
// there was a cache hit.
extern bool
LookupAsmJSModuleInCache(ExclusiveContext* cx,
                         AsmJSParser& parser,
                         ScopedJSDeletePtr<AsmJSModule>* module,
                         ScopedJSFreePtr<char>* compilationTimeReport);

// An AsmJSModuleObject is an internal implementation object (i.e., not exposed
// directly to user script) which manages the lifetime of an AsmJSModule. A
// JSObject is necessary since we want LinkAsmJS/CallAsmJS JSFunctions to be
// able to point to their module via their extended slots.
class AsmJSModuleObject : public JSObject
{
    static const unsigned MODULE_SLOT = 0;

  public:
    static const unsigned RESERVED_SLOTS = 1;

    // On success, return an AsmJSModuleClass JSObject that has taken ownership
    // (and release()ed) the given module.
    static AsmJSModuleObject* create(ExclusiveContext* cx, ScopedJSDeletePtr<AsmJSModule>* module);

    AsmJSModule& module() const;

    void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* asmJSModuleCode,
                       size_t* asmJSModuleData) {
        module().addSizeOfMisc(mallocSizeOf, asmJSModuleCode, asmJSModuleData);
    }

    static const Class class_;
};

}  // namespace js

#endif  // JS_ION

#endif /* jit_AsmJSModule_h */
