/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebGLContext.h"
#include "WebGLFramebuffer.h"
#include "WebGLExtensions.h"
#include "WebGLRenderbuffer.h"
#include "WebGLTexture.h"
#include "mozilla/dom/WebGLRenderingContextBinding.h"
#include "WebGLTexture.h"
#include "WebGLRenderbuffer.h"
#include "GLContext.h"
#include "WebGLContextUtils.h"

using namespace mozilla;
using namespace mozilla::gl;

JSObject*
WebGLFramebuffer::WrapObject(JSContext* cx)
{
    return dom::WebGLFramebufferBinding::Wrap(cx, this);
}

WebGLFramebuffer::WebGLFramebuffer(WebGLContext* context)
    : WebGLContextBoundObject(context)
    , mStatus(0)
    , mHasEverBeenBound(false)
    , mDepthAttachment(LOCAL_GL_DEPTH_ATTACHMENT)
    , mStencilAttachment(LOCAL_GL_STENCIL_ATTACHMENT)
    , mDepthStencilAttachment(LOCAL_GL_DEPTH_STENCIL_ATTACHMENT)
{
    SetIsDOMBinding();
    mContext->MakeContextCurrent();
    mContext->gl->fGenFramebuffers(1, &mGLName);
    mContext->mFramebuffers.insertBack(this);

    mColorAttachments.SetLength(1);
    mColorAttachments[0].mAttachmentPoint = LOCAL_GL_COLOR_ATTACHMENT0;
}

bool
WebGLFramebuffer::Attachment::IsDeleteRequested() const
{
    return Texture() ? Texture()->IsDeleteRequested()
         : Renderbuffer() ? Renderbuffer()->IsDeleteRequested()
         : false;
}

bool
WebGLFramebuffer::Attachment::HasAlpha() const
{
    MOZ_ASSERT(HasImage());

    GLenum format = 0;
    if (Texture() && Texture()->HasImageInfoAt(mTexImageTarget, mTexImageLevel))
        format = Texture()->ImageInfoAt(mTexImageTarget, mTexImageLevel).WebGLFormat();
    else if (Renderbuffer())
        format = Renderbuffer()->InternalFormat();
    return FormatHasAlpha(format);
}

bool
WebGLFramebuffer::Attachment::IsReadableFloat() const
{
    if (Texture() && Texture()->HasImageInfoAt(mTexImageTarget, mTexImageLevel)) {
        GLenum type = Texture()->ImageInfoAt(mTexImageTarget, mTexImageLevel).WebGLType();
        switch (type) {
        case LOCAL_GL_FLOAT:
        case LOCAL_GL_HALF_FLOAT_OES:
            return true;
        }
        return false;
    }

    if (Renderbuffer()) {
        GLenum format = Renderbuffer()->InternalFormat();
        switch (format) {
        case LOCAL_GL_RGB16F:
        case LOCAL_GL_RGBA16F:
        case LOCAL_GL_RGB32F:
        case LOCAL_GL_RGBA32F:
            return true;
        }
        return false;
    }

    MOZ_ASSERT(false, "Should not get here.");
    return false;
}

void
WebGLFramebuffer::Attachment::SetTexImage(WebGLTexture* tex, GLenum target, GLint level)
{
    mTexturePtr = tex;
    mRenderbufferPtr = nullptr;
    mTexImageTarget = target;
    mTexImageLevel = level;

    mNeedsFinalize = true;
}

void
WebGLFramebuffer::Attachment::SetRenderbuffer(WebGLRenderbuffer* rb)
{
    mTexturePtr = nullptr;
    mRenderbufferPtr = rb;

    mNeedsFinalize = true;
}

bool
WebGLFramebuffer::Attachment::HasUninitializedImageData() const
{
    if (!HasImage())
        return false;

    if (Renderbuffer()) {
        return Renderbuffer()->HasUninitializedImageData();
    }

    if (Texture()) {
        MOZ_ASSERT(Texture()->HasImageInfoAt(mTexImageTarget, mTexImageLevel));
        return Texture()->ImageInfoAt(mTexImageTarget, mTexImageLevel).HasUninitializedImageData();
    }

    MOZ_ASSERT(false, "Should not get here.");
    return false;
}

void
WebGLFramebuffer::Attachment::SetImageDataStatus(WebGLImageDataStatus newStatus)
{
    if (!HasImage())
        return;

    if (Renderbuffer()) {
        Renderbuffer()->SetImageDataStatus(newStatus);
        return;
    }

    if (Texture()) {
        Texture()->SetImageDataStatus(mTexImageTarget, mTexImageLevel, newStatus);
        return;
    }

    MOZ_ASSERT(false, "Should not get here.");
}

bool
WebGLFramebuffer::Attachment::HasImage() const
{
    if (Texture() && Texture()->HasImageInfoAt(mTexImageTarget, mTexImageLevel))
        return true;

    if (Renderbuffer())
        return true;

    return false;
}

const WebGLRectangleObject&
WebGLFramebuffer::Attachment::RectangleObject() const
{
    MOZ_ASSERT(HasImage(), "Make sure it has an image before requesting the rectangle.");

    if (Texture()) {
        MOZ_ASSERT(Texture()->HasImageInfoAt(mTexImageTarget, mTexImageLevel));
        return Texture()->ImageInfoAt(mTexImageTarget, mTexImageLevel);
    }

    if (Renderbuffer()) {
        return *Renderbuffer();
    }

    MOZ_CRASH("Should not get here.");
}

/* The following IsValidFBOTextureXXX functions check the internal
   format that is used by GL or GL ES texture formats.  This
   corresponds to the state that is stored in
   WebGLTexture::ImageInfo::InternalFormat()*/
static inline bool
IsValidFBOTextureColorFormat(GLenum internalFormat)
{
    /* These formats are internal formats for each texture -- the actual
     * low level format, which we might have to do conversions for when
     * running against desktop GL (e.g. GL_RGBA + GL_FLOAT -> GL_RGBA32F).
     *
     * This function just handles all of them whether desktop GL or ES.
     */

    return (
        /* linear 8-bit formats */
        internalFormat == LOCAL_GL_ALPHA ||
        internalFormat == LOCAL_GL_LUMINANCE ||
        internalFormat == LOCAL_GL_LUMINANCE_ALPHA ||
        internalFormat == LOCAL_GL_RGB ||
        internalFormat == LOCAL_GL_RGBA ||
        /* sRGB 8-bit formats */
        internalFormat == LOCAL_GL_SRGB_EXT ||
        internalFormat == LOCAL_GL_SRGB_ALPHA_EXT ||
        /* linear float32 formats */
        internalFormat == LOCAL_GL_ALPHA32F_ARB ||
        internalFormat == LOCAL_GL_LUMINANCE32F_ARB ||
        internalFormat == LOCAL_GL_LUMINANCE_ALPHA32F_ARB ||
        internalFormat == LOCAL_GL_RGB32F_ARB ||
        internalFormat == LOCAL_GL_RGBA32F_ARB ||
        /* texture_half_float formats */
        internalFormat == LOCAL_GL_ALPHA16F_ARB ||
        internalFormat == LOCAL_GL_LUMINANCE16F_ARB ||
        internalFormat == LOCAL_GL_LUMINANCE_ALPHA16F_ARB ||
        internalFormat == LOCAL_GL_RGB16F_ARB ||
        internalFormat == LOCAL_GL_RGBA16F_ARB
    );
}

static inline bool
IsValidFBOTextureDepthFormat(GLenum internalFormat)
{
    return (
        internalFormat == LOCAL_GL_DEPTH_COMPONENT ||
        internalFormat == LOCAL_GL_DEPTH_COMPONENT16 ||
        internalFormat == LOCAL_GL_DEPTH_COMPONENT32);
}

static inline bool
IsValidFBOTextureDepthStencilFormat(GLenum internalFormat)
{
    return (
        internalFormat == LOCAL_GL_DEPTH_STENCIL ||
        internalFormat == LOCAL_GL_DEPTH24_STENCIL8);
}

/* The following IsValidFBORenderbufferXXX functions check the internal
   format that is stored by WebGLRenderbuffer::InternalFormat(). Valid
   values can be found in WebGLContext::RenderbufferStorage. */
static inline bool
IsValidFBORenderbufferColorFormat(GLenum internalFormat)
{
    return (
        internalFormat == LOCAL_GL_RGB565 ||
        internalFormat == LOCAL_GL_RGB5_A1 ||
        internalFormat == LOCAL_GL_RGBA4 ||
        internalFormat == LOCAL_GL_SRGB8_ALPHA8_EXT);
}

static inline bool
IsValidFBORenderbufferDepthFormat(GLenum internalFormat)
{
    return internalFormat == LOCAL_GL_DEPTH_COMPONENT16;
}

static inline bool
IsValidFBORenderbufferDepthStencilFormat(GLenum internalFormat)
{
    return internalFormat == LOCAL_GL_DEPTH_STENCIL;
}

static inline bool
IsValidFBORenderbufferStencilFormat(GLenum internalFormat)
{
    return internalFormat == LOCAL_GL_STENCIL_INDEX8;
}

bool
WebGLFramebuffer::Attachment::IsComplete() const
{
    if (!HasImage())
        return false;

    const WebGLRectangleObject& rect = RectangleObject();

    if (!rect.Width() ||
        !rect.Height())
    {
        return false;
    }

    if (Texture()) {
        MOZ_ASSERT(Texture()->HasImageInfoAt(mTexImageTarget, mTexImageLevel));
        const WebGLTexture::ImageInfo& imageInfo =
            Texture()->ImageInfoAt(mTexImageTarget, mTexImageLevel);
        GLenum webGLFormat = imageInfo.WebGLFormat();

        if (mAttachmentPoint == LOCAL_GL_DEPTH_ATTACHMENT)
            return IsValidFBOTextureDepthFormat(webGLFormat);

        if (mAttachmentPoint == LOCAL_GL_DEPTH_STENCIL_ATTACHMENT)
            return IsValidFBOTextureDepthStencilFormat(webGLFormat);

        if (mAttachmentPoint >= LOCAL_GL_COLOR_ATTACHMENT0 &&
            mAttachmentPoint < GLenum(LOCAL_GL_COLOR_ATTACHMENT0 +
                                      WebGLContext::sMaxColorAttachments))
        {
            return IsValidFBOTextureColorFormat(webGLFormat);
        }
        MOZ_ASSERT(false, "Invalid WebGL attachment point?");
        return false;
    }

    if (Renderbuffer()) {
        GLenum internalFormat = Renderbuffer()->InternalFormat();

        if (mAttachmentPoint == LOCAL_GL_DEPTH_ATTACHMENT)
            return IsValidFBORenderbufferDepthFormat(internalFormat);

        if (mAttachmentPoint == LOCAL_GL_STENCIL_ATTACHMENT)
            return IsValidFBORenderbufferStencilFormat(internalFormat);

        if (mAttachmentPoint == LOCAL_GL_DEPTH_STENCIL_ATTACHMENT)
            return IsValidFBORenderbufferDepthStencilFormat(internalFormat);

        if (mAttachmentPoint >= LOCAL_GL_COLOR_ATTACHMENT0 &&
            mAttachmentPoint < GLenum(LOCAL_GL_COLOR_ATTACHMENT0 +
                                      WebGLContext::sMaxColorAttachments))
        {
            return IsValidFBORenderbufferColorFormat(internalFormat);
        }
        MOZ_ASSERT(false, "Invalid WebGL attachment point?");
        return false;
    }

    MOZ_ASSERT(false, "Should not get here.");
    return false;
}

void
WebGLFramebuffer::Attachment::FinalizeAttachment(GLContext* gl, GLenum attachmentLoc) const
{
    if (!mNeedsFinalize)
        return;

    mNeedsFinalize = false;

    if (!HasImage()) {
        if (attachmentLoc == LOCAL_GL_DEPTH_STENCIL_ATTACHMENT) {
            gl->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_DEPTH_ATTACHMENT,
                                         LOCAL_GL_RENDERBUFFER, 0);
            gl->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_STENCIL_ATTACHMENT,
                                         LOCAL_GL_RENDERBUFFER, 0);
        } else {
            gl->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, attachmentLoc,
                                         LOCAL_GL_RENDERBUFFER, 0);
        }

        return;
    }
    MOZ_ASSERT(HasImage());

    if (Texture()) {
        MOZ_ASSERT(gl == Texture()->Context()->gl);

        if (attachmentLoc == LOCAL_GL_DEPTH_STENCIL_ATTACHMENT) {
            gl->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_DEPTH_ATTACHMENT,
                                      TexImageTarget(), Texture()->GLName(), TexImageLevel());
            gl->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_STENCIL_ATTACHMENT,
                                      TexImageTarget(), Texture()->GLName(), TexImageLevel());
        } else {
            gl->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, attachmentLoc,
                                      TexImageTarget(), Texture()->GLName(), TexImageLevel());
        }
        return;
    }

    if (Renderbuffer()) {
        Renderbuffer()->FramebufferRenderbuffer(attachmentLoc);
        return;
    }

    MOZ_ASSERT(false, "Should not get here.");
}

void
WebGLFramebuffer::Delete()
{
    DetachAllAttachments();
    mColorAttachments.Clear();
    mDepthAttachment.Reset();
    mStencilAttachment.Reset();
    mDepthStencilAttachment.Reset();

    mContext->MakeContextCurrent();
    mContext->gl->fDeleteFramebuffers(1, &mGLName);
    LinkedListElement<WebGLFramebuffer>::removeFrom(mContext->mFramebuffers);
}

void
WebGLFramebuffer::DetachAttachment(WebGLFramebuffer::Attachment& attachment)
{
    if (attachment.Texture())
        attachment.Texture()->DetachFrom(this, attachment.mAttachmentPoint);

    if (attachment.Renderbuffer())
        attachment.Renderbuffer()->DetachFrom(this, attachment.mAttachmentPoint);
}

void
WebGLFramebuffer::DetachAllAttachments()
{
    size_t count = mColorAttachments.Length();
    for (size_t i = 0; i < count; i++) {
        DetachAttachment(mColorAttachments[i]);
    }

    DetachAttachment(mDepthAttachment);
    DetachAttachment(mStencilAttachment);
    DetachAttachment(mDepthStencilAttachment);
}

void
WebGLFramebuffer::FramebufferRenderbuffer(GLenum target,
                                          GLenum attachment,
                                          GLenum rbtarget,
                                          WebGLRenderbuffer* wrb)
{
    MOZ_ASSERT(mContext->mBoundFramebuffer == this);

    if (!mContext->ValidateObjectAllowNull("framebufferRenderbuffer: renderbuffer", wrb))
        return;

    if (target != LOCAL_GL_FRAMEBUFFER)
        return mContext->ErrorInvalidEnumInfo("framebufferRenderbuffer: target", target);

    if (rbtarget != LOCAL_GL_RENDERBUFFER)
        return mContext->ErrorInvalidEnumInfo("framebufferRenderbuffer: renderbuffer target:", rbtarget);

    /* Get the requested attachment. If result is NULL, attachment is
     * invalid and an error is generated.
     *
     * Don't use GetAttachment(...) here because it opt builds it
     * returns mColorAttachment[0] for invalid attachment, which we
     * really don't want to mess with.
     */
    Attachment* a = GetAttachmentOrNull(attachment);
    if (!a)
        return; // Error generated internally to GetAttachmentOrNull.

    /* Invalidate cached framebuffer status and inform texture of it's
     * new attachment
     */
    mStatus = 0;
    // Detach current
    if (a->Texture())
        a->Texture()->DetachFrom(this, attachment);
    else if (a->Renderbuffer())
        a->Renderbuffer()->DetachFrom(this, attachment);

    // Attach new
    if (wrb)
        wrb->AttachTo(this, attachment);

    a->SetRenderbuffer(wrb);
}

void
WebGLFramebuffer::FramebufferTexture2D(GLenum target,
                                       GLenum attachment,
                                       GLenum textarget,
                                       WebGLTexture* wtex,
                                       GLint level)
{
    MOZ_ASSERT(mContext->mBoundFramebuffer == this);

    if (!mContext->ValidateObjectAllowNull("framebufferTexture2D: texture", wtex))
        return;

    if (target != LOCAL_GL_FRAMEBUFFER)
        return mContext->ErrorInvalidEnumInfo("framebufferTexture2D: target", target);

    if (textarget != LOCAL_GL_TEXTURE_2D &&
        (textarget < LOCAL_GL_TEXTURE_CUBE_MAP_POSITIVE_X ||
         textarget > LOCAL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
    {
        return mContext->ErrorInvalidEnumInfo("framebufferTexture2D: invalid texture target", textarget);
    }

    if (wtex) {
        bool isTexture2D = wtex->Target() == LOCAL_GL_TEXTURE_2D;
        bool isTexTarget2D = textarget == LOCAL_GL_TEXTURE_2D;
        if (isTexture2D != isTexTarget2D) {
            return mContext->ErrorInvalidOperation("framebufferTexture2D: mismatched texture and texture target");
        }
    }

    if (level != 0)
        return mContext->ErrorInvalidValue("framebufferTexture2D: level must be 0");

    /* Get the requested attachment. If result is NULL, attachment is
     * invalid and an error is generated.
     *
     * Don't use GetAttachment(...) here because it opt builds it
     * returns mColorAttachment[0] for invalid attachment, which we
     * really don't want to mess with.
     */
    Attachment* a = GetAttachmentOrNull(attachment);
    if (!a)
        return; // Error generated internally to GetAttachmentOrNull.

    /* Invalidate cached framebuffer status and inform texture of it's
     * new attachment
     */
    mStatus = 0;
    // Detach current
    if (a->Texture())
        a->Texture()->DetachFrom(this, attachment);
    else if (a->Renderbuffer())
        a->Renderbuffer()->DetachFrom(this, attachment);

    // Attach new
    if (wtex)
        wtex->AttachTo(this, attachment);

    a->SetTexImage(wtex, textarget, level);
}

WebGLFramebuffer::Attachment*
WebGLFramebuffer::GetAttachmentOrNull(GLenum attachment)
{
    if (attachment == LOCAL_GL_DEPTH_STENCIL_ATTACHMENT)
        return &mDepthStencilAttachment;

    if (attachment == LOCAL_GL_DEPTH_ATTACHMENT)
        return &mDepthAttachment;

    if (attachment == LOCAL_GL_STENCIL_ATTACHMENT)
        return &mStencilAttachment;

    if (!CheckColorAttachmentNumber(attachment, "getAttachmentOrNull"))
        return nullptr;

    size_t colorAttachmentId = attachment - LOCAL_GL_COLOR_ATTACHMENT0;
    EnsureColorAttachments(colorAttachmentId);

    return &mColorAttachments[colorAttachmentId];
}

const WebGLFramebuffer::Attachment&
WebGLFramebuffer::GetAttachment(GLenum attachment) const
{
    if (attachment == LOCAL_GL_DEPTH_STENCIL_ATTACHMENT)
        return mDepthStencilAttachment;
    if (attachment == LOCAL_GL_DEPTH_ATTACHMENT)
        return mDepthAttachment;
    if (attachment == LOCAL_GL_STENCIL_ATTACHMENT)
        return mStencilAttachment;

    if (!CheckColorAttachmentNumber(attachment, "getAttachment")) {
        MOZ_ASSERT(false);
        return mColorAttachments[0];
    }

    size_t colorAttachmentId = attachment - LOCAL_GL_COLOR_ATTACHMENT0;
    if (colorAttachmentId >= mColorAttachments.Length()) {
        MOZ_ASSERT(false);
        return mColorAttachments[0];
    }

    return mColorAttachments[colorAttachmentId];
}

void
WebGLFramebuffer::DetachTexture(const WebGLTexture* tex)
{
    size_t count = mColorAttachments.Length();
    for (size_t i = 0; i < count; i++) {
        if (mColorAttachments[i].Texture() == tex) {
            FramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0+i, LOCAL_GL_TEXTURE_2D, nullptr, 0);
            // a texture might be attached more that once while editing the framebuffer
        }
    }

    if (mDepthAttachment.Texture() == tex)
        FramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_DEPTH_ATTACHMENT, LOCAL_GL_TEXTURE_2D, nullptr, 0);
    if (mStencilAttachment.Texture() == tex)
        FramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_STENCIL_ATTACHMENT, LOCAL_GL_TEXTURE_2D, nullptr, 0);
    if (mDepthStencilAttachment.Texture() == tex)
        FramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_DEPTH_STENCIL_ATTACHMENT, LOCAL_GL_TEXTURE_2D, nullptr, 0);
}

void
WebGLFramebuffer::DetachRenderbuffer(const WebGLRenderbuffer* rb)
{
    size_t count = mColorAttachments.Length();
    for (size_t i = 0; i < count; i++) {
        if (mColorAttachments[i].Renderbuffer() == rb) {
            FramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0+i, LOCAL_GL_RENDERBUFFER, nullptr);
            // a renderbuffer might be attached more that once while editing the framebuffer
        }
    }

    if (mDepthAttachment.Renderbuffer() == rb)
        FramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_DEPTH_ATTACHMENT, LOCAL_GL_RENDERBUFFER, nullptr);
    if (mStencilAttachment.Renderbuffer() == rb)
        FramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_STENCIL_ATTACHMENT, LOCAL_GL_RENDERBUFFER, nullptr);
    if (mDepthStencilAttachment.Renderbuffer() == rb)
        FramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_DEPTH_STENCIL_ATTACHMENT, LOCAL_GL_RENDERBUFFER, nullptr);
}

bool
WebGLFramebuffer::HasDefinedAttachments() const
{
    bool hasAttachments = false;

    size_t count = mColorAttachments.Length();
    for (size_t i = 0; i < count; i++) {
        hasAttachments |= mColorAttachments[i].IsDefined();
    }

    hasAttachments |= mDepthAttachment.IsDefined();
    hasAttachments |= mStencilAttachment.IsDefined();
    hasAttachments |= mDepthStencilAttachment.IsDefined();

    return hasAttachments;
}


static bool
IsIncomplete(const WebGLFramebuffer::Attachment& cur)
{
    return cur.IsDefined() && !cur.IsComplete();
}

bool
WebGLFramebuffer::HasIncompleteAttachments() const
{
    bool hasIncomplete = false;

    size_t count = mColorAttachments.Length();
    for (size_t i = 0; i < count; i++) {
        hasIncomplete |= IsIncomplete(mColorAttachments[i]);
    }

    hasIncomplete |= IsIncomplete(mDepthAttachment);
    hasIncomplete |= IsIncomplete(mStencilAttachment);
    hasIncomplete |= IsIncomplete(mDepthStencilAttachment);

    return hasIncomplete;
}


const WebGLRectangleObject&
WebGLFramebuffer::GetAnyRectObject() const
{
    MOZ_ASSERT(HasDefinedAttachments());

    size_t count = mColorAttachments.Length();
    for (size_t i = 0; i < count; i++) {
        if (mColorAttachments[i].HasImage())
            return mColorAttachments[i].RectangleObject();
    }

    if (mDepthAttachment.HasImage())
        return mDepthAttachment.RectangleObject();

    if (mStencilAttachment.HasImage())
        return mStencilAttachment.RectangleObject();

    if (mDepthStencilAttachment.HasImage())
        return mDepthStencilAttachment.RectangleObject();

    MOZ_CRASH("Should not get here.");
}


static bool
RectsMatch(const WebGLFramebuffer::Attachment& attachment,
           const WebGLRectangleObject& rect)
{
    return attachment.RectangleObject().HasSameDimensionsAs(rect);
}

bool
WebGLFramebuffer::AllImageRectsMatch() const
{
    MOZ_ASSERT(HasDefinedAttachments());
    MOZ_ASSERT(!HasIncompleteAttachments());

    const WebGLRectangleObject& rect = GetAnyRectObject();

    // Alright, we have *a* rect, let's check all the others.
    bool imageRectsMatch = true;

    size_t count = mColorAttachments.Length();
    for (size_t i = 0; i < count; i++) {
        if (mColorAttachments[i].HasImage())
            imageRectsMatch &= RectsMatch(mColorAttachments[i], rect);
    }

    if (mDepthAttachment.HasImage())
        imageRectsMatch &= RectsMatch(mDepthAttachment, rect);

    if (mStencilAttachment.HasImage())
        imageRectsMatch &= RectsMatch(mStencilAttachment, rect);

    if (mDepthStencilAttachment.HasImage())
        imageRectsMatch &= RectsMatch(mDepthStencilAttachment, rect);

    return imageRectsMatch;
}


const WebGLRectangleObject&
WebGLFramebuffer::RectangleObject() const
{
    // If we're using this as the RectObj of an FB, we need to be sure the FB
    // has a consistent rect.
    MOZ_ASSERT(AllImageRectsMatch(), "Did you mean `GetAnyRectObject`?");
    return GetAnyRectObject();
}

GLenum
WebGLFramebuffer::PrecheckFramebufferStatus() const
{
    MOZ_ASSERT(mContext->mBoundFramebuffer == this);

    if (!HasDefinedAttachments())
        return LOCAL_GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT; // No attachments

    if (HasIncompleteAttachments())
        return LOCAL_GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

    if (!AllImageRectsMatch())
        return LOCAL_GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS; // Inconsistent sizes

    if (HasDepthStencilConflict())
        return LOCAL_GL_FRAMEBUFFER_UNSUPPORTED;

    return LOCAL_GL_FRAMEBUFFER_COMPLETE;
}

GLenum
WebGLFramebuffer::CheckFramebufferStatus() const
{
    if (mStatus != 0)
        return mStatus;

    mStatus = PrecheckFramebufferStatus();
    if (mStatus != LOCAL_GL_FRAMEBUFFER_COMPLETE)
        return mStatus;

    // Looks good on our end. Let's ask the driver.
    mContext->MakeContextCurrent();

    // Ok, attach our chosen flavor of {DEPTH, STENCIL, DEPTH_STENCIL}.
    FinalizeAttachments();

    mStatus = mContext->gl->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
    return mStatus;
}

bool
WebGLFramebuffer::HasCompletePlanes(GLbitfield mask)
{
    if (CheckFramebufferStatus() != LOCAL_GL_FRAMEBUFFER_COMPLETE)
        return false;

    MOZ_ASSERT(mContext->mBoundFramebuffer == this);
    bool hasPlanes = true;
    if (mask & LOCAL_GL_COLOR_BUFFER_BIT) {
        hasPlanes &= ColorAttachmentCount() &&
                     ColorAttachment(0).IsDefined();
    }

    if (mask & LOCAL_GL_DEPTH_BUFFER_BIT) {
        hasPlanes &= DepthAttachment().IsDefined() ||
                     DepthStencilAttachment().IsDefined();
    }

    if (mask & LOCAL_GL_STENCIL_BUFFER_BIT) {
        hasPlanes &= StencilAttachment().IsDefined() ||
                     DepthStencilAttachment().IsDefined();
    }

    return hasPlanes;
}

bool
WebGLFramebuffer::CheckAndInitializeAttachments()
{
    MOZ_ASSERT(mContext->mBoundFramebuffer == this);

    if (CheckFramebufferStatus() != LOCAL_GL_FRAMEBUFFER_COMPLETE)
        return false;

    // Cool! We've checked out ok. Just need to initialize.
    size_t colorAttachmentCount = mColorAttachments.Length();

    // Check if we need to initialize anything
    {
        bool hasUninitializedAttachments = false;

        for (size_t i = 0; i < colorAttachmentCount; i++) {
            if (mColorAttachments[i].HasImage())
                hasUninitializedAttachments |= mColorAttachments[i].HasUninitializedImageData();
        }

        if (mDepthAttachment.HasImage())
            hasUninitializedAttachments |= mDepthAttachment.HasUninitializedImageData();
        if (mStencilAttachment.HasImage())
            hasUninitializedAttachments |= mStencilAttachment.HasUninitializedImageData();
        if (mDepthStencilAttachment.HasImage())
            hasUninitializedAttachments |= mDepthStencilAttachment.HasUninitializedImageData();

        if (!hasUninitializedAttachments)
            return true;
    }

    // Get buffer-bit-mask and color-attachment-mask-list
    uint32_t mask = 0;
    bool colorAttachmentsMask[WebGLContext::sMaxColorAttachments] = { false };
    MOZ_ASSERT(colorAttachmentCount <= WebGLContext::sMaxColorAttachments);

    for (size_t i = 0; i < colorAttachmentCount; i++) {
        if (mColorAttachments[i].HasUninitializedImageData()) {
          colorAttachmentsMask[i] = true;
          mask |= LOCAL_GL_COLOR_BUFFER_BIT;
        }
    }

    if (mDepthAttachment.HasUninitializedImageData() ||
        mDepthStencilAttachment.HasUninitializedImageData())
    {
        mask |= LOCAL_GL_DEPTH_BUFFER_BIT;
    }

    if (mStencilAttachment.HasUninitializedImageData() ||
        mDepthStencilAttachment.HasUninitializedImageData())
    {
        mask |= LOCAL_GL_STENCIL_BUFFER_BIT;
    }

    // Clear!
    mContext->ForceClearFramebufferWithDefaultValues(mask, colorAttachmentsMask);

    // Mark all the uninitialized images as initialized.
    for (size_t i = 0; i < colorAttachmentCount; i++) {
        if (mColorAttachments[i].HasUninitializedImageData())
            mColorAttachments[i].SetImageDataStatus(WebGLImageDataStatus::InitializedImageData);
    }

    if (mDepthAttachment.HasUninitializedImageData())
        mDepthAttachment.SetImageDataStatus(WebGLImageDataStatus::InitializedImageData);
    if (mStencilAttachment.HasUninitializedImageData())
        mStencilAttachment.SetImageDataStatus(WebGLImageDataStatus::InitializedImageData);
    if (mDepthStencilAttachment.HasUninitializedImageData())
        mDepthStencilAttachment.SetImageDataStatus(WebGLImageDataStatus::InitializedImageData);

    return true;
}

bool WebGLFramebuffer::CheckColorAttachmentNumber(GLenum attachment, const char* functionName) const
{
    const char* const errorFormating = "%s: attachment: invalid enum value 0x%x";

    if (mContext->IsExtensionEnabled(WebGLExtensionID::WEBGL_draw_buffers)) {
        if (attachment < LOCAL_GL_COLOR_ATTACHMENT0 ||
            attachment >= GLenum(LOCAL_GL_COLOR_ATTACHMENT0 + mContext->mGLMaxColorAttachments))
        {
            mContext->ErrorInvalidEnum(errorFormating, functionName, attachment);
            return false;
        }
    } else if (attachment != LOCAL_GL_COLOR_ATTACHMENT0) {
        if (attachment > LOCAL_GL_COLOR_ATTACHMENT0 &&
            attachment <= LOCAL_GL_COLOR_ATTACHMENT15)
        {
            mContext->ErrorInvalidEnum("%s: attachment: invalid enum value 0x%x. "
                                       "Try the WEBGL_draw_buffers extension if supported.", functionName, attachment);
            return false;
        } else {
            mContext->ErrorInvalidEnum(errorFormating, functionName, attachment);
            return false;
        }
    }

    return true;
}

void WebGLFramebuffer::EnsureColorAttachments(size_t colorAttachmentId)
{
    MOZ_ASSERT(colorAttachmentId < WebGLContext::sMaxColorAttachments);

    size_t currentAttachmentCount = mColorAttachments.Length();
    if (colorAttachmentId < currentAttachmentCount)
        return;

    mColorAttachments.SetLength(colorAttachmentId + 1);

    for (size_t i = colorAttachmentId; i >= currentAttachmentCount; i--) {
        mColorAttachments[i].mAttachmentPoint = LOCAL_GL_COLOR_ATTACHMENT0 + i;
    }
}

void
WebGLFramebuffer::NotifyAttachableChanged() const
{
    // Attachment has changed, so invalidate cached status
    mStatus = 0;
}

static void
FinalizeDrawAndReadBuffers(GLContext* aGL, bool aColorBufferDefined)
{
    MOZ_ASSERT(aGL, "Expected a valid GLContext ptr.");
    // GLES don't support DrawBuffer()/ReadBuffer.
    // According to http://www.opengl.org/wiki/Framebuffer_Object
    //
    // Each draw buffers must either specify color attachment points that have images
    // attached or must be GL_NONE???. (GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER??? when false).
    //
    // If the read buffer is set, then it must specify an attachment point that has an
    // image attached. (GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER??? when false).
    //
    // Note that this test is not performed if OpenGL 4.2 or ARB_ES2_compatibility is
    // available.
    if (aGL->IsGLES() ||
        aGL->IsSupported(GLFeature::ES2_compatibility) ||
        aGL->IsAtLeast(ContextProfile::OpenGL, 420))
    {
        return;
    }

    // TODO(djg): Assert that fDrawBuffer/fReadBuffer is not NULL.
    GLenum colorBufferSource = aColorBufferDefined ? LOCAL_GL_COLOR_ATTACHMENT0 : LOCAL_GL_NONE;
    aGL->fDrawBuffer(colorBufferSource);
    aGL->fReadBuffer(colorBufferSource);
}

void
WebGLFramebuffer::FinalizeAttachments() const
{
    GLContext* gl = mContext->gl;

    size_t count = ColorAttachmentCount();
    for (size_t i = 0; i < count; i++) {
        ColorAttachment(i).FinalizeAttachment(gl, LOCAL_GL_COLOR_ATTACHMENT0 + i);
    }

    DepthAttachment().FinalizeAttachment(gl, LOCAL_GL_DEPTH_ATTACHMENT);
    StencilAttachment().FinalizeAttachment(gl, LOCAL_GL_STENCIL_ATTACHMENT);
    DepthStencilAttachment().FinalizeAttachment(gl, LOCAL_GL_DEPTH_STENCIL_ATTACHMENT);

    FinalizeDrawAndReadBuffers(gl, ColorAttachment(0).IsDefined());
}

inline void
ImplCycleCollectionUnlink(mozilla::WebGLFramebuffer::Attachment& aField)
{
    aField.mTexturePtr = nullptr;
    aField.mRenderbufferPtr = nullptr;
}

inline void
ImplCycleCollectionTraverse(nsCycleCollectionTraversalCallback& aCallback,
                            mozilla::WebGLFramebuffer::Attachment& aField,
                            const char* aName,
                            uint32_t aFlags = 0)
{
    CycleCollectionNoteChild(aCallback, aField.mTexturePtr.get(),
                             aName, aFlags);

    CycleCollectionNoteChild(aCallback, aField.mRenderbufferPtr.get(),
                             aName, aFlags);
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_4(WebGLFramebuffer,
  mColorAttachments,
  mDepthAttachment,
  mStencilAttachment,
  mDepthStencilAttachment)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(WebGLFramebuffer, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(WebGLFramebuffer, Release)
