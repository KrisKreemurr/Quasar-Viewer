"""Source guards: the bulk mesh uploader runs quietly, gives meshes without a physics mesh a
chosen physics shape, and its Auto-confirm uploads each model exactly once (2026-11-07).

  QUIET          Loading a model never shows or focuses the model uploader:
                 LLModelPreview::loadModel has no openFloater(); the batch shows the uploader
                 only from "Load in uploader" (onLoadSelected); a run's steps (startEntry,
                 advance, stepCurrent, idle) never open, show or focus a floater.
  HIDDEN-PUMP    The hidden uploader is not drawn, so the batch's idle runs pumpBulk(), which
                 runs the preview's update(); loadBulkModel does what onOpen() does for a load
                 (sIgnoreLoadedCallback = false, requestAgentUploadPermissions).
  SINGLE-OPEN    The single-upload entry points open the uploader themselves (the file
                 picker's notify, showModelFile used by the file-type and local-asset paths).
  ONE-BAR        A run's uploads report into the run's progress bar: uploadModel takes
                 progress_batch and onUpload passes the batch's one.
  PHYS-DEFAULT   MeshBulkUploadPhysicsFallback defaults to bounding_box; an unknown name
                 reads as bounding box; applyBulkSettings applies the fallback even when no
                 settings are carried (no early return before it); the fallback is no longer
                 carried from the previous model (no physics_combo in the carried settings).
  PHYS-COMBO     physicsComboIndex matches the uploader's physics_lod_combo: High..Lowest at
                 1..4 and Bounding Box the item before From file.
  PHYS-DROPDOWN  floater_mesh_bulk_upload.xml has physics_fallback_combo, bounding box first,
                 with exactly the names LLMeshBulk knows.
  AUTO-CONFIRM   auto_confirm_check exists next to upload_all_btn, starts unchecked, is never
                 saved and is reset on open; RUN_AUTO_UPLOAD picks only models still in the run
                 (mInRun), a model leaves the run when it is sent; a failed upload or fee in
                 auto mode advances to the next model instead of stopping the run; auto mode
                 waits for the upload-permission answer and checks the L$ balance; uploadNow()
                 is called only from the fee answer (upload-all, auto) and the folder callback,
                 never from a failure path.
  RUN-MARKS-KEPT Starting a run (startAutoUpload, onConfirmUploadAll) never clears its models'
                 mInRun marks between marking them and advance(): beginProgress() used to call
                 endProgress(), so "Upload all remaining" found nothing and did nothing.
  NO-SILENT-RETURN Every early return on the auto path logs why (or stops / skips / uploads).
"""
import re
import xml.etree.ElementTree as ET

NAME = "mesh-bulk-upload"
STAGE = "source"
DESCRIPTION = "bulk mesh upload: hidden uploader (no window, no focus), bounding-box physics fallback, auto-confirm once each"
GUARDS = ("the bulk mesh uploader opening the model uploader and stealing focus for every model, meshes without a "
          "physics mesh uploading with no chosen shape, and auto-confirm uploads sent twice or stopping the run "
          "(2026-11-07).")

BATCH = "indra/newview/llfloatermeshbulkupload.cpp"
FMP = "indra/newview/llfloatermodelpreview.cpp"
MP = "indra/newview/llmodelpreview.cpp"
BULK = "indra/newview/llmeshbulkupload.cpp"
MENU = "indra/newview/llviewermenufile.cpp"
REPO = "indra/newview/llmeshrepository.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"
BATCH_XUI = "indra/newview/skins/default/xui/en/floater_mesh_bulk_upload.xml"
FMP_XUI = "indra/newview/skins/default/xui/en/floater_model_preview.xml"

FOCUS_CALLS = ("openFloater(", "setFocus(", "showInstance(", "showTypedInstance(", "setVisibleAndFrontmost(",
               "setFrontmost(")


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function(text, signature):
    """The body of the function whose definition starts with signature (brace matched)."""
    at = text.find(signature)
    if at < 0:
        return None
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def _read(ctx, rel):
    return open(ctx.path(rel), encoding="utf-8").read()


def _check_quiet(batch, mp, bad):
    lm = _function(mp, "void LLModelPreview::loadModel(")
    if lm is None:
        bad.append("QUIET: LLModelPreview::loadModel not found")
    elif "openFloater(" in lm:
        bad.append("QUIET: LLModelPreview::loadModel opens (and focuses) the uploader on every load")

    for sig in ("void LLFloaterMeshBulkUpload::startEntry(", "void LLFloaterMeshBulkUpload::advance(",
                "void LLFloaterMeshBulkUpload::stepCurrent(", "void LLFloaterMeshBulkUpload::idle(",
                "void LLFloaterMeshBulkUpload::startAutoUpload(", "void LLFloaterMeshBulkUpload::autoUploadCurrent(",
                "void LLFloaterMeshBulkUpload::onPreviewFeeReceived(",
                "void LLFloaterMeshBulkUpload::onPreviewUploadDone("):
        body = _function(batch, sig)
        if body is None:
            bad.append("QUIET: %s) not found" % sig)
            continue
        for call in FOCUS_CALLS:
            if call in body:
                bad.append("QUIET: %s) calls %s (a run must not show or focus a window)" % (sig, call))

    opens = [m.start() for m in re.finditer(r"openFloater\(", batch)]
    load = _function(batch, "void LLFloaterMeshBulkUpload::onLoadSelected(")
    if not load or "openFloater(" not in load:
        bad.append("QUIET: onLoadSelected no longer shows the uploader for the model the user opened")
    if len(opens) != 1:
        bad.append("QUIET: openFloater is called %d times in the batch (only onLoadSelected may show the uploader)"
                   % len(opens))


def _check_pump(batch, fmp, bad):
    idle = _function(batch, "void LLFloaterMeshBulkUpload::idle(") or ""
    if "pumpBulk()" not in idle:
        bad.append("HIDDEN-PUMP: the batch's idle does not pump the hidden uploader")
    pump = _function(fmp, "void LLFloaterModelPreview::pumpBulk(")
    if not pump or "mModelPreview->update()" not in pump or "getVisible()" not in pump:
        bad.append("HIDDEN-PUMP: pumpBulk does not run the preview's update() while the uploader is hidden")
    lbm = _function(fmp, "void LLFloaterModelPreview::loadBulkModel(")
    if not lbm or not re.search(r"LLModelPreview::sIgnoreLoadedCallback\s*=\s*false", lbm):
        bad.append("HIDDEN-PUMP: loadBulkModel does not stop the closed uploader ignoring its loads")
    if not lbm or "requestAgentUploadPermissions()" not in lbm:
        bad.append("HIDDEN-PUMP: loadBulkModel does not ask for upload permission (onOpen does not run hidden)")
    draw = _function(fmp, "void LLFloaterModelPreview::draw(")
    if not draw or "updateLoadStatus()" not in draw:
        bad.append("HIDDEN-PUMP: draw() and pumpBulk() no longer share updateLoadStatus()")


def _check_single_open(fmp, menu, bad):
    notify = _function(fmp, "void LLMeshFilePicker::notify(")
    if not notify or "openFloater()" not in notify:
        bad.append("SINGLE-OPEN: the uploader's file picker no longer shows the uploader after a pick")
    smf = _function(fmp, "void LLFloaterModelPreview::showModelFile(")
    if not smf or "openFloater()" not in smf or "onPreviewClosed(" not in smf:
        bad.append("SINGLE-OPEN: showModelFile must take the uploader over from a bulk run and show it")
    if "LLFloaterModelPreview::showModelFile(filename)" not in menu:
        bad.append("SINGLE-OPEN: a model file opened by type no longer goes through showModelFile")


def _check_one_bar(fmp, repo, bad):
    um = _function(repo, "void LLMeshRepository::uploadModel(")
    if not um or "progress_batch ? progress_batch" not in um:
        bad.append("ONE-BAR: uploadModel does not report into a given progress batch")
    up = _function(fmp, "void LLFloaterModelPreview::onUpload(")
    if not up or not re.search(r"uploadModel\([^;]*progress_batch\)", up, flags=re.S):
        bad.append("ONE-BAR: onUpload does not pass the bulk run's progress batch")


def _check_physics(ctx, batch, fmp, bulk, bad):
    settings = _read(ctx, SETTINGS)
    m = re.search(r"<key>MeshBulkUploadPhysicsFallback</key>\s*<map>(.*?)</map>", settings, flags=re.S)
    if not m:
        bad.append("PHYS-DEFAULT: MeshBulkUploadPhysicsFallback is not in settings.xml")
    else:
        if not re.search(r"<key>Type</key>\s*<string>String</string>", m.group(1)):
            bad.append("PHYS-DEFAULT: MeshBulkUploadPhysicsFallback is not a String setting")
        if not re.search(r"<key>Value</key>\s*<string>bounding_box</string>", m.group(1)):
            bad.append("PHYS-DEFAULT: MeshBulkUploadPhysicsFallback does not default to bounding_box")

    names_m = re.search(r"PHYSICS_FALLBACK_NAMES\[PHYSICS_FALLBACK_COUNT\]\s*=\s*\{([^}]*)\}", bulk)
    names = re.findall(r'"([a-z_]+)"', names_m.group(1)) if names_m else []
    if not names or names[0] != "bounding_box":
        bad.append("PHYS-DEFAULT: the physics fallback names do not start with bounding_box (enum value 0)")
    ffn = _function(bulk, "EPhysicsFallback physicsFallbackFromName(")
    if not ffn or not re.search(r"return PHYSICS_BOUNDING_BOX;\s*\}$", ffn.strip()):
        bad.append("PHYS-DEFAULT: an unknown physics fallback name does not read as bounding box")

    abs_ = _function(fmp, "void LLFloaterModelPreview::applyBulkSettings(")
    if not abs_:
        bad.append("PHYS-DEFAULT: applyBulkSettings not found")
    else:
        if re.search(r"if \(!settings\.isMap\(\)\)\s*\{\s*return;", abs_):
            bad.append("PHYS-DEFAULT: applyBulkSettings returns before the physics fallback when nothing is carried")
        if not re.search(r"if \(!physics_from_file\)\s*\{.*?physicsComboIndex\(.*?onPhysicsUseLOD\(", abs_, flags=re.S):
            bad.append("PHYS-DEFAULT: applyBulkSettings does not apply the physics fallback to a model without a physics mesh")
    if '"physics_combo"' in fmp or '"physics_combo"' in batch:
        bad.append("PHYS-DEFAULT: the physics shape is still carried from the previous model (physics_combo)")
    step = _function(batch, "void LLFloaterMeshBulkUpload::stepCurrent(") or ""
    if "physicsFallbackSetting()" not in step:
        bad.append("PHYS-DEFAULT: the batch does not pass the saved physics fallback to applyBulkSettings")

    # PHYS-COMBO: the uploader's combo order
    items = []
    try:
        root = ET.parse(ctx.path(FMP_XUI)).getroot()
        for combo in root.iter("combo_box"):
            if combo.get("name") == "physics_lod_combo":
                items = [c.get("name") for c in combo if c.tag in ("combo_item", "combo_box.item")]
    except ET.ParseError as e:
        bad.append("PHYS-COMBO: floater_model_preview.xml does not parse: %s" % e)
    if not items:
        bad.append("PHYS-COMBO: physics_lod_combo not found in floater_model_preview.xml")
    else:
        expect = {1: "physics_high", 2: "physics_medium", 3: "physics_low", 4: "physics_lowest",
                  len(items) - 2: "physics_bounding_box", len(items) - 1: "load_from_file"}
        for index, name in expect.items():
            if index >= len(items) or items[index] != name:
                bad.append("PHYS-COMBO: physics_lod_combo item %d is %r, physicsComboIndex expects %r"
                           % (index, items[index] if index < len(items) else None, name))
    pci = _function(bulk, "S32 physicsComboIndex(") or ""
    for case, want in (("PHYSICS_BOUNDING_BOX", "combo_item_count - 2"), ("PHYSICS_LOWEST", "4"),
                       ("PHYSICS_LOW", "3"), ("PHYSICS_MEDIUM", "2")):
        m = re.search(r"case %s:\s*return ([^;]*);" % case, pci)
        if not m or want not in m.group(1):
            bad.append("PHYS-COMBO: physicsComboIndex maps %s to the wrong physics_lod_combo item" % case)

    # PHYS-DROPDOWN
    try:
        root = ET.parse(ctx.path(BATCH_XUI)).getroot()
    except ET.ParseError as e:
        bad.append("PHYS-DROPDOWN: floater_mesh_bulk_upload.xml does not parse: %s" % e)
        return None
    combo = [c for c in root.iter("combo_box") if c.get("name") == "physics_fallback_combo"]
    if not combo:
        bad.append("PHYS-DROPDOWN: physics_fallback_combo is not in floater_mesh_bulk_upload.xml")
    else:
        values = [c.get("value") for c in combo[0] if c.tag in ("combo_item", "combo_box.item")]
        if not values or values[0] != "bounding_box":
            bad.append("PHYS-DROPDOWN: the physics fallback dropdown does not list bounding box first")
        if names and sorted(values) != sorted(names):
            bad.append("PHYS-DROPDOWN: dropdown values %s differ from LLMeshBulk's names %s" % (values, names))
    return root


def _check_auto(ctx, root, batch, bad):
    if root is not None:
        children = list(root)
        names = [c.get("name") for c in children]
        if "auto_confirm_check" not in names:
            bad.append("AUTO-CONFIRM: auto_confirm_check is not in floater_mesh_bulk_upload.xml")
        else:
            check = children[names.index("auto_confirm_check")]
            if check.get("initial_value", "false") != "false":
                bad.append("AUTO-CONFIRM: auto_confirm_check does not start unchecked")
            if check.get("control_name"):
                bad.append("AUTO-CONFIRM: auto_confirm_check is bound to a saved setting (it spends L$ without asking)")
            if "upload_all_btn" not in names or abs(names.index("auto_confirm_check") - names.index("upload_all_btn")) > 3:
                bad.append("AUTO-CONFIRM: auto_confirm_check is not next to upload_all_btn")
    if re.search(r"<key>[^<]*AutoConfirm[^<]*</key>", _read(ctx, SETTINGS)):
        bad.append("AUTO-CONFIRM: an auto-confirm setting is saved (it must reset every time)")
    on_open = _function(batch, "void LLFloaterMeshBulkUpload::onOpen(") or ""
    if "mAutoConfirmCheck->set(false)" not in on_open:
        bad.append("AUTO-CONFIRM: opening the batch does not reset Auto-confirm to off")

    upload_all = _function(batch, "void LLFloaterMeshBulkUpload::onUploadAll(") or ""
    if not re.search(r"if \(mAutoConfirmCheck->get\(\)\)\s*\{\s*startAutoUpload\(\);\s*return;", upload_all):
        bad.append("AUTO-CONFIRM: Upload all remaining does not start the auto run when Auto-confirm is checked")
    auto = _function(batch, "void LLFloaterMeshBulkUpload::startAutoUpload(") or ""
    if "MeshBulkUploadConfirm" in auto or "RUN_AUTO_UPLOAD" not in auto or "mInRun = true" not in auto:
        bad.append("AUTO-CONFIRM: startAutoUpload must mark the run's models and start without the confirmation")

    # RUN-MARKS-KEPT (2026-11-07 owner test: Auto-confirm "Upload all remaining" did nothing):
    # beginProgress() called endProgress(), which cleared the mInRun marks just set, so advance()
    # found no model and the run ended at once. Nothing a run's start calls between marking its
    # models and advance() may clear the marks.
    for starter in ("void LLFloaterMeshBulkUpload::startAutoUpload(", "void LLFloaterMeshBulkUpload::onConfirmUploadAll("):
        body = _function(batch, starter) or ""
        mark = body.rfind("mInRun = true")
        go = body.find("advance()", mark)
        if mark < 0 or go < 0:
            bad.append("RUN-MARKS-KEPT: %s) does not mark its models and then advance()" % starter)
            continue
        for callee in re.findall(r"\b([A-Za-z_]\w*)\(\)", body[mark:go]):
            if callee == "endProgress":
                bad.append("RUN-MARKS-KEPT: %s) ends the run (endProgress) after marking its models" % starter)
                continue
            callee_body = _function(batch, "LLFloaterMeshBulkUpload::%s(" % callee)
            if callee_body and ("endProgress(" in callee_body or "mInRun = false" in callee_body):
                bad.append("RUN-MARKS-KEPT: %s) calls %s(), which clears the run's marks before advance() "
                           "(the run finds nothing to upload)" % (starter, callee))
    bp = _function(batch, "void LLFloaterMeshBulkUpload::beginProgress(") or ""
    if "endProgress(" in bp or "mInRun = false" in bp:
        bad.append("RUN-MARKS-KEPT: beginProgress clears the run's marks")

    # NO-SILENT-RETURN: every early return on the auto path says why in the batch log.
    for sig in ("void LLFloaterMeshBulkUpload::onUploadAll(", "void LLFloaterMeshBulkUpload::autoUploadCurrent(",
                "void LLFloaterMeshBulkUpload::startAutoUpload(", "void LLFloaterMeshBulkUpload::startEntry("):
        body = _function(batch, sig) or ""
        for m in re.finditer(r"\{([^{}]*)\breturn;", body):
            block = m.group(1)
            if not re.search(r"\b(log|stopRun|onEntryNotReady|advance|skipInProgress|startAutoUpload|uploadNow)\(", block):
                bad.append("NO-SILENT-RETURN: %s) returns without a log line: %r" % (sig, block.strip()[:80]))

    nxt = _function(batch, "S32 LLFloaterMeshBulkUpload::findNextEntry(") or ""
    if not re.search(r"if \(isUploadRun\(\) && !entry\.mInRun\)\s*\{\s*continue;", nxt):
        bad.append("AUTO-CONFIRM: findNextEntry may pick a model that already left the run (sent twice)")
    req = _function(batch, "bool LLFloaterMeshBulkUpload::onPreviewUploadRequested(") or ""
    if not re.search(r"entry\.mInRun = false;\s*self->mStep = STEP_WAIT_UPLOAD;", req):
        bad.append("AUTO-CONFIRM: a sent model does not leave the run")

    done = _function(batch, "void LLFloaterMeshBulkUpload::onPreviewUploadDone(") or ""
    fail = done[done.find("else"):] if "else" in done else ""
    m = re.search(r"if \(self->mRunMode == RUN_AUTO_UPLOAD\)\s*\{(.*?)\}\s*else", fail, flags=re.S)
    if not m or "advance()" not in m.group(1) or "RUN_MANUAL" in m.group(1):
        bad.append("AUTO-CONFIRM: a failed upload in auto mode stops the run instead of going on with the others")
    if m and "uploadNow(" in m.group(1):
        bad.append("AUTO-CONFIRM: a failed upload is retried")
    err = _function(batch, "void LLFloaterMeshBulkUpload::onPreviewFeeError(") or ""
    if not re.search(r"RUN_AUTO_UPLOAD\)\s*\{[^}]*skipInProgress\([^}]*advance\(\);", err, flags=re.S):
        bad.append("AUTO-CONFIRM: a failed fee in auto mode is not reported and skipped")

    auc = _function(batch, "void LLFloaterMeshBulkUpload::autoUploadCurrent(") or ""
    if not re.search(r"if \(!fmp->isUploadPermissionKnown\(\)\)\s*\{.*?mAutoAwaitPerm = true;.*?return;", auc, flags=re.S):
        bad.append("AUTO-CONFIRM: auto mode decides before the hidden uploader's upload permission is known")
    if "can_afford_transaction(" not in auc:
        bad.append("AUTO-CONFIRM: auto mode uploads without checking the L$ balance for the fee")

    calls = len(re.findall(r"uploadNow\(\)", batch))
    if calls != 3:
        bad.append("AUTO-CONFIRM: uploadNow() is called %d times in the batch (expected 3: upload-all, auto, folder "
                   "callback); a new call site may send a model twice" % calls)


def run(ctx, result):
    batch = _strip_comments(_read(ctx, BATCH))
    fmp = _strip_comments(_read(ctx, FMP))
    mp = _strip_comments(_read(ctx, MP))
    bulk = _strip_comments(_read(ctx, BULK))
    menu = _strip_comments(_read(ctx, MENU))
    repo = _strip_comments(_read(ctx, REPO))
    bad = []

    _check_quiet(batch, mp, bad)
    _check_pump(batch, fmp, bad)
    _check_single_open(fmp, menu, bad)
    _check_one_bar(fmp, repo, bad)
    root = _check_physics(ctx, batch, fmp, bulk, bad)
    _check_auto(ctx, root, batch, bad)

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = ("uploader hidden during runs (pumped, never focused); bounding-box physics fallback with "
                          "dropdown; auto-confirm sends each model once and goes on after failures")
