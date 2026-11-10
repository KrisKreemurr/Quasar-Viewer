"""Source guards: desktop click picks never wait for the GPU on the main thread (2026-11-07,
hitch class B: vk_performPick / pick_intersectAll ~10.7 ms, df_gatherInput ~13 ms).

A GPU pick answer asked for between frames (askNow -> pickNow -> submitImmediate) waits for
the frames in flight: ~7-15 ms on the main thread. Clicks used to do that whenever the newest
per-frame answer did not cover their ray (LLToolPie's two pickImmediate per press while the
camera or cursor moved: "pick time elapsed 0.015 s") and for every depth-window page of a
click's walk (crowds: overflowing candidate / fragment lists, the held async click's fetch).
  PRESS-HOLD        LLWindowSDL::gatherInput asks holdMouseDown before handling a button press
                    and leaves it (and what follows it) queued while it is held;
                    LLViewerWindow::holdMouseDown holds world presses through holdClick.
  HELD-ANSWER       answerFor serves a held click its own answer before anything else.
  PAGES-PREFETCHED  walkLayer gets pages from pageAnswer, never askNow; pageAnswer serves a
                    held click from the pages its hold fetched and returns before askNow;
                    fillRequest asks for the held click's next page.
  NO-HOVER-PAGE     every walkLayer call pages only on a click (exact && click).
  ANSWER-NOW-SITES  askNow is called from answerFor and pageAnswer's unheld tail only (clicks
                    that could not be held: VR, synthetic presses, a hold that ran out).
"""
import re

NAME = "pick-hitch"
STAGE = "source"
DESCRIPTION = "desktop click picks take held / prefetched GPU answers, never a main-thread GPU wait"
GUARDS = ("~10-15 ms main-thread GPU waits on clicks: answer-now picks in mouse-down handlers and depth-window "
          "pages of held clicks (2026-11-07 vk_performPick / df_gatherInput hitches).")

PICK = "indra/newview/llcleancursorpick.cpp"
SDL = "indra/llwindow/llwindowsdl.cpp"
WIN = "indra/newview/llviewerwindow.cpp"


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


def run(ctx, result):
    pick = _strip_comments(open(ctx.path(PICK), encoding="utf-8").read())
    sdl = _strip_comments(open(ctx.path(SDL), encoding="utf-8").read())
    win = _strip_comments(open(ctx.path(WIN), encoding="utf-8").read())
    bad = []

    # PRESS-HOLD
    gather = _function(sdl, "void LLWindowSDL::gatherInput(")
    if not gather or not re.search(r"while \(!held && SDL_PollEvent\(&event\)\)\s*\{\s*if \(holdPress\(event\)\)", gather):
        bad.append("PRESS-HOLD: LLWindowSDL::gatherInput handles events without asking holdPress first")
    hp = _function(sdl, "bool LLWindowSDL::holdPress(")
    if not hp or "SDL_EVENT_MOUSE_BUTTON_DOWN" not in hp or "holdMouseDown(" not in hp:
        bad.append("PRESS-HOLD: holdPress does not ask the callbacks' holdMouseDown for button presses")
    hm = _function(win, "bool LLViewerWindow::holdMouseDown(")
    if not hm or "LLCleanCursorPick::holdClick(" not in hm:
        bad.append("PRESS-HOLD: LLViewerWindow::holdMouseDown does not hold world presses through holdClick")

    # HELD-ANSWER
    af = _function(pick, "bool answerFor(")
    if not af or not re.search(r"heldReadyFor\(q\.x, q\.y\).*?r = sHeld\.answer;.*?latestPick", af, flags=re.S):
        bad.append("HELD-ANSWER: answerFor does not serve a held click its own answer first")

    # PAGES-PREFETCHED
    wl = _function(pick, "bool walkLayer(")
    if not wl:
        bad.append("walkLayer not found in llcleancursorpick.cpp")
    else:
        if "askNow(" in wl:
            bad.append("PAGES-PREFETCHED: walkLayer answers a page now (askNow) instead of through pageAnswer")
        if "pageAnswer(" not in wl:
            bad.append("PAGES-PREFETCHED: walkLayer no longer gets its pages from pageAnswer")
    pa = _function(pick, "bool pageAnswer(")
    if not pa or not re.search(r"if \(heldReadyFor\(q\.mouse_x, q\.mouse_y\)\)\s*\{.*?sHeld\.pages.*?return false;\s*\}", pa,
                               flags=re.S):
        bad.append("PAGES-PREFETCHED: pageAnswer does not serve a held click from its fetched pages without waiting")
    fr = _function(pick, "void fillRequest(")
    if not fr or not re.search(r"if \(held && sHeld\.answered\)\s*\{.*?r = sHeld\.want;", fr, flags=re.S):
        bad.append("PAGES-PREFETCHED: fillRequest does not ask for the held click's next page")
    hc = _function(pick, "bool holdClick(")
    if not hc or "sHeld.pages.push_back(" not in hc or "advanceChain()" not in hc:
        bad.append("PAGES-PREFETCHED: holdClick does not collect the held click's pages")

    # NO-HOVER-PAGE
    calls = re.findall(r"walkLayer\(\s*(?:true|false|hud)\s*,\s*r\s*,\s*([^,]+),", pick)
    if not calls:
        bad.append("NO-HOVER-PAGE: no walkLayer calls found; update this check")
    for arg in calls:
        arg = arg.strip()
        if arg == "mayPage":
            if not re.search(r"const bool mayPage = exact && click;", pick):
                bad.append("NO-HOVER-PAGE: intersectAll's mayPage is no longer exact && click")
        elif arg != "exact && click":
            bad.append("NO-HOVER-PAGE: a walkLayer call pages on %r (hover picks must never page)" % arg)

    # ANSWER-NOW-SITES
    sites = []
    for m in re.finditer(r"(?<![A-Za-z_])askNow\(", pick):
        line_start = pick.rfind("\n", 0, m.start()) + 1
        line = pick[line_start:pick.find("\n", m.start())]
        if line.lstrip().startswith("bool askNow("):
            continue
        # the enclosing top-level function
        head = None
        for f in re.finditer(r"\n(?:bool|void|Source|Focus|SidePickState)\s+([A-Za-z_]+)\(", pick[:m.start()]):
            head = f.group(1)
        sites.append(head)
    for head in sites:
        if head not in ("answerFor", "pageAnswer"):
            bad.append("ANSWER-NOW-SITES: %s answers a pick now (a main-thread GPU wait)" % head)
    if sites.count("pageAnswer") > 1 or sites.count("answerFor") > 1:
        bad.append("ANSWER-NOW-SITES: more than one answer-now site in answerFor / pageAnswer")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "press hold, held answers and prefetched pages present; answer-now only for unheld clicks"
