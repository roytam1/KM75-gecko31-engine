/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=8 et :
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/message_loop.h"
#include "chrome/common/child_process_info.h"

#include "mozilla/ipc/Transport.h"

using namespace base;
using namespace std;

namespace mozilla {
namespace ipc {

bool
CreateTransport(ProcessHandle aProcOne, ProcessHandle /*unused*/,
                TransportDescriptor* aOne, TransportDescriptor* aTwo)
{
  wstring id = IPC::Channel::GenerateVerifiedChannelID(std::wstring());
  // Use MODE_SERVER to force creation of the pipe
  Transport t(id, Transport::MODE_SERVER, nullptr);
  HANDLE serverPipe = t.GetServerPipeHandle();
  if (!serverPipe) {
    return false;
  }

  // NB: we create the server pipe immediately, instead of just
  // grabbing an ID, on purpose.  In the current setup, the client
  // needs to connect to an existing server pipe, so to prevent race
  // conditions, we create the server side here and then dup it to the
  // eventual server process.
  HANDLE serverDup;
  DWORD access = 0;
  DWORD options = DUPLICATE_SAME_ACCESS;
  if (!DuplicateHandle(GetCurrentProcess(), serverPipe, aProcOne,
                       &serverDup,
                       access,
                       FALSE/*not inheritable*/,
                       options)) {
    return false;
  }

  aOne->mPipeName = aTwo->mPipeName = id;
  aOne->mServerPipe = serverDup;
  aTwo->mServerPipe = INVALID_HANDLE_VALUE;
  return true;
}

Transport*
OpenDescriptor(const TransportDescriptor& aTd, Transport::Mode aMode)
{
  return new Transport(aTd.mPipeName, aTd.mServerPipe, aMode, nullptr);
}

Transport*
OpenDescriptor(const FileDescriptor& aFd, Transport::Mode aMode)
{
  NS_NOTREACHED("Not implemented!");
  return nullptr;
}

void
CloseDescriptor(const TransportDescriptor& aTd)
{
  CloseHandle(aTd.mServerPipe);
}

} // namespace ipc
} // namespace mozilla
