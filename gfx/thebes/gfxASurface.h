/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_ASURFACE_H
#define GFX_ASURFACE_H

#include "mozilla/MemoryReporting.h"
#include "gfxTypes.h"
#include "mozilla/Scoped.h"
#include "nscore.h"
#include "nsSize.h"

#ifdef MOZILLA_INTERNAL_API
#include "nsStringFwd.h"
#else
#include "nsStringAPI.h"
#endif

class gfxImageSurface;
struct nsIntPoint;
struct nsIntRect;
struct gfxRect;
struct gfxPoint;

template <typename T>
struct already_AddRefed;

/**
 * A surface is something you can draw on. Instantiate a subclass of this
 * abstract class, and use gfxContext to draw on this surface.
 */
class gfxASurface {
public:
#ifdef MOZILLA_INTERNAL_API
    nsrefcnt AddRef(void);
    nsrefcnt Release(void);

    // These functions exist so that browsercomps can refcount a gfxASurface
    virtual nsrefcnt AddRefExternal(void);
    virtual nsrefcnt ReleaseExternal(void);
#else
    virtual nsrefcnt AddRef(void);
    virtual nsrefcnt Release(void);
#endif

public:

    /** Wrap the given cairo surface and return a gfxASurface for it.
     * This adds a reference to csurf (owned by the returned gfxASurface).
     */
    static already_AddRefed<gfxASurface> Wrap(cairo_surface_t *csurf, const gfxIntSize& aSize = gfxIntSize(-1, -1));

    /*** this DOES NOT addref the surface */
    cairo_surface_t *CairoSurface() {
        return mSurface;
    }

    gfxSurfaceType GetType() const;

    gfxContentType GetContentType() const;

    void SetDeviceOffset(const gfxPoint& offset);
    gfxPoint GetDeviceOffset() const;

    virtual bool GetRotateForLandscape() { return false; }

    void Flush() const;
    void MarkDirty();
    void MarkDirty(const gfxRect& r);

    /* Printing backend functions */
    virtual nsresult BeginPrinting(const nsAString& aTitle, const nsAString& aPrintToFileName);
    virtual nsresult EndPrinting();
    virtual nsresult AbortPrinting();
    virtual nsresult BeginPage();
    virtual nsresult EndPage();

    void SetData(const cairo_user_data_key_t *key,
                 void *user_data,
                 thebes_destroy_func_t destroy);
    void *GetData(const cairo_user_data_key_t *key);

    virtual void Finish();

    /**
     * Create an offscreen surface that can be efficiently copied into
     * this surface (at least if tiling is not involved).
     * Returns null on error.
     */
    virtual already_AddRefed<gfxASurface> CreateSimilarSurface(gfxContentType aType,
                                                               const nsIntSize& aSize);

    /**
     * Returns an image surface for this surface, or nullptr if not supported.
     * This will not copy image data, just wraps an image surface around
     * pixel data already available in memory.
     */
    virtual already_AddRefed<gfxImageSurface> GetAsImageSurface();

    /**
     * Returns a read-only ARGB32 image surface for this surface. If this is an
     * optimized surface this may require a copy.
     * Returns null on error.
     */
    virtual already_AddRefed<gfxImageSurface> GetAsReadableARGB32ImageSurface();

    /**
     * Creates a new ARGB32 image surface with the same contents as this surface.
     * Returns null on error.
     */
    already_AddRefed<gfxImageSurface> CopyToARGB32ImageSurface();

    int CairoStatus();

    /* Make sure that the given dimensions don't overflow a 32-bit signed int
     * using 4 bytes per pixel; optionally, make sure that either dimension
     * doesn't exceed the given limit.
     */
    static bool CheckSurfaceSize(const nsIntSize& sz, int32_t limit = 0);

    /* Provide a stride value that will respect all alignment requirements of
     * the accelerated image-rendering code.
     */
    static int32_t FormatStrideForWidth(gfxImageFormat format, int32_t width);

    /* Return the default set of context flags for this surface; these are
     * hints to the context about any special rendering considerations.  See
     * gfxContext::SetFlag for documentation.
     */
    virtual int32_t GetDefaultContextFlags() const { return 0; }

    static gfxContentType ContentFromFormat(gfxImageFormat format);

    void SetSubpixelAntialiasingEnabled(bool aEnabled);
    bool GetSubpixelAntialiasingEnabled();

    /**
     * Record number of bytes for given surface type.  Use positive bytes
     * for allocations and negative bytes for deallocations.
     */
    static void RecordMemoryUsedForSurfaceType(gfxSurfaceType aType,
                                               int32_t aBytes);

    /**
     * Same as above, but use current surface type as returned by GetType().
     * The bytes will be accumulated until RecordMemoryFreed is called,
     * in which case the value that was recorded for this surface will
     * be freed.
     */
    void RecordMemoryUsed(int32_t aBytes);
    void RecordMemoryFreed();

    virtual int32_t KnownMemoryUsed() { return mBytesRecorded; }

    virtual size_t SizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;
    virtual size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;
    // gfxASurface has many sub-classes.  This method indicates if a sub-class
    // is capable of measuring its own size accurately.  If not, the caller
    // must fall back to a computed size.  (Note that gfxASurface can actually
    // measure itself, but we must |return false| here because it serves as the
    // (conservative) default for all the sub-classes.  Therefore, this
    // function should only be called on a |gfxASurface*| that actually points
    // to a sub-class of gfxASurface.)
    virtual bool SizeOfIsMeasured() const { return false; }

    /**
     * Where does this surface's memory live?  By default, we say it's in this
     * process's heap.
     */
    virtual gfxMemoryLocation GetMemoryLocation() const;

    static int32_t BytePerPixelFromFormat(gfxImageFormat format);

    virtual const nsIntSize GetSize() const;

    /**
     * Debug functions to encode the current image as a PNG and export it.
     */

    /**
     * Writes a binary PNG file.
     */
    void WriteAsPNG(const char* aFile);

    /**
     * Write as a PNG encoded Data URL to a file.
     */
    void DumpAsDataURL(FILE* aOutput = stdout);

    /**
     * Write as a PNG encoded Data URL to stdout.
     */
    void PrintAsDataURL();

    /**
     * Copy a PNG encoded Data URL to the clipboard.
     */
    void CopyAsDataURL();

    void WriteAsPNG_internal(FILE* aFile, bool aBinary);

    void SetOpaqueRect(const gfxRect& aRect);

    const gfxRect& GetOpaqueRect() {
        if (!!mOpaqueRect)
            return *mOpaqueRect;
        return GetEmptyOpaqueRect();
    }

    /**
     * Move the pixels in |aSourceRect| to |aDestTopLeft|.  Like with
     * memmove(), |aSourceRect| and the rectangle defined by
     * |aDestTopLeft| are allowed to overlap, and the effect is
     * equivalent to copying |aSourceRect| to a scratch surface and
     * then back to |aDestTopLeft|.
     *
     * |aSourceRect| and the destination rectangle defined by
     * |aDestTopLeft| are clipped to this surface's bounds.
     */
    virtual void MovePixels(const nsIntRect& aSourceRect,
                            const nsIntPoint& aDestTopLeft);

    /**
     * Mark the surface as being allowed/not allowed to be used as a source.
     */
    void SetAllowUseAsSource(bool aAllow) { mAllowUseAsSource = aAllow; }
    bool GetAllowUseAsSource() { return mAllowUseAsSource; }

    static uint8_t BytesPerPixel(gfxImageFormat aImageFormat);

protected:
    gfxASurface();

    static gfxASurface* GetSurfaceWrapper(cairo_surface_t *csurf);
    static void SetSurfaceWrapper(cairo_surface_t *csurf, gfxASurface *asurf);

    /**
     * An implementation of MovePixels that assumes the backend can
     * internally handle this operation and doesn't allocate any
     * temporary surfaces.
     */
    void FastMovePixels(const nsIntRect& aSourceRect,
                        const nsIntPoint& aDestTopLeft);

    // NB: Init() *must* be called from within subclass's
    // constructors.  It's unsafe to call it after the ctor finishes;
    // leaks and use-after-frees are possible.
    void Init(cairo_surface_t *surface, bool existingSurface = false);

    // out-of-line helper to allow GetOpaqueRect() to be inlined
    // without including gfxRect.h here
    static const gfxRect& GetEmptyOpaqueRect();

    virtual ~gfxASurface();

    cairo_surface_t *mSurface;
    mozilla::ScopedDeletePtr<gfxRect> mOpaqueRect;

private:
    static void SurfaceDestroyFunc(void *data);

    int32_t mFloatingRefs;
    int32_t mBytesRecorded;

protected:
    bool mSurfaceValid;
    bool mAllowUseAsSource;
};

/**
 * An Unknown surface; used to wrap unknown cairo_surface_t returns from cairo
 */
class gfxUnknownSurface : public gfxASurface {
public:
    gfxUnknownSurface(cairo_surface_t *surf, const gfxIntSize& aSize)
        : mSize(aSize)
    {
        Init(surf, true);
    }

    virtual ~gfxUnknownSurface() { }
    virtual const nsIntSize GetSize() const { return mSize; }

private:
    nsIntSize mSize;
};

#endif /* GFX_ASURFACE_H */
