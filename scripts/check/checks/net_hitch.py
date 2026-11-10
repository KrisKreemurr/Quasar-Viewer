"""Source guards for the network frame-hitch attribution (2026-11-07, hitch class A).

Frames of ~25 ms showed LL_idleNetwork at ~10 ms where a normal frame spends ~0.2 ms, with no
finer section to say where. These guards keep the attribution that the next log needs:
  SCOPE-ONLY-NETWORK  LLAppViewer::idle: the LL_idleNetwork section times idleNetwork() alone;
                      the AFK check and update_statistics() have their own sections (the old
                      block-scoped section also covered them, so their spikes read as network).
  HANDLER-TIMING      idleNetwork installs the per-message handler timing callback
                      (setTimingFunc: net_dispatch, a >2 ms handler named in the HITCH line and a
                      VKHitch line) and the processAcks step timing (setSectionTimingFunc).
  LOOP-SECTIONS       the reader lock, the UDP dispatch, the HTTP pipe pump / callback, the
                      post-loop work, processAcks and the peer continuations are separate sections.
Removing one fails this check.
"""
import re

NAME = "net-hitch"
STAGE = "source"
DESCRIPTION = "network frame-hitch attribution (idleNetwork sections, per-message handler timing)"
GUARDS = ("~10 ms LL_idleNetwork hitches (2026-11-07) that the log could not attribute to a handler, "
          "the reader lock, the HTTP pump or processAcks.")

APP = "indra/newview/llappviewer.cpp"
MSG = "indra/llmessage/message.cpp"


def run(ctx, result):
    app = open(ctx.path(APP), encoding="utf-8").read()
    msg = open(ctx.path(MSG), encoding="utf-8").read()
    bad = []

    if not re.search(r'\{\s*VK_HITCH_SCOPE\("LL_idleNetwork"\);\s*idleNetwork\(\);\s*\}', app):
        bad.append('SCOPE-ONLY-NETWORK: LL_idleNetwork is not a section of idleNetwork() alone')
    for name, call in (("LL_afkCheck", "idle_afk_check()"), ("LL_updateStatistics", "update_statistics()")):
        if not re.search(r'\{\s*VK_HITCH_SCOPE\("%s"\);\s*%s;\s*\}' % (name, re.escape(call)), app):
            bad.append("SCOPE-ONLY-NETWORK: %s is not timed in its own section (%s)" % (call, name))

    m = re.search(r"void LLAppViewer::idleNetwork\(\)\n\{.*?\n\}\n", app, flags=re.S)
    body = m.group(0) if m else ""
    if not body:
        bad.append("idleNetwork not found in llappviewer.cpp")
    if "setTimingFunc(&netHandlerTiming)" not in body:
        bad.append("HANDLER-TIMING: idleNetwork does not install the per-message handler timing callback")
    if "setSectionTimingFunc(&netSectionTiming)" not in body:
        bad.append("HANDLER-TIMING: idleNetwork does not install the processAcks step timing")
    if not re.search(r'void netHandlerTiming\(.*?kSlowHandlerMs.*?LLVKHitch::add\(name, ms', app, flags=re.S):
        bad.append("HANDLER-TIMING: a slow handler no longer gets its own (message-named) section")
    for section in ("net_readerLock", "net_checkMessages", "net_httpPump", "net_httpCallback", "net_postLoop",
                    "net_acks", "net_peerContinuations"):
        if '"%s"' % section not in body:
            bad.append("LOOP-SECTIONS: idleNetwork lacks the %s section" % section)
    for section in ("msg_transfers", "msg_sendCompletions", "msg_circuitTimeouts"):
        if 'SectionTime t(timing, "%s")' % section not in msg:
            bad.append("LOOP-SECTIONS: processAcks does not time %s" % section)

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "network sections and handler timing present"
