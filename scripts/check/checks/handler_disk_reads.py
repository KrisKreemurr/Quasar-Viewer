"""Source guards: message handlers never read the disk on the main thread (2026-11-08).

The ~10 ms SoundTrigger / AttachedSound / AvatarAnimation / ObjectAnimation handlers
(netHandlerTiming) were the main thread blocked in the kernel (state D, folio_wait_bit_common /
read_extent_buffer_pages) for ~9.5 ms per cold read: a decoded sound file (.dsf) read when a
sound started, or a cached animation read when a motion was created. Sampling the main thread's
/proc state matched every slow handler line to one such wait. These guards keep the reads on a
General worker:
  SOUND-LOAD      LLAudioData::load() posts the decoded file's read to the General queue and
                  reads no file itself; the OpenAL buffer is made from the bytes
                  (loadWAVImage, no LLFile call).
  SOUND-TOUCH     LLSoundCache::touch() posts the mtime write (last_write_time) to a worker.
  ANIM-LOAD       LLKeyframeMotion::onInitialize() / onLoadComplete() open no LLFileSystem;
                  readAssetAsync() reads the asset on the General queue.
  STAND-HOLD      LLKeyframeStandMotion::onInitialize() returns HOLD while the data loads
                  (looking for joint states then failed the motion for the session).
Putting a read back on the main thread fails this check.
"""
import re

NAME = "handler-disk-reads"
STAGE = "source"
DESCRIPTION = "message handlers never read the disk on the main thread (sound / animation loads)"
GUARDS = ("~10 ms SoundTrigger / AttachedSound / AvatarAnimation handlers (2026-11-08): main-thread "
          "disk waits reading decoded sounds and cached animations.")

AUDIO = "indra/llaudio/llaudioengine.cpp"
OPENAL = "indra/llaudio/llaudioengine_openal.cpp"
SOUNDCACHE = "indra/llaudio/llsoundcache.cpp"
KEYFRAME = "indra/llcharacter/llkeyframemotion.cpp"
STAND = "indra/llcharacter/llkeyframestandmotion.cpp"


def body(text, signature):
    """The body of the function whose definition starts with `signature` (to the first
    closing brace in column 0), or ''."""
    i = text.find(signature)
    if i < 0:
        return ""
    j = text.find("\n}", i)
    return text[i:j + 2] if j >= 0 else ""


def run(ctx, result):
    read = lambda p: open(ctx.path(p), encoding="utf-8").read()
    audio, openal, cache, keyframe, stand = (read(p) for p in (AUDIO, OPENAL, SOUNDCACHE, KEYFRAME, STAND))
    bad = []

    load = body(audio, "bool LLAudioData::load()")
    if not load:
        bad.append("SOUND-LOAD: LLAudioData::load() not found")
    else:
        if not re.search(r'getInstance\("General"\)', load) or "postTo(" not in load:
            bad.append("SOUND-LOAD: LLAudioData::load() does not post the file read to the General queue")
        if re.search(r"loadWAV\(\s*wav_path|->loadWAV\(", load):
            bad.append("SOUND-LOAD: LLAudioData::load() loads the file into the buffer itself (main-thread read)")
    if re.search(r"bool\s+LLAudioBufferOpenAL::loadWAV\(\s*const std::string&\s*filename", openal):
        bad.append("SOUND-LOAD: the OpenAL buffer still loads from a file name")
    img = body(openal, "bool LLAudioBufferOpenAL::loadWAVImage(")
    if not img:
        bad.append("SOUND-LOAD: LLAudioBufferOpenAL::loadWAVImage() not found")
    elif "LLFile::" in img:
        bad.append("SOUND-LOAD: loadWAVImage() touches the disk (LLFile)")

    touch = body(cache, "void LLSoundCache::touch(")
    if not touch:
        bad.append("SOUND-TOUCH: LLSoundCache::touch() not found")
    elif "last_write_time" in touch and not re.search(r'getInstance\("General"\)', touch):
        bad.append("SOUND-TOUCH: LLSoundCache::touch() writes the file's mtime on the main thread")

    init = body(keyframe, "LLMotion::LLMotionInitStatus LLKeyframeMotion::onInitialize(")
    done = body(keyframe, "void LLKeyframeMotion::onLoadComplete(")
    for name, fn in (("onInitialize", init), ("onLoadComplete", done)):
        if not fn:
            bad.append("ANIM-LOAD: LLKeyframeMotion::%s() not found" % name)
        elif "LLFileSystem" in fn:
            bad.append("ANIM-LOAD: LLKeyframeMotion::%s() reads the cached asset on the main thread" % name)
        elif "readAssetAsync(" not in fn:
            bad.append("ANIM-LOAD: LLKeyframeMotion::%s() does not hand the read to readAssetAsync()" % name)
    async_read = body(keyframe, "void LLKeyframeMotion::readAssetAsync(")
    if not async_read or not re.search(r'getInstance\("General"\)', async_read) or "postTo(" not in async_read:
        bad.append("ANIM-LOAD: readAssetAsync() does not read on the General queue")

    sinit = body(stand, "LLMotion::LLMotionInitStatus LLKeyframeStandMotion::onInitialize(")
    if not re.search(r"status\s*!=\s*STATUS_SUCCESS\s*\)\s*\{[^}]*return status;", sinit):
        bad.append("STAND-HOLD: LLKeyframeStandMotion::onInitialize() does not wait out HOLD")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "sound and animation loads read on a worker"
