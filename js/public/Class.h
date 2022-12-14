/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JSClass definition and its component types, plus related interfaces. */

#ifndef js_Class_h
#define js_Class_h

#include "mozilla/NullPtr.h"
 
#include "jstypes.h"

#include "js/CallArgs.h"
#include "js/Id.h"
#include "js/TypeDecls.h"

/*
 * A JSClass acts as a vtable for JS objects that allows JSAPI clients to
 * control various aspects of the behavior of an object like property lookup.
 * js::Class is an engine-private extension that allows more control over
 * object behavior and, e.g., allows custom slow layout.
 */

class JSFreeOp;
struct JSFunctionSpec;

namespace js {

class Class;
class FreeOp;
class PropertyName;
class Shape;

// This is equal to JSFunction::class_.  Use it in places where you don't want
// to #include jsfun.h.
extern JS_FRIEND_DATA(const js::Class* const) FunctionClassPtr;

} // namespace js

// JSClass operation signatures.

// Add or get a property named by id in obj.  Note the jsid id type -- id may
// be a string (Unicode property identifier) or an int (element index).  The
// *vp out parameter, on success, is the new property value after the action.
typedef bool
(* JSPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                 JS::MutableHandleValue vp);

// Set a property named by id in obj, treating the assignment as strict
// mode code if strict is true. Note the jsid id type -- id may be a string
// (Unicode property identifier) or an int (element index). The *vp out
// parameter, on success, is the new property value after the
// set.
typedef bool
(* JSStrictPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                       bool strict, JS::MutableHandleValue vp);

// Delete a property named by id in obj.
//
// If an error occurred, return false as per normal JSAPI error practice.
//
// If no error occurred, but the deletion attempt wasn't allowed (perhaps
// because the property was non-configurable), set *succeeded to false and
// return true.  This will cause |delete obj[id]| to evaluate to false in
// non-strict mode code, and to throw a TypeError in strict mode code.
//
// If no error occurred and the deletion wasn't disallowed (this is *not* the
// same as saying that a deletion actually occurred -- deleting a non-existent
// property, or an inherited property, is allowed -- it's just pointless),
// set *succeeded to true and return true.
typedef bool
(* JSDeletePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                       bool* succeeded);

// This function type is used for callbacks that enumerate the properties of
// a JSObject.  The behavior depends on the value of enum_op:
//
//  JSENUMERATE_INIT
//    A new, opaque iterator state should be allocated and stored in *statep.
//    (You can use PRIVATE_TO_JSVAL() to tag the pointer to be stored).
//
//    The number of properties that will be enumerated should be returned as
//    an integer jsval in *idp, if idp is non-null, and provided the number of
//    enumerable properties is known.  If idp is non-null and the number of
//    enumerable properties can't be computed in advance, *idp should be set
//    to JSVAL_ZERO.
//
//  JSENUMERATE_INIT_ALL
//    Used identically to JSENUMERATE_INIT, but exposes all properties of the
//    object regardless of enumerability.
//
//  JSENUMERATE_NEXT
//    A previously allocated opaque iterator state is passed in via statep.
//    Return the next jsid in the iteration using *idp.  The opaque iterator
//    state pointed at by statep is destroyed and *statep is set to JSVAL_NULL
//    if there are no properties left to enumerate.
//
//  JSENUMERATE_DESTROY
//    Destroy the opaque iterator state previously allocated in *statep by a
//    call to this function when enum_op was JSENUMERATE_INIT or
//    JSENUMERATE_INIT_ALL.
//
// The return value is used to indicate success, with a value of false
// indicating failure.
typedef bool
(* JSNewEnumerateOp)(JSContext* cx, JS::HandleObject obj, JSIterateOp enum_op,
                     JS::MutableHandleValue statep, JS::MutableHandleId idp);

// The old-style JSClass.enumerate op should define all lazy properties not
// yet reflected in obj.
typedef bool
(* JSEnumerateOp)(JSContext* cx, JS::HandleObject obj);

// Resolve a lazy property named by id in obj by defining it directly in obj.
// Lazy properties are those reflected from some peer native property space
// (e.g., the DOM attributes for a given node reflected as obj) on demand.
//
// JS looks for a property in an object, and if not found, tries to resolve
// the given id.  If resolve succeeds, the engine looks again in case resolve
// defined obj[id].  If no such property exists directly in obj, the process
// is repeated with obj's prototype, etc.
//
// NB: JSNewResolveOp provides a cheaper way to resolve lazy properties.
typedef bool
(* JSResolveOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id);

// Like JSResolveOp, except the *objp out parameter, on success, should be null
// to indicate that id was not resolved; and non-null, referring to obj or one
// of its prototypes, if id was resolved.  The hook may assume* objp is null on
// entry.
//
// This hook instead of JSResolveOp is called via the JSClass.resolve member
// if JSCLASS_NEW_RESOLVE is set in JSClass.flags.
typedef bool
(* JSNewResolveOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                   JS::MutableHandleObject objp);

// Convert obj to the given type, returning true with the resulting value in
// *vp on success, and returning false on error or exception.
typedef bool
(* JSConvertOp)(JSContext* cx, JS::HandleObject obj, JSType type,
                JS::MutableHandleValue vp);

// Finalize obj, which the garbage collector has determined to be unreachable
// from other live objects or from GC roots.  Obviously, finalizers must never
// store a reference to obj.
typedef void
(* JSFinalizeOp)(JSFreeOp* fop, JSObject* obj);

// Finalizes external strings created by JS_NewExternalString.
struct JSStringFinalizer {
    void (*finalize)(const JSStringFinalizer* fin, jschar* chars);
};

// Check whether v is an instance of obj.  Return false on error or exception,
// true on success with true in *bp if v is an instance of obj, false in
// *bp otherwise.
typedef bool
(* JSHasInstanceOp)(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue vp,
                    bool* bp);

// Function type for trace operation of the class called to enumerate all
// traceable things reachable from obj's private data structure. For each such
// thing, a trace implementation must call one of the JS_Call*Tracer variants
// on the thing.
//
// JSTraceOp implementation can assume that no other threads mutates object
// state. It must not change state of the object or corresponding native
// structures. The only exception for this rule is the case when the embedding
// needs a tight integration with GC. In that case the embedding can check if
// the traversal is a part of the marking phase through calling
// JS_IsGCMarkingTracer and apply a special code like emptying caches or
// marking its native structures.
typedef void
(* JSTraceOp)(JSTracer* trc, JSObject* obj);

// A generic type for functions mapping an object to another object, or null
// if an error or exception was thrown on cx.
typedef JSObject*
(* JSObjectOp)(JSContext* cx, JS::HandleObject obj);

// Hook that creates an iterator object for a given object. Returns the
// iterator object or null if an error or exception was thrown on cx.
typedef JSObject*
(* JSIteratorOp)(JSContext* cx, JS::HandleObject obj, bool keysonly);

typedef JSObject*
(* JSWeakmapKeyDelegateOp)(JSObject* obj);

/* js::Class operation signatures. */

namespace js {

typedef bool
(* LookupGenericOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                    JS::MutableHandleObject objp, JS::MutableHandle<Shape*> propp);
typedef bool
(* LookupPropOp)(JSContext* cx, JS::HandleObject obj, JS::Handle<PropertyName*> name,
                 JS::MutableHandleObject objp, JS::MutableHandle<Shape*> propp);
typedef bool
(* LookupElementOp)(JSContext* cx, JS::HandleObject obj, uint32_t index,
                    JS::MutableHandleObject objp, JS::MutableHandle<Shape*> propp);
typedef bool
(* DefineGenericOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue value,
                    JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);
typedef bool
(* DefinePropOp)(JSContext* cx, JS::HandleObject obj, JS::Handle<PropertyName*> name,
                 JS::HandleValue value, JSPropertyOp getter, JSStrictPropertyOp setter,
                 unsigned attrs);
typedef bool
(* DefineElementOp)(JSContext* cx, JS::HandleObject obj, uint32_t index, JS::HandleValue value,
                    JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);
typedef bool
(* GenericIdOp)(JSContext* cx, JS::HandleObject obj, JS::HandleObject receiver, JS::HandleId id,
                JS::MutableHandleValue vp);
typedef bool
(* PropertyIdOp)(JSContext* cx, JS::HandleObject obj, JS::HandleObject receiver,
                 JS::Handle<PropertyName*> name, JS::MutableHandleValue vp);
typedef bool
(* ElementIdOp)(JSContext* cx, JS::HandleObject obj, JS::HandleObject receiver, uint32_t index,
                JS::MutableHandleValue vp);
typedef bool
(* StrictGenericIdOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                      JS::MutableHandleValue vp, bool strict);
typedef bool
(* StrictPropertyIdOp)(JSContext* cx, JS::HandleObject obj, JS::Handle<PropertyName*> name,
                       JS::MutableHandleValue vp, bool strict);
typedef bool
(* StrictElementIdOp)(JSContext* cx, JS::HandleObject obj, uint32_t index,
                      JS::MutableHandleValue vp, bool strict);
typedef bool
(* GenericAttributesOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, unsigned* attrsp);
typedef bool
(* PropertyAttributesOp)(JSContext* cx, JS::HandleObject obj, JS::Handle<PropertyName*> name,
                         unsigned* attrsp);
typedef bool
(* DeletePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::Handle<PropertyName*> name,
                     bool* succeeded);
typedef bool
(* DeleteElementOp)(JSContext* cx, JS::HandleObject obj, uint32_t index, bool* succeeded);

typedef bool
(* WatchOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleObject callable);

typedef bool
(* UnwatchOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id);

typedef bool
(* SliceOp)(JSContext* cx, JS::HandleObject obj, uint32_t begin, uint32_t end,
            JS::HandleObject result); // result is actually preallocted.

typedef JSObject*
(* ObjectOp)(JSContext* cx, JS::HandleObject obj);
typedef void
(* FinalizeOp)(FreeOp* fop, JSObject* obj);

#define JS_CLASS_MEMBERS(FinalizeOpType)                                      \
    const char*         name;                                                \
    uint32_t            flags;                                                \
                                                                              \
    /* Mandatory function pointer members. */                                 \
    JSPropertyOp        addProperty;                                          \
    JSDeletePropertyOp  delProperty;                                          \
    JSPropertyOp        getProperty;                                          \
    JSStrictPropertyOp  setProperty;                                          \
    JSEnumerateOp       enumerate;                                            \
    JSResolveOp         resolve;                                              \
    JSConvertOp         convert;                                              \
                                                                              \
    /* Optional members (may be null). */                                     \
    FinalizeOpType      finalize;                                             \
    JSNative            call;                                                 \
    JSHasInstanceOp     hasInstance;                                          \
    JSNative            construct;                                            \
    JSTraceOp           trace

// Callback for the creation of constructor and prototype objects.
typedef JSObject* (*ClassObjectCreationOp)(JSContext* cx, JSProtoKey key);

// Callback for custom post-processing after class initialization via ClassSpec.
typedef bool (*FinishClassInitOp)(JSContext* cx, JS::HandleObject ctor,
                                  JS::HandleObject proto);

struct ClassSpec
{
    ClassObjectCreationOp createConstructor;
    ClassObjectCreationOp createPrototype;
    const JSFunctionSpec* constructorFunctions;
    const JSFunctionSpec* prototypeFunctions;
    FinishClassInitOp finishInit;
    bool defined() const { return !!createConstructor; }
};

struct ClassExtension
{
    JSObjectOp          outerObject;
    JSObjectOp          innerObject;
    JSIteratorOp        iteratorObject;

    /*
     * isWrappedNative is true only if the class is an XPCWrappedNative.
     * WeakMaps use this to override the wrapper disposal optimization.
     */
    bool                isWrappedNative;

    /*
     * If an object is used as a key in a weakmap, it may be desirable for the
     * garbage collector to keep that object around longer than it otherwise
     * would. A common case is when the key is a wrapper around an object in
     * another compartment, and we want to avoid collecting the wrapper (and
     * removing the weakmap entry) as long as the wrapped object is alive. In
     * that case, the wrapped object is returned by the wrapper's
     * weakmapKeyDelegateOp hook. As long as the wrapper is used as a weakmap
     * key, it will not be collected (and remain in the weakmap) until the
     * wrapped object is collected.
     */
    JSWeakmapKeyDelegateOp weakmapKeyDelegateOp;
};

#define JS_NULL_CLASS_SPEC  {nullptr,nullptr,nullptr,nullptr,nullptr}
#define JS_NULL_CLASS_EXT   {nullptr,nullptr,nullptr,false,nullptr}

struct ObjectOps
{
    LookupGenericOp     lookupGeneric;
    LookupPropOp        lookupProperty;
    LookupElementOp     lookupElement;
    DefineGenericOp     defineGeneric;
    DefinePropOp        defineProperty;
    DefineElementOp     defineElement;
    GenericIdOp         getGeneric;
    PropertyIdOp        getProperty;
    ElementIdOp         getElement;
    StrictGenericIdOp   setGeneric;
    StrictPropertyIdOp  setProperty;
    StrictElementIdOp   setElement;
    GenericAttributesOp getGenericAttributes;
    GenericAttributesOp setGenericAttributes;
    DeletePropertyOp    deleteProperty;
    DeleteElementOp     deleteElement;
    WatchOp             watch;
    UnwatchOp           unwatch;
    SliceOp             slice; // Optimized slice, can be null.

    JSNewEnumerateOp    enumerate;
    ObjectOp            thisObject;
};

#define JS_NULL_OBJECT_OPS                                                    \
    {nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr, \
     nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr, \
     nullptr,nullptr}

} // namespace js

// Classes, objects, and properties.

typedef void (*JSClassInternal)();

struct JSClass {
    JS_CLASS_MEMBERS(JSFinalizeOp);

    void*               reserved[31];
};

#define JSCLASS_HAS_PRIVATE             (1<<0)  // objects have private slot
#define JSCLASS_NEW_ENUMERATE           (1<<1)  // has JSNewEnumerateOp hook
#define JSCLASS_NEW_RESOLVE             (1<<2)  // has JSNewResolveOp hook
#define JSCLASS_PRIVATE_IS_NSISUPPORTS  (1<<3)  // private is (nsISupports*)
#define JSCLASS_IS_DOMJSCLASS           (1<<4)  // objects are DOM
#define JSCLASS_IMPLEMENTS_BARRIERS     (1<<5)  // Correctly implements GC read
                                                // and write barriers
#define JSCLASS_EMULATES_UNDEFINED      (1<<6)  // objects of this class act
                                                // like the value undefined,
                                                // in some contexts
#define JSCLASS_USERBIT1                (1<<7)  // Reserved for embeddings.

// To reserve slots fetched and stored via JS_Get/SetReservedSlot, bitwise-or
// JSCLASS_HAS_RESERVED_SLOTS(n) into the initializer for JSClass.flags, where
// n is a constant in [1, 255].  Reserved slots are indexed from 0 to n-1.
#define JSCLASS_RESERVED_SLOTS_SHIFT    8       // room for 8 flags below */
#define JSCLASS_RESERVED_SLOTS_WIDTH    8       // and 16 above this field */
#define JSCLASS_RESERVED_SLOTS_MASK     JS_BITMASK(JSCLASS_RESERVED_SLOTS_WIDTH)
#define JSCLASS_HAS_RESERVED_SLOTS(n)   (((n) & JSCLASS_RESERVED_SLOTS_MASK)  \
                                         << JSCLASS_RESERVED_SLOTS_SHIFT)
#define JSCLASS_RESERVED_SLOTS(clasp)   (((clasp)->flags                      \
                                          >> JSCLASS_RESERVED_SLOTS_SHIFT)    \
                                         & JSCLASS_RESERVED_SLOTS_MASK)

#define JSCLASS_HIGH_FLAGS_SHIFT        (JSCLASS_RESERVED_SLOTS_SHIFT +       \
                                         JSCLASS_RESERVED_SLOTS_WIDTH)

#define JSCLASS_IS_ANONYMOUS            (1<<(JSCLASS_HIGH_FLAGS_SHIFT+0))
#define JSCLASS_IS_GLOBAL               (1<<(JSCLASS_HIGH_FLAGS_SHIFT+1))
#define JSCLASS_INTERNAL_FLAG2          (1<<(JSCLASS_HIGH_FLAGS_SHIFT+2))
#define JSCLASS_INTERNAL_FLAG3          (1<<(JSCLASS_HIGH_FLAGS_SHIFT+3))

#define JSCLASS_IS_PROXY                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+4))

// Bit 22 unused.

// Reserved for embeddings.
#define JSCLASS_USERBIT2                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+6))
#define JSCLASS_USERBIT3                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+7))

#define JSCLASS_BACKGROUND_FINALIZE     (1<<(JSCLASS_HIGH_FLAGS_SHIFT+8))

// Bits 26 through 31 are reserved for the CACHED_PROTO_KEY mechanism, see
// below.

// ECMA-262 requires that most constructors used internally create objects
// with "the original Foo.prototype value" as their [[Prototype]] (__proto__)
// member initial value.  The "original ... value" verbiage is there because
// in ECMA-262, global properties naming class objects are read/write and
// deleteable, for the most part.
//
// Implementing this efficiently requires that global objects have classes
// with the following flags. Failure to use JSCLASS_GLOBAL_FLAGS was
// previously allowed, but is now an ES5 violation and thus unsupported.
//
#define JSCLASS_GLOBAL_SLOT_COUNT      (3 + JSProto_LIMIT * 3 + 31)
#define JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(n)                                    \
    (JSCLASS_IS_GLOBAL | JSCLASS_HAS_RESERVED_SLOTS(JSCLASS_GLOBAL_SLOT_COUNT + (n)))
#define JSCLASS_GLOBAL_FLAGS                                                  \
    JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(0)
#define JSCLASS_HAS_GLOBAL_FLAG_AND_SLOTS(clasp)                              \
  (((clasp)->flags & JSCLASS_IS_GLOBAL)                                       \
   && JSCLASS_RESERVED_SLOTS(clasp) >= JSCLASS_GLOBAL_SLOT_COUNT)

// Fast access to the original value of each standard class's prototype.
#define JSCLASS_CACHED_PROTO_SHIFT      (JSCLASS_HIGH_FLAGS_SHIFT + 10)
#define JSCLASS_CACHED_PROTO_WIDTH      6
#define JSCLASS_CACHED_PROTO_MASK       JS_BITMASK(JSCLASS_CACHED_PROTO_WIDTH)
#define JSCLASS_HAS_CACHED_PROTO(key)   (uint32_t(key) << JSCLASS_CACHED_PROTO_SHIFT)
#define JSCLASS_CACHED_PROTO_KEY(clasp) ((JSProtoKey)                         \
                                         (((clasp)->flags                     \
                                           >> JSCLASS_CACHED_PROTO_SHIFT)     \
                                          & JSCLASS_CACHED_PROTO_MASK))

// Initializer for unused members of statically initialized JSClass structs.
#define JSCLASS_NO_INTERNAL_MEMBERS     {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
#define JSCLASS_NO_OPTIONAL_MEMBERS     0,0,0,0,0,JSCLASS_NO_INTERNAL_MEMBERS

namespace js {

struct Class
{
    JS_CLASS_MEMBERS(FinalizeOp);
    ClassSpec          spec;
    ClassExtension      ext;
    ObjectOps           ops;

    /* Class is not native and its map is not a scope. */
    static const uint32_t NON_NATIVE = JSCLASS_INTERNAL_FLAG2;

    bool isNative() const {
        return !(flags & NON_NATIVE);
    }

    bool hasPrivate() const {
        return !!(flags & JSCLASS_HAS_PRIVATE);
    }

    bool emulatesUndefined() const {
        return flags & JSCLASS_EMULATES_UNDEFINED;
    }

    bool isJSFunction() const {
        return this == js::FunctionClassPtr;
    }

    bool isCallable() const {
        return isJSFunction() || call;
    }

    bool isProxy() const {
        return flags & JSCLASS_IS_PROXY;
    }

    bool isDOMClass() const {
        return flags & JSCLASS_IS_DOMJSCLASS;
    }

    static size_t offsetOfFlags() { return offsetof(Class, flags); }
};

JS_STATIC_ASSERT(offsetof(JSClass, name) == offsetof(Class, name));
JS_STATIC_ASSERT(offsetof(JSClass, flags) == offsetof(Class, flags));
JS_STATIC_ASSERT(offsetof(JSClass, addProperty) == offsetof(Class, addProperty));
JS_STATIC_ASSERT(offsetof(JSClass, delProperty) == offsetof(Class, delProperty));
JS_STATIC_ASSERT(offsetof(JSClass, getProperty) == offsetof(Class, getProperty));
JS_STATIC_ASSERT(offsetof(JSClass, setProperty) == offsetof(Class, setProperty));
JS_STATIC_ASSERT(offsetof(JSClass, enumerate) == offsetof(Class, enumerate));
JS_STATIC_ASSERT(offsetof(JSClass, resolve) == offsetof(Class, resolve));
JS_STATIC_ASSERT(offsetof(JSClass, convert) == offsetof(Class, convert));
JS_STATIC_ASSERT(offsetof(JSClass, finalize) == offsetof(Class, finalize));
JS_STATIC_ASSERT(offsetof(JSClass, call) == offsetof(Class, call));
JS_STATIC_ASSERT(offsetof(JSClass, construct) == offsetof(Class, construct));
JS_STATIC_ASSERT(offsetof(JSClass, hasInstance) == offsetof(Class, hasInstance));
JS_STATIC_ASSERT(offsetof(JSClass, trace) == offsetof(Class, trace));
JS_STATIC_ASSERT(sizeof(JSClass) == sizeof(Class));

static MOZ_ALWAYS_INLINE const JSClass*
Jsvalify(const Class* c)
{
    return (const JSClass*)c;
}

static MOZ_ALWAYS_INLINE const Class*
Valueify(const JSClass* c)
{
    return (const Class*)c;
}

/*
 * Enumeration describing possible values of the [[Class]] internal property
 * value of objects.
 */
enum ESClassValue {
    ESClass_Array, ESClass_Number, ESClass_String, ESClass_Boolean,
    ESClass_RegExp, ESClass_ArrayBuffer, ESClass_Date,
    // Special snowflake for the ES6 IsArray method.
    // Please don't use it without calling that function.
    ESClass_IsArray
};

/*
 * Return whether the given object has the given [[Class]] internal property
 * value. Beware, this query says nothing about the js::Class of the JSObject
 * so the caller must not assume anything about obj's representation (e.g., obj
 * may be a proxy).
 */
inline bool
ObjectClassIs(JSObject& obj, ESClassValue classValue, JSContext* cx);

/* Just a helper that checks v.isObject before calling ObjectClassIs. */
inline bool
IsObjectWithClass(const JS::Value& v, ESClassValue classValue, JSContext* cx);

}  /* namespace js */

#endif  /* js_Class_h */
