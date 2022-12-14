/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_NativeKeyBindings_h_
#define mozilla_widget_NativeKeyBindings_h_

#include <gtk/gtk.h>
#include "mozilla/Attributes.h"
#include "mozilla/EventForwards.h"
#include "nsIWidget.h"

namespace mozilla {
namespace widget {

class NativeKeyBindings MOZ_FINAL
{
  typedef nsIWidget::NativeKeyBindingsType NativeKeyBindingsType;
  typedef nsIWidget::DoCommandCallback DoCommandCallback;

public:
  static NativeKeyBindings* GetInstance(NativeKeyBindingsType aType);
  static void Shutdown();

  void Init(NativeKeyBindingsType aType);

  bool Execute(const WidgetKeyboardEvent& aEvent,
               DoCommandCallback aCallback,
               void* aCallbackData);

private:
  ~NativeKeyBindings();

  bool ExecuteInternal(const WidgetKeyboardEvent& aEvent,
                       DoCommandCallback aCallback,
                       void* aCallbackData,
                       guint aKeyval);

  GtkWidget* mNativeTarget;

  static NativeKeyBindings* sInstanceForSingleLineEditor;
  static NativeKeyBindings* sInstanceForMultiLineEditor;
};

} // namespace widget
} // namespace mozilla

#endif // mozilla_widget_NativeKeyBindings_h_
