/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_OldDebugAPI_h
#define js_OldDebugAPI_h

/*
 * JS debugger API.
 */

#include "mozilla/NullPtr.h"

#include "jsapi.h"
#include "jsbytecode.h"

#include "js/CallArgs.h"
#include "js/TypeDecls.h"

class JSAtom;
class JSFreeOp;

namespace js {
class InterpreterFrame;
class ScriptFrameIter;
}

// Raw JSScript* because this needs to be callable from a signal handler.
extern JS_PUBLIC_API(unsigned)
JS_PCToLineNumber(JSContext* cx, JSScript* script, jsbytecode* pc);

extern JS_PUBLIC_API(const char*)
JS_GetScriptFilename(JSScript* script);

namespace JS {

class FrameDescription
{
  public:
    explicit FrameDescription(const js::ScriptFrameIter& iter);

    unsigned lineno() {
        if (!linenoComputed) {
            lineno_ = JS_PCToLineNumber(nullptr, script_, pc_);
            linenoComputed = true;
        }
        return lineno_;
    }

    const char* filename() const {
        return JS_GetScriptFilename(script_);
    }

    JSFlatString* funDisplayName() const {
        return funDisplayName_ ? JS_ASSERT_STRING_IS_FLAT(funDisplayName_) : nullptr;
    }

    // Both these locations should be traced during GC but otherwise not used;
    // they are implementation details.
    Heap<JSScript*>& markedLocation1() {
        return script_;
    }
    Heap<JSString*>& markedLocation2() {
        return funDisplayName_;
    }

  private:
    Heap<JSScript*> script_;
    Heap<JSString*> funDisplayName_;
    jsbytecode* pc_;
    unsigned lineno_;
    bool linenoComputed;
};

struct StackDescription
{
    unsigned nframes;
    FrameDescription* frames;
};

extern JS_PUBLIC_API(StackDescription*)
DescribeStack(JSContext* cx, unsigned maxFrames);

extern JS_PUBLIC_API(void)
FreeStackDescription(JSContext* cx, StackDescription* desc);

extern JS_PUBLIC_API(char*)
FormatStackDump(JSContext* cx, char* buf, bool showArgs, bool showLocals, bool showThisProps);

} // namespace JS

# ifdef JS_DEBUG
JS_FRIEND_API(void) js_DumpValue(const JS::Value& val);
JS_FRIEND_API(void) js_DumpId(jsid id);
JS_FRIEND_API(void) js_DumpInterpreterFrame(JSContext* cx, js::InterpreterFrame* start = nullptr);
# endif

JS_FRIEND_API(void)
js_DumpBacktrace(JSContext* cx);

typedef enum JSTrapStatus {
    JSTRAP_ERROR,
    JSTRAP_CONTINUE,
    JSTRAP_RETURN,
    JSTRAP_THROW,
    JSTRAP_LIMIT
} JSTrapStatus;

typedef JSTrapStatus
(* JSTrapHandler)(JSContext* cx, JSScript* script, jsbytecode* pc, JS::Value* rval,
                  JS::Value closure);

typedef JSTrapStatus
(* JSInterruptHook)(JSContext* cx, JSScript* script, jsbytecode* pc, JS::Value* rval,
                    void* closure);

typedef JSTrapStatus
(* JSDebuggerHandler)(JSContext* cx, JSScript* script, jsbytecode* pc, JS::Value* rval,
                      void* closure);

typedef JSTrapStatus
(* JSThrowHook)(JSContext* cx, JSScript* script, jsbytecode* pc, JS::Value* rval,
                void* closure);

typedef bool
(* JSWatchPointHandler)(JSContext* cx, JSObject* obj, jsid id, JS::Value old,
                        JS::Value* newp, void* closure);

/* called just after script creation */
typedef void
(* JSNewScriptHook)(JSContext* cx,
                    const char* filename,  /* URL of script */
                    unsigned   lineno,     /* first line */
                    JSScript*  script,
                    JSFunction* fun,
                    void*      callerdata);

/* called just before script destruction */
typedef void
(* JSDestroyScriptHook)(JSFreeOp* fop,
                        JSScript* script,
                        void*    callerdata);

typedef void
(* JSSourceHandler)(const char* filename, unsigned lineno, const jschar* str,
                    size_t length, void** listenerTSData, void* closure);



extern JS_PUBLIC_API(JSCompartment*)
JS_EnterCompartmentOfScript(JSContext* cx, JSScript* target);

extern JS_PUBLIC_API(JSString*)
JS_DecompileScript(JSContext* cx, JS::HandleScript script, const char* name, unsigned indent);

/*
 * Currently, we only support runtime-wide debugging. In the future, we should
 * be able to support compartment-wide debugging.
 */
extern JS_PUBLIC_API(void)
JS_SetRuntimeDebugMode(JSRuntime* rt, bool debug);

/*
 * Debug mode is a compartment-wide mode that enables a debugger to attach
 * to and interact with running methodjit-ed frames. In particular, it causes
 * every function to be compiled as if an eval was present (so eval-in-frame)
 * can work, and it ensures that functions can be re-JITed for other debug
 * features. In general, it is not safe to interact with frames that were live
 * before debug mode was enabled. For this reason, it is also not safe to
 * enable debug mode while frames are live.
 */

/* Get current state of debugging mode. */
extern JS_PUBLIC_API(bool)
JS_GetDebugMode(JSContext* cx);

/*
 * Turn on/off debugging mode for all compartments. This returns false if any code
 * from any of the runtime's compartments is running or on the stack.
 */
JS_FRIEND_API(bool)
JS_SetDebugModeForAllCompartments(JSContext* cx, bool debug);

/*
 * Turn on/off debugging mode for a single compartment. This should only be
 * used when no code from this compartment is running or on the stack in any
 * thread.
 */
JS_FRIEND_API(bool)
JS_SetDebugModeForCompartment(JSContext* cx, JSCompartment* comp, bool debug);

/*
 * Turn on/off debugging mode for a context's compartment.
 */
JS_FRIEND_API(bool)
JS_SetDebugMode(JSContext* cx, bool debug);

/* Turn on single step mode. */
extern JS_PUBLIC_API(bool)
JS_SetSingleStepMode(JSContext* cx, JS::HandleScript script, bool singleStep);

/* The closure argument will be marked. */
extern JS_PUBLIC_API(bool)
JS_SetTrap(JSContext* cx, JS::HandleScript script, jsbytecode* pc,
           JSTrapHandler handler, JS::HandleValue closure);

extern JS_PUBLIC_API(void)
JS_ClearTrap(JSContext* cx, JSScript* script, jsbytecode* pc,
             JSTrapHandler* handlerp, JS::Value* closurep);

extern JS_PUBLIC_API(void)
JS_ClearScriptTraps(JSRuntime* rt, JSScript* script);

extern JS_PUBLIC_API(void)
JS_ClearAllTrapsForCompartment(JSContext* cx);

extern JS_PUBLIC_API(bool)
JS_SetInterrupt(JSRuntime* rt, JSInterruptHook handler, void* closure);

extern JS_PUBLIC_API(bool)
JS_ClearInterrupt(JSRuntime* rt, JSInterruptHook* handlerp, void** closurep);

/************************************************************************/

extern JS_PUBLIC_API(bool)
JS_SetWatchPoint(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                 JSWatchPointHandler handler, JS::HandleObject closure);

extern JS_PUBLIC_API(bool)
JS_ClearWatchPoint(JSContext* cx, JSObject* obj, jsid id,
                   JSWatchPointHandler* handlerp, JSObject** closurep);

extern JS_PUBLIC_API(bool)
JS_ClearWatchPointsForObject(JSContext* cx, JSObject* obj);

/************************************************************************/

extern JS_PUBLIC_API(jsbytecode*)
JS_LineNumberToPC(JSContext* cx, JSScript* script, unsigned lineno);

extern JS_PUBLIC_API(jsbytecode*)
JS_EndPC(JSContext* cx, JSScript* script);

extern JS_PUBLIC_API(bool)
JS_GetLinePCs(JSContext* cx, JSScript* script,
              unsigned startLine, unsigned maxLines,
              unsigned* count, unsigned** lines, jsbytecode*** pcs);

extern JS_PUBLIC_API(unsigned)
JS_GetFunctionArgumentCount(JSContext* cx, JSFunction* fun);

extern JS_PUBLIC_API(bool)
JS_FunctionHasLocalNames(JSContext* cx, JSFunction* fun);

/*
 * N.B. The mark is in the context temp pool and thus the caller must take care
 * to call JS_ReleaseFunctionLocalNameArray in a LIFO manner (wrt to any other
 * call that may use the temp pool.
 */
extern JS_PUBLIC_API(uintptr_t*)
JS_GetFunctionLocalNameArray(JSContext* cx, JSFunction* fun, void** markp);

extern JS_PUBLIC_API(JSAtom*)
JS_LocalNameToAtom(uintptr_t w);

extern JS_PUBLIC_API(JSString*)
JS_AtomKey(JSAtom* atom);

extern JS_PUBLIC_API(void)
JS_ReleaseFunctionLocalNameArray(JSContext* cx, void* mark);

extern JS_PUBLIC_API(JSScript*)
JS_GetFunctionScript(JSContext* cx, JS::HandleFunction fun);

extern JS_PUBLIC_API(JSNative)
JS_GetFunctionNative(JSContext* cx, JSFunction* fun);

extern JS_PUBLIC_API(JSPrincipals*)
JS_GetScriptPrincipals(JSScript* script);

extern JS_PUBLIC_API(JSPrincipals*)
JS_GetScriptOriginPrincipals(JSScript* script);

JS_PUBLIC_API(JSFunction*)
JS_GetScriptFunction(JSContext* cx, JSScript* script);

extern JS_PUBLIC_API(JSObject*)
JS_GetParentOrScopeChain(JSContext* cx, JSObject* obj);

/************************************************************************/

/*
 * This is almost JS_GetClass(obj)->name except that certain debug-only
 * proxies are made transparent. In particular, this function turns the class
 * of any scope (returned via JS_GetFrameScopeChain or JS_GetFrameCalleeObject)
 * from "Proxy" to "Call", "Block", "With" etc.
 */
extern JS_PUBLIC_API(const char*)
JS_GetDebugClassName(JSObject* obj);

/************************************************************************/

extern JS_PUBLIC_API(const jschar*)
JS_GetScriptSourceMap(JSContext* cx, JSScript* script);

extern JS_PUBLIC_API(unsigned)
JS_GetScriptBaseLineNumber(JSContext* cx, JSScript* script);

extern JS_PUBLIC_API(unsigned)
JS_GetScriptLineExtent(JSContext* cx, JSScript* script);

extern JS_PUBLIC_API(JSVersion)
JS_GetScriptVersion(JSContext* cx, JSScript* script);

extern JS_PUBLIC_API(bool)
JS_GetScriptIsSelfHosted(JSScript* script);

/************************************************************************/

/*
 * Hook setters for script creation and destruction.  These macros provide
 * binary compatibility and newer, shorter synonyms.
 */
#define JS_SetNewScriptHook     JS_SetNewScriptHookProc
#define JS_SetDestroyScriptHook JS_SetDestroyScriptHookProc

extern JS_PUBLIC_API(void)
JS_SetNewScriptHook(JSRuntime* rt, JSNewScriptHook hook, void* callerdata);

extern JS_PUBLIC_API(void)
JS_SetDestroyScriptHook(JSRuntime* rt, JSDestroyScriptHook hook,
                        void* callerdata);

/************************************************************************/

typedef struct JSPropertyDesc {
    JS::Value       id;         /* primary id, atomized string, or int */
    JS::Value       value;      /* property value */
    uint8_t         flags;      /* flags, see below */
    uint8_t         spare;      /* unused */
    JS::Value       alias;      /* alias id if JSPD_ALIAS flag */
} JSPropertyDesc;

#define JSPD_ENUMERATE  0x01    /* visible to for/in loop */
#define JSPD_READONLY   0x02    /* assignment is error */
#define JSPD_PERMANENT  0x04    /* property cannot be deleted */
#define JSPD_ALIAS      0x08    /* property has an alias id */
#define JSPD_EXCEPTION  0x40    /* exception occurred fetching the property, */
                                /* value is exception */
#define JSPD_ERROR      0x80    /* native getter returned false without */
                                /* throwing an exception */

typedef struct JSPropertyDescArray {
    uint32_t        length;     /* number of elements in array */
    JSPropertyDesc* array;     /* alloc'd by Get, freed by Put */
} JSPropertyDescArray;

typedef struct JSScopeProperty JSScopeProperty;

extern JS_PUBLIC_API(bool)
JS_GetPropertyDescArray(JSContext* cx, JS::HandleObject obj, JSPropertyDescArray* pda);

extern JS_PUBLIC_API(void)
JS_PutPropertyDescArray(JSContext* cx, JSPropertyDescArray* pda);

/************************************************************************/

/*
 * JSAbstractFramePtr is the public version of AbstractFramePtr, a pointer to a
 * StackFrame or baseline JIT frame.
 */
class JS_PUBLIC_API(JSAbstractFramePtr)
{
    uintptr_t ptr_;
    jsbytecode* pc_;

  protected:
    JSAbstractFramePtr()
      : ptr_(0), pc_(nullptr)
    { }

  public:
    JSAbstractFramePtr(void* raw, jsbytecode* pc);

    uintptr_t raw() const { return ptr_; }
    jsbytecode* pc() const { return pc_; }

    operator bool() const { return !!ptr_; }

    JSObject* scopeChain(JSContext* cx);
    JSObject* callObject(JSContext* cx);

    JSFunction* maybeFun();
    JSScript* script();

    bool getThisValue(JSContext* cx, JS::MutableHandleValue thisv);

    bool isDebuggerFrame();

    bool evaluateInStackFrame(JSContext* cx,
                              const char* bytes, unsigned length,
                              const char* filename, unsigned lineno,
                              JS::MutableHandleValue rval);

    bool evaluateUCInStackFrame(JSContext* cx,
                                const jschar* chars, unsigned length,
                                const char* filename, unsigned lineno,
                                JS::MutableHandleValue rval);
};

class JS_PUBLIC_API(JSNullFramePtr) : public JSAbstractFramePtr
{
  public:
    JSNullFramePtr()
      : JSAbstractFramePtr()
    {}
};

/*
 * This class does not work when IonMonkey is active. It's only used by jsd,
 * which can only be used when IonMonkey is disabled.
 *
 * To find the calling script and line number, use JS_DescribeSciptedCaller.
 * To summarize the call stack, use JS::DescribeStack.
 */
class JS_PUBLIC_API(JSBrokenFrameIterator)
{
    void* data_;

  public:
    JSBrokenFrameIterator(JSContext* cx);
    ~JSBrokenFrameIterator();

    bool done() const;
    JSBrokenFrameIterator& operator++();

    JSAbstractFramePtr abstractFramePtr() const;
    jsbytecode* pc() const;

    bool isConstructing() const;
};

/*
 * This hook captures high level script execution and function calls (JS or
 * native).  It is used by JS_SetExecuteHook to hook top level scripts and by
 * JS_SetCallHook to hook function calls.  It will get called twice per script
 * or function call: just before execution begins and just after it finishes.
 * In both cases the 'current' frame is that of the executing code.
 *
 * The 'before' param is true for the hook invocation before the execution
 * and false for the invocation after the code has run.
 *
 * The 'ok' param is significant only on the post execution invocation to
 * signify whether or not the code completed 'normally'.
 *
 * The 'closure' param is as passed to JS_SetExecuteHook or JS_SetCallHook
 * for the 'before'invocation, but is whatever value is returned from that
 * invocation for the 'after' invocation. Thus, the hook implementor *could*
 * allocate a structure in the 'before' invocation and return a pointer to that
 * structure. The pointer would then be handed to the hook for the 'after'
 * invocation. Alternately, the 'before' could just return the same value as
 * in 'closure' to cause the 'after' invocation to be called with the same
 * 'closure' value as the 'before'.
 *
 * Returning nullptr in the 'before' hook will cause the 'after' hook *not* to
 * be called.
 */
typedef void*
(* JSInterpreterHook)(JSContext* cx, JSAbstractFramePtr frame, bool isConstructing,
                      bool before, bool* ok, void* closure);

typedef bool
(* JSDebugErrorHook)(JSContext* cx, const char* message, JSErrorReport* report,
                     void* closure);

typedef struct JSDebugHooks {
    JSInterruptHook     interruptHook;
    void*               interruptHookData;
    JSNewScriptHook     newScriptHook;
    void*               newScriptHookData;
    JSDestroyScriptHook destroyScriptHook;
    void*               destroyScriptHookData;
    JSDebuggerHandler   debuggerHandler;
    void*               debuggerHandlerData;
    JSSourceHandler     sourceHandler;
    void*               sourceHandlerData;
    JSInterpreterHook   executeHook;
    void*               executeHookData;
    JSInterpreterHook   callHook;
    void*               callHookData;
    JSThrowHook         throwHook;
    void*               throwHookData;
    JSDebugErrorHook    debugErrorHook;
    void*               debugErrorHookData;
} JSDebugHooks;

/************************************************************************/

extern JS_PUBLIC_API(bool)
JS_SetDebuggerHandler(JSRuntime* rt, JSDebuggerHandler hook, void* closure);

extern JS_PUBLIC_API(bool)
JS_SetSourceHandler(JSRuntime* rt, JSSourceHandler handler, void* closure);

extern JS_PUBLIC_API(bool)
JS_SetExecuteHook(JSRuntime* rt, JSInterpreterHook hook, void* closure);

extern JS_PUBLIC_API(bool)
JS_SetCallHook(JSRuntime* rt, JSInterpreterHook hook, void* closure);

extern JS_PUBLIC_API(bool)
JS_SetThrowHook(JSRuntime* rt, JSThrowHook hook, void* closure);

extern JS_PUBLIC_API(bool)
JS_SetDebugErrorHook(JSRuntime* rt, JSDebugErrorHook hook, void* closure);

/************************************************************************/

extern JS_PUBLIC_API(const JSDebugHooks*)
JS_GetGlobalDebugHooks(JSRuntime* rt);

/**
 * Add various profiling-related functions as properties of the given object.
 */
extern JS_PUBLIC_API(bool)
JS_DefineProfilingFunctions(JSContext* cx, JSObject* obj);

/* Defined in vm/Debugger.cpp. */
extern JS_PUBLIC_API(bool)
JS_DefineDebuggerObject(JSContext* cx, JS::HandleObject obj);

extern JS_PUBLIC_API(void)
JS_DumpPCCounts(JSContext* cx, JS::HandleScript script);

extern JS_PUBLIC_API(void)
JS_DumpCompartmentPCCounts(JSContext* cx);

namespace js {
extern JS_FRIEND_API(bool)
CanCallContextDebugHandler(JSContext* cx);
}

/* Call the context debug handler on the topmost scripted frame. */
extern JS_FRIEND_API(bool)
js_CallContextDebugHandler(JSContext* cx);

#endif /* js_OldDebugAPI_h */
