/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_RootingAPI_h
#define js_RootingAPI_h

#include "mozilla/Attributes.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/LinkedList.h"
#include "mozilla/NullPtr.h"
#include "mozilla/TypeTraits.h"

#include "jspubtd.h"

#include "js/TypeDecls.h"
#include "js/Utility.h"

/*
 * Moving GC Stack Rooting
 *
 * A moving GC may change the physical location of GC allocated things, even
 * when they are rooted, updating all pointers to the thing to refer to its new
 * location. The GC must therefore know about all live pointers to a thing,
 * not just one of them, in order to behave correctly.
 *
 * The |Rooted| and |Handle| classes below are used to root stack locations
 * whose value may be held live across a call that can trigger GC. For a
 * code fragment such as:
 *
 * JSObject* obj = NewObject(cx);
 * DoSomething(cx);
 * ... = obj->lastProperty();
 *
 * If |DoSomething()| can trigger a GC, the stack location of |obj| must be
 * rooted to ensure that the GC does not move the JSObject referred to by
 * |obj| without updating |obj|'s location itself. This rooting must happen
 * regardless of whether there are other roots which ensure that the object
 * itself will not be collected.
 *
 * If |DoSomething()| cannot trigger a GC, and the same holds for all other
 * calls made between |obj|'s definitions and its last uses, then no rooting
 * is required.
 *
 * SpiderMonkey can trigger a GC at almost any time and in ways that are not
 * always clear. For example, the following innocuous-looking actions can
 * cause a GC: allocation of any new GC thing; JSObject::hasProperty;
 * JS_ReportError and friends; and ToNumber, among many others. The following
 * dangerous-looking actions cannot trigger a GC: js_malloc, cx->malloc_,
 * rt->malloc_, and friends and JS_ReportOutOfMemory.
 *
 * The following family of three classes will exactly root a stack location.
 * Incorrect usage of these classes will result in a compile error in almost
 * all cases. Therefore, it is very hard to be incorrectly rooted if you use
 * these classes exclusively. These classes are all templated on the type T of
 * the value being rooted.
 *
 * - Rooted<T> declares a variable of type T, whose value is always rooted.
 *   Rooted<T> may be automatically coerced to a Handle<T>, below. Rooted<T>
 *   should be used whenever a local variable's value may be held live across a
 *   call which can trigger a GC.
 *
 * - Handle<T> is a const reference to a Rooted<T>. Functions which take GC
 *   things or values as arguments and need to root those arguments should
 *   generally use handles for those arguments and avoid any explicit rooting.
 *   This has two benefits. First, when several such functions call each other
 *   then redundant rooting of multiple copies of the GC thing can be avoided.
 *   Second, if the caller does not pass a rooted value a compile error will be
 *   generated, which is quicker and easier to fix than when relying on a
 *   separate rooting analysis.
 *
 * - MutableHandle<T> is a non-const reference to Rooted<T>. It is used in the
 *   same way as Handle<T> and includes a |set(const T& v)| method to allow
 *   updating the value of the referenced Rooted<T>. A MutableHandle<T> can be
 *   created from a Rooted<T> by using |Rooted<T>::operator&()|.
 *
 * In some cases the small performance overhead of exact rooting (measured to
 * be a few nanoseconds on desktop) is too much. In these cases, try the
 * following:
 *
 * - Move all Rooted<T> above inner loops: this allows you to re-use the root
 *   on each iteration of the loop.
 *
 * - Pass Handle<T> through your hot call stack to avoid re-rooting costs at
 *   every invocation.
 *
 * The following diagram explains the list of supported, implicit type
 * conversions between classes of this family:
 *
 *  Rooted<T> ----> Handle<T>
 *     |               ^
 *     |               |
 *     |               |
 *     +---> MutableHandle<T>
 *     (via &)
 *
 * All of these types have an implicit conversion to raw pointers.
 */

namespace js {

class ScriptSourceObject;

template <typename T>
struct GCMethods {};

template <typename T>
class RootedBase {};

template <typename T>
class HandleBase {};

template <typename T>
class MutableHandleBase {};

template <typename T>
class HeapBase {};

/*
 * js::NullPtr acts like a nullptr pointer in contexts that require a Handle.
 *
 * Handle provides an implicit constructor for js::NullPtr so that, given:
 *   foo(Handle<JSObject*> h);
 * callers can simply write:
 *   foo(js::NullPtr());
 * which avoids creating a Rooted<JSObject*> just to pass nullptr.
 *
 * This is the SpiderMonkey internal variant. js::NullPtr should be used in
 * preference to JS::NullPtr to avoid the GOT access required for JS_PUBLIC_API
 * symbols.
 */
struct NullPtr
{
    static void * const constNullValue;
};

namespace gc {
struct Cell;
template<typename T>
struct PersistentRootedMarker;
} /* namespace gc */

} /* namespace js */

namespace JS {

template <typename T> class Rooted;
template <typename T> class PersistentRooted;

/* This is exposing internal state of the GC for inlining purposes. */
JS_FRIEND_API(bool) isGCEnabled();

/*
 * JS::NullPtr acts like a nullptr pointer in contexts that require a Handle.
 *
 * Handle provides an implicit constructor for JS::NullPtr so that, given:
 *   foo(Handle<JSObject*> h);
 * callers can simply write:
 *   foo(JS::NullPtr());
 * which avoids creating a Rooted<JSObject*> just to pass nullptr.
 */
struct JS_PUBLIC_API(NullPtr)
{
    static void * const constNullValue;
};

/*
 * The Heap<T> class is a heap-stored reference to a JS GC thing. All members of
 * heap classes that refer to GC things should use Heap<T> (or possibly
 * TenuredHeap<T>, described below).
 *
 * Heap<T> is an abstraction that hides some of the complexity required to
 * maintain GC invariants for the contained reference. It uses operator
 * overloading to provide a normal pointer interface, but notifies the GC every
 * time the value it contains is updated. This is necessary for generational GC,
 * which keeps track of all pointers into the nursery.
 *
 * Heap<T> instances must be traced when their containing object is traced to
 * keep the pointed-to GC thing alive.
 *
 * Heap<T> objects should only be used on the heap. GC references stored on the
 * C/C++ stack must use Rooted/Handle/MutableHandle instead.
 *
 * Type T must be one of: JS::Value, jsid, JSObject*, JSString*, JSScript*
 */
template <typename T>
class Heap : public js::HeapBase<T>
{
  public:
    Heap() {
        static_assert(sizeof(T) == sizeof(Heap<T>),
                      "Heap<T> must be binary compatible with T.");
        init(js::GCMethods<T>::initial());
    }
    explicit Heap(T p) { init(p); }

    /*
     * For Heap, move semantics are equivalent to copy semantics. In C++, a
     * copy constructor taking const-ref is the way to get a single function
     * that will be used for both lvalue and rvalue copies, so we can simply
     * omit the rvalue variant.
     */
    explicit Heap(const Heap<T>& p) { init(p.ptr); }

    ~Heap() {
        if (js::GCMethods<T>::needsPostBarrier(ptr))
            relocate();
    }

    bool operator==(const Heap<T>& other) { return ptr == other.ptr; }
    bool operator!=(const Heap<T>& other) { return ptr != other.ptr; }

    bool operator==(const T& other) const { return ptr == other; }
    bool operator!=(const T& other) const { return ptr != other; }

    operator T() const { return ptr; }
    T operator->() const { return ptr; }
    const T* address() const { return &ptr; }
    const T& get() const { return ptr; }

    T* unsafeGet() { return &ptr; }

    Heap<T>& operator=(T p) {
        set(p);
        return *this;
    }

    Heap<T>& operator=(const Heap<T>& other) {
        set(other.get());
        return *this;
    }

    void set(T newPtr) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(newPtr));
        if (js::GCMethods<T>::needsPostBarrier(newPtr)) {
            ptr = newPtr;
            post();
        } else if (js::GCMethods<T>::needsPostBarrier(ptr)) {
            relocate();  /* Called before overwriting ptr. */
            ptr = newPtr;
        } else {
            ptr = newPtr;
        }
    }

    /*
     * Set the pointer to a value which will cause a crash if it is
     * dereferenced.
     */
    void setToCrashOnTouch() {
        ptr = reinterpret_cast<T>(crashOnTouchPointer);
    }

    bool isSetToCrashOnTouch() {
        return ptr == crashOnTouchPointer;
    }

  private:
    void init(T newPtr) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(newPtr));
        ptr = newPtr;
        if (js::GCMethods<T>::needsPostBarrier(ptr))
            post();
    }

    void post() {
#ifdef JSGC_GENERATIONAL
        MOZ_ASSERT(js::GCMethods<T>::needsPostBarrier(ptr));
        js::GCMethods<T>::postBarrier(&ptr);
#endif
    }

    void relocate() {
#ifdef JSGC_GENERATIONAL
        js::GCMethods<T>::relocate(&ptr);
#endif
    }

    enum {
        crashOnTouchPointer = 1
    };

    T ptr;
};

#ifdef JS_DEBUG
/*
 * For generational GC, assert that an object is in the tenured generation as
 * opposed to being in the nursery.
 */
extern JS_FRIEND_API(void)
AssertGCThingMustBeTenured(JSObject* obj);
#else
inline void
AssertGCThingMustBeTenured(JSObject* obj) {}
#endif

/*
 * The TenuredHeap<T> class is similar to the Heap<T> class above in that it
 * encapsulates the GC concerns of an on-heap reference to a JS object. However,
 * it has two important differences:
 *
 *  1) Pointers which are statically known to only reference "tenured" objects
 *     can avoid the extra overhead of SpiderMonkey's write barriers.
 *
 *  2) Objects in the "tenured" heap have stronger alignment restrictions than
 *     those in the "nursery", so it is possible to store flags in the lower
 *     bits of pointers known to be tenured. TenuredHeap wraps a normal tagged
 *     pointer with a nice API for accessing the flag bits and adds various
 *     assertions to ensure that it is not mis-used.
 *
 * GC things are said to be "tenured" when they are located in the long-lived
 * heap: e.g. they have gained tenure as an object by surviving past at least
 * one GC. For performance, SpiderMonkey allocates some things which are known
 * to normally be long lived directly into the tenured generation; for example,
 * global objects. Additionally, SpiderMonkey does not visit individual objects
 * when deleting non-tenured objects, so object with finalizers are also always
 * tenured; for instance, this includes most DOM objects.
 *
 * The considerations to keep in mind when using a TenuredHeap<T> vs a normal
 * Heap<T> are:
 *
 *  - It is invalid for a TenuredHeap<T> to refer to a non-tenured thing.
 *  - It is however valid for a Heap<T> to refer to a tenured thing.
 *  - It is not possible to store flag bits in a Heap<T>.
 */
template <typename T>
class TenuredHeap : public js::HeapBase<T>
{
  public:
    TenuredHeap() : bits(0) {
        static_assert(sizeof(T) == sizeof(TenuredHeap<T>),
                      "TenuredHeap<T> must be binary compatible with T.");
    }
    explicit TenuredHeap(T p) : bits(0) { setPtr(p); }
    explicit TenuredHeap(const TenuredHeap<T>& p) : bits(0) { setPtr(p.getPtr()); }

    bool operator==(const TenuredHeap<T>& other) { return bits == other.bits; }
    bool operator!=(const TenuredHeap<T>& other) { return bits != other.bits; }

    void setPtr(T newPtr) {
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(newPtr) & flagsMask) == 0);
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(newPtr));
        if (newPtr)
            AssertGCThingMustBeTenured(newPtr);
        bits = (bits & flagsMask) | reinterpret_cast<uintptr_t>(newPtr);
    }

    void setFlags(uintptr_t flagsToSet) {
        MOZ_ASSERT((flagsToSet & ~flagsMask) == 0);
        bits |= flagsToSet;
    }

    void unsetFlags(uintptr_t flagsToUnset) {
        MOZ_ASSERT((flagsToUnset & ~flagsMask) == 0);
        bits &= ~flagsToUnset;
    }

    bool hasFlag(uintptr_t flag) const {
        MOZ_ASSERT((flag & ~flagsMask) == 0);
        return (bits & flag) != 0;
    }

    T getPtr() const { return reinterpret_cast<T>(bits & ~flagsMask); }
    uintptr_t getFlags() const { return bits & flagsMask; }

    operator T() const { return getPtr(); }
    T operator->() const { return getPtr(); }

    TenuredHeap<T>& operator=(T p) {
        setPtr(p);
        return *this;
    }

    TenuredHeap<T>& operator=(const TenuredHeap<T>& other) {
        bits = other.bits;
        return *this;
    }

  private:
    enum {
        maskBits = 3,
        flagsMask = (1 << maskBits) - 1,
    };

    uintptr_t bits;
};

/*
 * Reference to a T that has been rooted elsewhere. This is most useful
 * as a parameter type, which guarantees that the T lvalue is properly
 * rooted. See "Move GC Stack Rooting" above.
 *
 * If you want to add additional methods to Handle for a specific
 * specialization, define a HandleBase<T> specialization containing them.
 */
template <typename T>
class MOZ_NONHEAP_CLASS Handle : public js::HandleBase<T>
{
    friend class JS::MutableHandle<T>;

  public:
    /* Creates a handle from a handle of a type convertible to T. */
    template <typename S>
    Handle(Handle<S> handle,
           typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0)
    {
        static_assert(sizeof(Handle<T>) == sizeof(T*),
                      "Handle must be binary compatible with T*.");
        ptr = reinterpret_cast<const T*>(handle.address());
    }

    /* Create a handle for a nullptr pointer. */
    Handle(js::NullPtr) {
        static_assert(mozilla::IsPointer<T>::value,
                      "js::NullPtr overload not valid for non-pointer types");
        ptr = reinterpret_cast<const T*>(&js::NullPtr::constNullValue);
    }

    /* Create a handle for a nullptr pointer. */
    Handle(JS::NullPtr) {
        static_assert(mozilla::IsPointer<T>::value,
                      "JS::NullPtr overload not valid for non-pointer types");
        ptr = reinterpret_cast<const T*>(&JS::NullPtr::constNullValue);
    }

    Handle(MutableHandle<T> handle) {
        ptr = handle.address();
    }

    /*
     * Take care when calling this method!
     *
     * This creates a Handle from the raw location of a T.
     *
     * It should be called only if the following conditions hold:
     *
     *  1) the location of the T is guaranteed to be marked (for some reason
     *     other than being a Rooted), e.g., if it is guaranteed to be reachable
     *     from an implicit root.
     *
     *  2) the contents of the location are immutable, or at least cannot change
     *     for the lifetime of the handle, as its users may not expect its value
     *     to change underneath them.
     */
    static MOZ_CONSTEXPR Handle fromMarkedLocation(const T* p) {
        return Handle(p, DeliberatelyChoosingThisOverload,
                      ImUsingThisOnlyInFromFromMarkedLocation);
    }

    /*
     * Construct a handle from an explicitly rooted location. This is the
     * normal way to create a handle, and normally happens implicitly.
     */
    template <typename S>
    inline
    Handle(const Rooted<S>& root,
           typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0);

    template <typename S>
    inline
    Handle(const PersistentRooted<S>& root,
           typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0);

    /* Construct a read only handle from a mutable handle. */
    template <typename S>
    inline
    Handle(MutableHandle<S>& root,
           typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0);

    const T* address() const { return ptr; }
    const T& get() const { return *ptr; }

    /*
     * Return a reference so passing a Handle<T> to something that
     * takes a |const T&| is not a GC hazard.
     */
    operator const T&() const { return get(); }
    T operator->() const { return get(); }

    bool operator!=(const T& other) const { return *ptr != other; }
    bool operator==(const T& other) const { return *ptr == other; }

    /* Change this handle to point to the same rooted location RHS does. */
    void repoint(const Handle& rhs) { ptr = rhs.address(); }

  private:
    Handle() {}

    enum Disambiguator { DeliberatelyChoosingThisOverload = 42 };
    enum CallerIdentity { ImUsingThisOnlyInFromFromMarkedLocation = 17 };
    MOZ_CONSTEXPR Handle(const T* p, Disambiguator, CallerIdentity) : ptr(p) {}

    const T* ptr;

    template <typename S> void operator=(S) MOZ_DELETE;
    void operator=(Handle) MOZ_DELETE;
};

/*
 * Similar to a handle, but the underlying storage can be changed. This is
 * useful for outparams.
 *
 * If you want to add additional methods to MutableHandle for a specific
 * specialization, define a MutableHandleBase<T> specialization containing
 * them.
 */
template <typename T>
class MOZ_STACK_CLASS MutableHandle : public js::MutableHandleBase<T>
{
  public:
    inline MutableHandle(Rooted<T>* root);
    inline MutableHandle(PersistentRooted<T>* root);

  private:
    // Disallow true nullptr and emulated nullptr (gcc 4.4/4.5, __null, appears
    // as int/long [32/64-bit]) for overloading purposes.
    template<typename N>
    MutableHandle(N,
                  typename mozilla::EnableIf<mozilla::IsNullPointer<N>::value ||
                                             mozilla::IsSame<N, int>::value ||
                                             mozilla::IsSame<N, long>::value,
                                             int>::Type dummy = 0)
    MOZ_DELETE;

  public:
    void set(T v) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(v));
        *ptr = v;
    }

    /*
     * This may be called only if the location of the T is guaranteed
     * to be marked (for some reason other than being a Rooted),
     * e.g., if it is guaranteed to be reachable from an implicit root.
     *
     * Create a MutableHandle from a raw location of a T.
     */
    static MutableHandle fromMarkedLocation(T* p) {
        MutableHandle h;
        h.ptr = p;
        return h;
    }

    T* address() const { return ptr; }
    const T& get() const { return *ptr; }

    /*
     * Return a reference so passing a MutableHandle<T> to something that takes
     * a |const T&| is not a GC hazard.
     */
    operator const T&() const { return get(); }
    T operator->() const { return get(); }

  private:
    MutableHandle() {}

    T* ptr;

    template <typename S> void operator=(S v) MOZ_DELETE;
    void operator=(MutableHandle other) MOZ_DELETE;
};

#ifdef JSGC_GENERATIONAL
JS_FRIEND_API(void) HeapCellPostBarrier(js::gc::Cell** cellp);
JS_FRIEND_API(void) HeapCellRelocate(js::gc::Cell** cellp);
#endif

} /* namespace JS */

namespace js {

/*
 * InternalHandle is a handle to an internal pointer into a gcthing. Use
 * InternalHandle when you have a pointer to a direct field of a gcthing, or
 * when you need a parameter type for something that *may* be a pointer to a
 * direct field of a gcthing.
 */
template <typename T>
class InternalHandle {};

template <typename T>
class InternalHandle<T*>
{
    void * const* holder;
    size_t offset;

  public:
    /*
     * Create an InternalHandle using a Handle to the gcthing containing the
     * field in question, and a pointer to the field.
     */
    template<typename H>
    InternalHandle(const JS::Handle<H>& handle, T* field)
      : holder((void**)handle.address()), offset(uintptr_t(field) - uintptr_t(handle.get()))
    {}

    /*
     * Create an InternalHandle to a field within a Rooted<>.
     */
    template<typename R>
    InternalHandle(const JS::Rooted<R>& root, T* field)
      : holder((void**)root.address()), offset(uintptr_t(field) - uintptr_t(root.get()))
    {}

    InternalHandle(const InternalHandle<T*>& other)
      : holder(other.holder), offset(other.offset) {}

    T* get() const { return reinterpret_cast<T*>(uintptr_t(*holder) + offset); }

    const T& operator*() const { return *get(); }
    T* operator->() const { return get(); }

    static InternalHandle<T*> fromMarkedLocation(T* fieldPtr) {
        return InternalHandle(fieldPtr);
    }

  private:
    /*
     * Create an InternalHandle to something that is not a pointer to a
     * gcthing, and so does not need to be rooted in the first place. Use these
     * InternalHandles to pass pointers into functions that also need to accept
     * regular InternalHandles to gcthing fields.
     *
     * Make this private to prevent accidental misuse; this is only for
     * fromMarkedLocation().
     */
    InternalHandle(T* field)
      : holder(reinterpret_cast<void * const*>(&js::NullPtr::constNullValue)),
        offset(uintptr_t(field))
    {}

    void operator=(InternalHandle<T*> other) MOZ_DELETE;
};

/*
 * By default, pointers should use the inheritance hierarchy to find their
 * ThingRootKind. Some pointer types are explicitly set in jspubtd.h so that
 * Rooted<T> may be used without the class definition being available.
 */
template <typename T>
struct RootKind<T*>
{
    static ThingRootKind rootKind() { return T::rootKind(); }
};

template <typename T>
struct GCMethods<T*>
{
    static T* initial() { return nullptr; }
    static ThingRootKind kind() { return RootKind<T*>::rootKind(); }
    static bool poisoned(T* v) { return JS::IsPoisonedPtr(v); }
    static bool needsPostBarrier(T* v) { return false; }
#ifdef JSGC_GENERATIONAL
    static void postBarrier(T** vp) {}
    static void relocate(T** vp) {}
#endif
};

template <>
struct GCMethods<JSObject*>
{
    static JSObject* initial() { return nullptr; }
    static ThingRootKind kind() { return RootKind<JSObject*>::rootKind(); }
    static bool poisoned(JSObject* v) { return JS::IsPoisonedPtr(v); }
    static bool needsPostBarrier(JSObject* v) { return v; }
#ifdef JSGC_GENERATIONAL
    static void postBarrier(JSObject** vp) {
        JS::HeapCellPostBarrier(reinterpret_cast<js::gc::Cell**>(vp));
    }
    static void relocate(JSObject** vp) {
        JS::HeapCellRelocate(reinterpret_cast<js::gc::Cell**>(vp));
    }
#endif
};

template <>
struct GCMethods<JSFunction*>
{
    static JSFunction* initial() { return nullptr; }
    static ThingRootKind kind() { return RootKind<JSObject*>::rootKind(); }
    static bool poisoned(JSFunction* v) { return JS::IsPoisonedPtr(v); }
    static bool needsPostBarrier(JSFunction* v) { return v; }
#ifdef JSGC_GENERATIONAL
    static void postBarrier(JSFunction** vp) {
        JS::HeapCellPostBarrier(reinterpret_cast<js::gc::Cell**>(vp));
    }
    static void relocate(JSFunction** vp) {
        JS::HeapCellRelocate(reinterpret_cast<js::gc::Cell**>(vp));
    }
#endif
};

#ifdef JS_DEBUG
/* This helper allows us to assert that Rooted<T> is scoped within a request. */
extern JS_PUBLIC_API(bool)
IsInRequest(JSContext* cx);
#endif

} /* namespace js */

namespace JS {

/*
 * Local variable of type T whose value is always rooted. This is typically
 * used for local variables, or for non-rooted values being passed to a
 * function that requires a handle, e.g. Foo(Root<T>(cx, x)).
 *
 * If you want to add additional methods to Rooted for a specific
 * specialization, define a RootedBase<T> specialization containing them.
 */
template <typename T>
class MOZ_STACK_CLASS Rooted : public js::RootedBase<T>
{
    /* Note: CX is a subclass of either ContextFriendFields or PerThreadDataFriendFields. */
    template <typename CX>
    void init(CX* cx) {
#ifdef JSGC_TRACK_EXACT_ROOTS
        js::ThingRootKind kind = js::GCMethods<T>::kind();
        this->stack = &cx->thingGCRooters[kind];
        this->prev = *stack;
        *stack = reinterpret_cast<Rooted<void*>*>(this);

        MOZ_ASSERT(!js::GCMethods<T>::poisoned(ptr));
#endif
    }

  public:
    Rooted(JSContext* cx
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(js::GCMethods<T>::initial())
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
#ifdef JS_DEBUG
        MOZ_ASSERT(js::IsInRequest(cx));
#endif
        init(js::ContextFriendFields::get(cx));
    }

    Rooted(JSContext* cx, T initial
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(initial)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
#ifdef JS_DEBUG
        MOZ_ASSERT(js::IsInRequest(cx));
#endif
        init(js::ContextFriendFields::get(cx));
    }

    Rooted(js::ContextFriendFields* cx
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(js::GCMethods<T>::initial())
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(cx);
    }

    Rooted(js::ContextFriendFields* cx, T initial
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(initial)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(cx);
    }

    Rooted(js::PerThreadDataFriendFields* pt
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(js::GCMethods<T>::initial())
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(pt);
    }

    Rooted(js::PerThreadDataFriendFields* pt, T initial
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(initial)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(pt);
    }

    Rooted(JSRuntime* rt
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(js::GCMethods<T>::initial())
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(js::PerThreadDataFriendFields::getMainThread(rt));
    }

    Rooted(JSRuntime* rt, T initial
           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(initial)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(js::PerThreadDataFriendFields::getMainThread(rt));
    }

    // Note that we need to let the compiler generate the default destructor in
    // non-exact-rooting builds because of a bug in the instrumented PGO builds
    // using MSVC, see bug 915735 for more details.
#ifdef JSGC_TRACK_EXACT_ROOTS
    ~Rooted() {
        MOZ_ASSERT(*stack == reinterpret_cast<Rooted<void*>*>(this));
        *stack = prev;
    }
#endif

#ifdef JSGC_TRACK_EXACT_ROOTS
    Rooted<T>* previous() { return prev; }
#endif

    /*
     * Important: Return a reference here so passing a Rooted<T> to
     * something that takes a |const T&| is not a GC hazard.
     */
    operator const T&() const { return ptr; }
    T operator->() const { return ptr; }
    T* address() { return &ptr; }
    const T* address() const { return &ptr; }
    T& get() { return ptr; }
    const T& get() const { return ptr; }

    T& operator=(T value) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(value));
        ptr = value;
        return ptr;
    }

    T& operator=(const Rooted& value) {
        ptr = value;
        return ptr;
    }

    void set(T value) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(value));
        ptr = value;
    }

    bool operator!=(const T& other) const { return ptr != other; }
    bool operator==(const T& other) const { return ptr == other; }

  private:
#ifdef JSGC_TRACK_EXACT_ROOTS
    Rooted<void*>** stack, *prev;
#endif

    /*
     * |ptr| must be the last field in Rooted because the analysis treats all
     * Rooted as Rooted<void*> during the analysis. See bug 829372.
     */
    T ptr;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    Rooted(const Rooted&) MOZ_DELETE;
};

} /* namespace JS */

namespace js {

/*
 * Augment the generic Rooted<T> interface when T = JSObject* with
 * class-querying and downcasting operations.
 *
 * Given a Rooted<JSObject*> obj, one can view
 *   Handle<StringObject*> h = obj.as<StringObject*>();
 * as an optimization of
 *   Rooted<StringObject*> rooted(cx, &obj->as<StringObject*>());
 *   Handle<StringObject*> h = rooted;
 */
template <>
class RootedBase<JSObject*>
{
  public:
    template <class U>
    JS::Handle<U*> as() const;
};


/*
 * RootedGeneric<T> allows a class to instantiate its own Rooted type by
 * including the following two methods:
 *
 *    static inline js::ThingRootKind rootKind() { return js::THING_ROOT_CUSTOM; }
 *    void trace(JSTracer* trc);
 *
 * The trace() method must trace all of the class's fields.
 *
 * Implementation:
 *
 * RootedGeneric<T> works by placing a pointer to its 'rooter' field into the
 * usual list of rooters when it is instantiated. When marking, it backs up
 * from this pointer to find a vtable containing a type-appropriate trace()
 * method.
 */
template <typename GCType>
class JS_PUBLIC_API(RootedGeneric)
{
  public:
    JS::Rooted<GCType> rooter;

    RootedGeneric(js::ContextFriendFields* cx)
        : rooter(cx)
    {
    }

    RootedGeneric(js::ContextFriendFields* cx, const GCType& initial)
        : rooter(cx, initial)
    {
    }

    virtual inline void trace(JSTracer* trc);

    operator const GCType&() const { return rooter.get(); }
    GCType operator->() const { return rooter.get(); }
};

template <typename GCType>
inline void RootedGeneric<GCType>::trace(JSTracer* trc)
{
    rooter->trace(trc);
}

// We will instantiate RootedGeneric<void*> in RootMarking.cpp, and MSVC will
// notice that void*s have no trace() method defined on them and complain (even
// though it's never called.) MSVC's complaint is not unreasonable, so
// specialize for void*.
template <>
inline void RootedGeneric<void*>::trace(JSTracer* trc)
{
    MOZ_ASSUME_UNREACHABLE("RootedGeneric<void*>::trace()");
}

/* Interface substitute for Rooted<T> which does not root the variable's memory. */
template <typename T>
class FakeRooted : public RootedBase<T>
{
  public:
    template <typename CX>
    FakeRooted(CX* cx
               MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(GCMethods<T>::initial())
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    template <typename CX>
    FakeRooted(CX* cx, T initial
               MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : ptr(initial)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    operator T() const { return ptr; }
    T operator->() const { return ptr; }
    T* address() { return &ptr; }
    const T* address() const { return &ptr; }
    T& get() { return ptr; }
    const T& get() const { return ptr; }

    FakeRooted<T>& operator=(T value) {
        MOZ_ASSERT(!GCMethods<T>::poisoned(value));
        ptr = value;
        return *this;
    }

    FakeRooted<T>& operator=(const FakeRooted<T>& other) {
        MOZ_ASSERT(!GCMethods<T>::poisoned(other.ptr));
        ptr = other.ptr;
        return *this;
    }

    bool operator!=(const T& other) const { return ptr != other; }
    bool operator==(const T& other) const { return ptr == other; }

  private:
    T ptr;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    FakeRooted(const FakeRooted&) MOZ_DELETE;
};

/* Interface substitute for MutableHandle<T> which is not required to point to rooted memory. */
template <typename T>
class FakeMutableHandle : public js::MutableHandleBase<T>
{
  public:
    FakeMutableHandle(T* t) {
        ptr = t;
    }

    FakeMutableHandle(FakeRooted<T>* root) {
        ptr = root->address();
    }

    void set(T v) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(v));
        *ptr = v;
    }

    T* address() const { return ptr; }
    T get() const { return *ptr; }

    operator T() const { return get(); }
    T operator->() const { return get(); }

  private:
    FakeMutableHandle() {}

    T* ptr;

    template <typename S>
    void operator=(S v) MOZ_DELETE;

    void operator=(const FakeMutableHandle<T>& other) MOZ_DELETE;
};

/*
 * Types for a variable that either should or shouldn't be rooted, depending on
 * the template parameter allowGC. Used for implementing functions that can
 * operate on either rooted or unrooted data.
 *
 * The toHandle() and toMutableHandle() functions are for calling functions
 * which require handle types and are only called in the CanGC case. These
 * allow the calling code to type check.
 */
enum AllowGC {
    NoGC = 0,
    CanGC = 1
};
template <typename T, AllowGC allowGC>
class MaybeRooted
{
};

template <typename T> class MaybeRooted<T, CanGC>
{
  public:
    typedef JS::Handle<T> HandleType;
    typedef JS::Rooted<T> RootType;
    typedef JS::MutableHandle<T> MutableHandleType;

    static inline JS::Handle<T> toHandle(HandleType v) {
        return v;
    }

    static inline JS::MutableHandle<T> toMutableHandle(MutableHandleType v) {
        return v;
    }
};

template <typename T> class MaybeRooted<T, NoGC>
{
  public:
    typedef T HandleType;
    typedef FakeRooted<T> RootType;
    typedef FakeMutableHandle<T> MutableHandleType;

    static inline JS::Handle<T> toHandle(HandleType v) {
        MOZ_ASSUME_UNREACHABLE("Bad conversion");
    }

    static inline JS::MutableHandle<T> toMutableHandle(MutableHandleType v) {
        MOZ_ASSUME_UNREACHABLE("Bad conversion");
    }
};

} /* namespace js */

namespace JS {

template <typename T> template <typename S>
inline
Handle<T>::Handle(const Rooted<S>& root,
                  typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy)
{
    ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T> template <typename S>
inline
Handle<T>::Handle(const PersistentRooted<S>& root,
                  typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy)
{
    ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T> template <typename S>
inline
Handle<T>::Handle(MutableHandle<S>& root,
                  typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy)
{
    ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T>
inline
MutableHandle<T>::MutableHandle(Rooted<T>* root)
{
    static_assert(sizeof(MutableHandle<T>) == sizeof(T*),
                  "MutableHandle must be binary compatible with T*.");
    ptr = root->address();
}

template <typename T>
inline
MutableHandle<T>::MutableHandle(PersistentRooted<T>* root)
{
    static_assert(sizeof(MutableHandle<T>) == sizeof(T*),
                  "MutableHandle must be binary compatible with T*.");
    ptr = root->address();
}

/*
 * A copyable, assignable global GC root type with arbitrary lifetime, an
 * infallible constructor, and automatic unrooting on destruction.
 *
 * These roots can be used in heap-allocated data structures, so they are not
 * associated with any particular JSContext or stack. They are registered with
 * the JSRuntime itself, without locking, so they require a full JSContext to be
 * constructed, not one of its more restricted superclasses.
 *
 * Note that you must not use an PersistentRooted in an object owned by a JS
 * object:
 *
 * Whenever one object whose lifetime is decided by the GC refers to another
 * such object, that edge must be traced only if the owning JS object is traced.
 * This applies not only to JS objects (which obviously are managed by the GC)
 * but also to C++ objects owned by JS objects.
 *
 * If you put a PersistentRooted in such a C++ object, that is almost certainly
 * a leak. When a GC begins, the referent of the PersistentRooted is treated as
 * live, unconditionally (because a PersistentRooted is a *root*), even if the
 * JS object that owns it is unreachable. If there is any path from that
 * referent back to the JS object, then the C++ object containing the
 * PersistentRooted will not be destructed, and the whole blob of objects will
 * not be freed, even if there are no references to them from the outside.
 *
 * In the context of Firefox, this is a severe restriction: almost everything in
 * Firefox is owned by some JS object or another, so using PersistentRooted in
 * such objects would introduce leaks. For these kinds of edges, Heap<T> or
 * TenuredHeap<T> would be better types. It's up to the implementor of the type
 * containing Heap<T> or TenuredHeap<T> members to make sure their referents get
 * marked when the object itself is marked.
 */
template<typename T>
class PersistentRooted : private mozilla::LinkedListElement<PersistentRooted<T> > {
    friend class mozilla::LinkedList<PersistentRooted>;
    friend class mozilla::LinkedListElement<PersistentRooted>;

    friend class js::gc::PersistentRootedMarker<T>;

    void registerWithRuntime(JSRuntime* rt) {
        JS::shadow::Runtime* srt = JS::shadow::Runtime::asShadowRuntime(rt);
        srt->getPersistentRootedList<T>().insertBack(this);
    }

  public:
    PersistentRooted(JSContext* cx) : ptr(js::GCMethods<T>::initial())
    {
        registerWithRuntime(js::GetRuntime(cx));
    }

    PersistentRooted(JSContext* cx, T initial) : ptr(initial)
    {
        registerWithRuntime(js::GetRuntime(cx));
    }

    PersistentRooted(JSRuntime* rt) : ptr(js::GCMethods<T>::initial())
    {
        registerWithRuntime(rt);
    }

    PersistentRooted(JSRuntime* rt, T initial) : ptr(initial)
    {
        registerWithRuntime(rt);
    }

    PersistentRooted(PersistentRooted& rhs) : ptr(rhs.ptr)
    {
        /*
         * Copy construction takes advantage of the fact that the original
         * is already inserted, and simply adds itself to whatever list the
         * original was on - no JSRuntime pointer needed.
         */
        rhs.setNext(this);
    }

    /*
     * Important: Return a reference here so passing a Rooted<T> to
     * something that takes a |const T&| is not a GC hazard.
     */
    operator const T&() const { return ptr; }
    T operator->() const { return ptr; }
    T* address() { return &ptr; }
    const T* address() const { return &ptr; }
    T& get() { return ptr; }
    const T& get() const { return ptr; }

    T& operator=(T value) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(value));
        ptr = value;
        return ptr;
    }

    T& operator=(const PersistentRooted& value) {
        ptr = value;
        return ptr;
    }

    void set(T value) {
        MOZ_ASSERT(!js::GCMethods<T>::poisoned(value));
        ptr = value;
    }

    bool operator!=(const T& other) const { return ptr != other; }
    bool operator==(const T& other) const { return ptr == other; }

  private:
    T ptr;
};

} /* namespace JS */

namespace js {

/* Base class for automatic read-only object rooting during compilation. */
class CompilerRootNode
{
  protected:
    CompilerRootNode(js::gc::Cell* ptr) : next(nullptr), ptr_(ptr) {}

  public:
    void** address() { return (void**)&ptr_; }

  public:
    CompilerRootNode* next;

  protected:
    js::gc::Cell* ptr_;
};

}  /* namespace js */

#endif  /* js_RootingAPI_h */
