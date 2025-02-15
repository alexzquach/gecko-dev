/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_MEDIASESSION_MEDIASESSIONUTILS_H_
#define DOM_MEDIA_MEDIASESSION_MEDIASESSIONUTILS_H_

#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/MediaController.h"

namespace mozilla {
namespace dom {

inline void NotfiyMediaSessionCreationOrDeconstruction(
    BrowsingContext* aContext, bool aIsCreated) {
  if (XRE_IsContentProcess()) {
    ContentChild* contentChild = ContentChild::GetSingleton();
    Unused << contentChild->SendNotifyMediaSessionUpdated(aContext, aIsCreated);
    return;
  }

  RefPtr<MediaController> controller =
      aContext->Canonical()->GetMediaController();
  if (!controller) {
    return;
  }
  if (aIsCreated) {
    controller->NotifySessionCreated(aContext->Id());
  } else {
    controller->NotifySessionDestroyed(aContext->Id());
  }
}

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_MEDIASESSION_MEDIASESSIONUTILS_H_
