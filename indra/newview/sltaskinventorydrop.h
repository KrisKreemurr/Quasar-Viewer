/**
 * @file sltaskinventorydrop.h
 * @brief #1466: paced, verified drops of inventory items into a prim's contents.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_TASKINVENTORYDROP_H
#define SL_TASKINVENTORYDROP_H

#include "lltooldraganddrop.h"

#include <string>

class LLInventoryItem;
class LLViewerObject;

// Every drop of an agent or library item into a prim's contents goes through here
// (LLToolDragAndDrop::dropInventory / dropScript). One tracker per target object:
//  - reads the prim's contents from the server before sending (baseline);
//  - sends in small batches, each gated on the sim's reliable-message acks;
//  - after each batch, re-reads the contents (RequestTaskInventory) and matches the
//    new entries to what was sent;
//  - once the contents stop changing, retries an item only when it is provably
//    still absent from the prim and still in the user's inventory (AIS read), at
//    most kMaxAttempts sends per item, never after a server refusal;
//  - reports "N of M added" with what failed and why.
namespace SLTaskInvDrop
{
    // Queue one item for target's contents. The send happens later, from the
    // tracker; nothing is sent or removed from inventory here.
    void queueItem(LLViewerObject* target,
                   LLInventoryItem* item,
                   LLToolDragAndDrop::ESource source,
                   bool is_script,
                   bool script_active);

    // Server alert (AlertMessage / AgentAlertMessage). Any alert while a drop has
    // writes in flight stops that drop's retries and is reported as the reason.
    void onServerAlert(const std::string& notification, const std::string& message);
}

#endif // SL_TASKINVENTORYDROP_H
