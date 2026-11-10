/**
 * @file llfloatersltelemetry.cpp
 * @author SkyLine
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"
#include "llfloatersltelemetry.h"
#include "sltelemetry.h"
#include "llbutton.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llweb.h"
bool LLFloaterSLTelemetry::postBuild()
{
    getChild<LLButton>("refresh_report")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshReport(); });
    getChild<LLButton>("toggle_consent")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    { SLTelemetry::setConsent(!SLTelemetry::enabled()); refreshReport(); });
    getChild<LLButton>("delete_queue")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    { SLTelemetry::deleteQueued(); refreshReport(); });
    getChild<LLButton>("field_guide")->setCommitCallback([](LLUICtrl*, const LLSD&)
    { LLWeb::loadURL("https://slipstreamviewer.org/telemetry/field-guide.html"); });
    return true;
}
void LLFloaterSLTelemetry::onOpen(const LLSD&) { refreshReport(); }
void LLFloaterSLTelemetry::refreshReport()
{
    // Direct serialized strings, no parse/re-serialization or LLSD conversion.
    getChild<LLTextEditor>("next_report")->setText(SLTelemetry::preview());
    getChild<LLTextBox>("last_label")->setText("Last successfully sent ("+SLTelemetry::lastSentDay()+", local day; literal wire body):");
    getChild<LLTextEditor>("last_report")->setText(SLTelemetry::lastSent());
    getChild<LLTextBox>("queue_count")->setText("Reports waiting to send: " + std::to_string(SLTelemetry::queuedCount()));
    getChild<LLButton>("toggle_consent")->setLabel(SLTelemetry::enabled() ? "Stop sharing" : "Enable");
}
