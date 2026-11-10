/**
 * @file slpanelsoundplayer.cpp
 * @brief The sound preview player (see slpanelsoundplayer.h)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slpanelsoundplayer.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slpanelsoundplayer.h"

#include "llaudioengine.h"
#include "llsoundpreviewlimits.h"
#include "lltimer.h"
#include "llvieweraudio.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "lltextbox.h"
#include "slassetblocklist.h"
#include "slsoundscrub.h"
#include "slsoundscrubbar.h"

#include <limits>
#include <new>

static LLPanelInjector<SLPanelSoundPlayer> t_sound_player("sound_player");

namespace
{
    // The player playing, if any: one local preview at a time.
    LLHandle<SLPanelSoundPlayer> sPlaying;
    U64 sPCMBytes = 0, sPCMBytesPeak = 0;
    U32 sPCMCount = 0, sPCMCountPeak = 0;
}

SLPanelSoundPlayer::SLPanelSoundPlayer()
:   LLPanel()
{
}

SLPanelSoundPlayer::~SLPanelSoundPlayer()
{
    mEngineConn.disconnect();
    if (mIdling)
    {
        gIdleCallbacks.deleteFunction(&SLPanelSoundPlayer::onIdle, this);
    }
    destroyClip();
    releaseRequest();
    releasePCM();
}

bool SLPanelSoundPlayer::postBuild()
{
    mBar = getChild<SLSoundScrubBar>("scrub_bar");
    mPlayBtn = getChild<LLButton>("play_btn");
    mTimeText = getChild<LLTextBox>("time_text");
    mLoopCheck = getChild<LLCheckBoxCtrl>("loop_check");

    mPlayBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlayPause(); });
    mBar->setCommitCallback([this](LLUICtrl*, const LLSD& value) { onScrub((F32)value.asReal()); });
    mLoopCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLoop(); });

    mBlocklistConn = SLAssetBlocklist::getInstance()->setChangeCallback([this]() { onBlocklistChanged(); });
    try
    {
        mEngineConn = audio_on_engine_changed([this]()
        {
            forgetStaleEngine();
            // Dispatch owns allocation-failure retry; do not hide a failed refresh
            // before it has reattached loading idle work.
            refreshState(); // rearm loading, or enable retained PCM without autoplay
        });
    }
    catch (const std::bad_alloc&)
    {
        LL_WARNS("SoundPreview") << "Audio availability observer allocation refused" << LL_ENDL;
    }

    mPlayLabel = getString("play_file");
    mPauseLabel = getString("pause");
    refreshState();
    return LLPanel::postBuild();
}

void SLPanelSoundPlayer::setSound(const LLUUID& asset_id)
{
    if (asset_id.notNull() && asset_id == mAssetID)
    {
        forgetStaleEngine();
        refreshState();
        return;
    }
    reset();
    mAssetID = asset_id;
    mPlayLabel = getString("play_sound");
    mPlayBtn->setToolTip(getString("play_sound_tip"));
    if (mAssetID.notNull())
    {
        pollAsset();    // (already decoded: ready at once; else requested now)
    }
    refreshState();
}

void SLPanelSoundPlayer::setPCM(std::shared_ptr<const std::vector<S16>> pcm, U32 channels, U32 rate)
{
    reset();
    mPlayLabel = getString("play_file");
    mPlayBtn->setToolTip(getString("play_file_tip"));
    adoptPCM(std::move(pcm), channels, rate);
    refreshState();
}

void SLPanelSoundPlayer::reset()
{
    destroyClip();
    releaseRequest();
    mAssetID.setNull();
    releasePCM();
    mChannels = 1;
    mRate = 0;
    mFrames = 0;
    mCursor = 0;
    mClipFailed = false;
    mNextLoadRetry = 0;
}

void SLPanelSoundPlayer::releasePCM()
{
    if (mPCM)
    {
        sPCMBytes -= mPCMBytes;
        --sPCMCount;
    }
    mPCMBytes = 0;
    mPCM.reset();
    if (mBar) mBar->setPCM(nullptr, 1);
}

bool SLPanelSoundPlayer::adoptPCM(std::shared_ptr<const std::vector<S16>> pcm, U32 channels, U32 rate)
{
    std::uint64_t bytes = 0;
    if (!pcm || !LLSoundPreviewLimits::retainedBytes(pcm->size(), pcm->capacity(), channels, rate, bytes))
        return false;
    const size_t frames = pcm->size() / channels;
    if ((!mPCM && sPCMCount >= LLSoundPreviewLimits::REQUESTS) ||
        !LLSoundPreviewLimits::fits(sPCMBytes, mPCMBytes, bytes)) return false;
    sPCMBytes = sPCMBytes - mPCMBytes + bytes;
    if (!mPCM) ++sPCMCount;
    mPCMBytes = bytes;
    mPCM = std::move(pcm);
    mChannels = channels;
    mRate = rate;
    mFrames = (U32)frames;
    mCursor = 0;
    mBar->setPCM(mPCM, mChannels);
    if (sPCMCount > sPCMCountPeak || sPCMBytes > sPCMBytesPeak)
    {
        const bool new_megabyte = (sPCMBytes >> 20) > (sPCMBytesPeak >> 20);
        sPCMCountPeak = (std::max)(sPCMCountPeak, sPCMCount);
        sPCMBytesPeak = (std::max)(sPCMBytesPeak, sPCMBytes);
        if (new_megabyte || (sPCMCountPeak & (sPCMCountPeak - 1)) == 0)
        {
            try
            {
                LL_DEBUGS("SoundPreview") << "pcm_panels_peak=" << sPCMCountPeak << " pcm_bytes_peak=" << sPCMBytesPeak << LL_ENDL;
            }
            catch (const std::bad_alloc&) {} // diagnostics cannot invalidate admitted PCM
        }
    }
    return true;
}

// static
void SLPanelSoundPlayer::onIdle(void* self)
{
    static_cast<SLPanelSoundPlayer*>(self)->idle();
}

void SLPanelSoundPlayer::idle()
{
    forgetStaleEngine();
    if (!mPCM && mAssetID.notNull())
    {
        pollAsset();
    }
    if (mPlaying)
    {
        followClip();
    }
    refreshState();
}

bool SLPanelSoundPlayer::hasWork() const
{
    if (mPlaying)
    {
        return true;
    }
    // A bad mark expires and the engine tries the sound again: keep looking.
    const EState state = getState();
    return state == LOADING || state == UNAVAILABLE;
}

void SLPanelSoundPlayer::refreshState()
{
    updateControls();
    const bool work = hasWork();
    if (work != mIdling)
    {
        if (work)
        {
            gIdleCallbacks.addFunction(&SLPanelSoundPlayer::onIdle, this);
        }
        else
        {
            gIdleCallbacks.deleteFunction(&SLPanelSoundPlayer::onIdle, this);
        }
        mIdling = work;
    }
}

void SLPanelSoundPlayer::forgetStaleEngine()
{
    // An engine that was replaced or shut down took our clip and request with it: forget both
    // (never hand its ids to another engine). The clip is remade on the next play.
    if (mClipEngine && mClipEngine != gAudiop)
    {
        mClipEngine = nullptr;
        mClip = 0;
        mPlaying = false;
    }
    if (mRequestEngine && mRequestEngine != gAudiop)
    {
        mRequestEngine = nullptr;
    }
}

void SLPanelSoundPlayer::onBlocklistChanged()
{
    if (isBlocked())
    {
        pause();
        releaseRequest();   // (a blocked sound is neither fetched nor decoded)
    }
    refreshState();         // (unblocked while loading: the idle poll requests it again)
}

void SLPanelSoundPlayer::onVisibilityChange(bool new_visibility)
{
    if (!new_visibility && mPlaying)
    {
        pause();    // closed, minimised or a hidden tab: it does not play on unseen
        refreshState();
    }
    else if (new_visibility && mPlayBtn)
    {
        forgetStaleEngine();
        refreshState();
    }
    LLPanel::onVisibilityChange(new_visibility);
}

void SLPanelSoundPlayer::pollAsset()
{
    const F64 now = LLTimer::getTotalSeconds();
    if (now < mNextLoadRetry) return;
    LLAudioEngine* engine = gAudiop;
    if (!engine || isBlocked())
    {
        releaseRequest();   // (a blocked sound is neither fetched nor decoded)
        return;
    }
    if (!mRequestEngine)
    {
        if (!engine->requestSound(mAssetID))
        {
            mNextLoadRetry = now + 1.0;
            return;
        }
        mRequestEngine = engine;
    }
    if (sPCMCount >= LLSoundPreviewLimits::REQUESTS || sPCMBytes >= LLSoundPreviewLimits::PCM_BYTES)
    {
        mNextLoadRetry = now + 1.0;
        return;
    }
    std::vector<S16> samples;
    U32 channels = 0, rate = 0;
    if (engine->readDecodedSound(mAssetID, samples, channels, rate, LLSoundPreviewLimits::PCM_BYTES - sPCMBytes))
    {
        releaseRequest();   // the PCM is ours now: the engine need not keep the sound
        try
        {
            if (!adoptPCM(std::make_shared<const std::vector<S16>>(std::move(samples)), channels, rate))
                mNextLoadRetry = now + 1.0;
        }
        catch (const std::bad_alloc&)
        {
            mNextLoadRetry = now + 5.0;
            LL_WARNS("SoundPreview") << "Preview PCM allocation refused" << LL_ENDL;
        }
    }
    else
    {
        mNextLoadRetry = now + 1.0;
    }
}

SLPanelSoundPlayer::EState SLPanelSoundPlayer::getState() const
{
    if (!mPCM && mAssetID.isNull())
    {
        return NO_SOUND;
    }
    if (!gAudiop)
    {
        return NO_AUDIO;
    }
    if (isBlocked())
    {
        return BLOCKED;
    }
    if (mPCM)
    {
        return READY;
    }
    return gAudiop->isCorruptSound(mAssetID) ? UNAVAILABLE : LOADING;
}

bool SLPanelSoundPlayer::isBlocked() const
{
    return LLAudioEngine::isSoundBlocked(mAssetID);
}

void SLPanelSoundPlayer::play()
{
    forgetStaleEngine();
    LLAudioEngine* engine = gAudiop;
    if (!engine || getState() != READY)
    {
        return;
    }
    if (!mClip)
    {
        mClip = engine->createLocalClip(mPCM->data(), mFrames, mChannels, mRate);
        if (!mClip)
        {
            // (Out of OpenAL memory, most likely: each press tries again.)
            if (!mClipFailed)
            {
                LL_WARNS("SoundPreview") << "The audio engine refused a clip of " << mFrames << " frames, "
                                         << mChannels << " channel(s) at " << mRate << " Hz" << LL_ENDL;
                mClipFailed = true;
            }
            return;
        }
        mClipEngine = engine;
    }

    SLPanelSoundPlayer* other = sPlaying.get();
    if (other && other != this)
    {
        other->pause();
        other->refreshState();
    }
    sPlaying = getDerivedHandle<SLPanelSoundPlayer>();

    engine->seekLocalClip(mClip, mCursor);
    engine->setLocalClip(mClip, true, mLoopCheck->get(), gain());
    mPlaying = true;
}

void SLPanelSoundPlayer::pause()
{
    if (!mPlaying)
    {
        return;
    }
    mPlaying = false;
    if (LLAudioEngine* engine = clipEngine())
    {
        const S32 pos = engine->localClipPosition(mClip);
        if (pos >= 0)
        {
            mCursor = (U32)pos;
        }
        engine->setLocalClip(mClip, false, mLoopCheck->get(), gain());
    }
}

void SLPanelSoundPlayer::followClip()
{
    LLAudioEngine* engine = clipEngine();
    if (!engine)
    {
        pause();
        return;
    }
    // Asked first: a clip that played to its end rewinds, and the position then says so.
    const bool playing = engine->localClipPlaying(mClip);
    const S32 pos = engine->localClipPosition(mClip);
    if (pos >= 0)
    {
        mCursor = (U32)pos;
    }
    if (!playing)
    {
        mPlaying = false;
        return;
    }
    engine->setLocalClip(mClip, true, mLoopCheck->get(), gain());   // (follows the volume settings)
}

void SLPanelSoundPlayer::onPlayPause()
{
    if (mPlaying)
    {
        pause();
    }
    else
    {
        play();
    }
    refreshState();
}

void SLPanelSoundPlayer::onScrub(F32 fraction)
{
    mCursor = SLSoundScrub::frameAt(fraction, mFrames);
    LLAudioEngine* engine = clipEngine();
    if (mPlaying && engine)
    {
        engine->seekLocalClip(mClip, mCursor);
    }
    refreshState();
}

void SLPanelSoundPlayer::onLoop()
{
    LLAudioEngine* engine = clipEngine();
    if (mPlaying && engine)
    {
        engine->setLocalClip(mClip, true, mLoopCheck->get(), gain());
    }
}

void SLPanelSoundPlayer::updateControls()
{
    const EState state = getState();
    const bool ready = state == READY;
    mPlayBtn->setEnabled(ready);
    mBar->setEnabled(ready);
    mLoopCheck->setEnabled(ready);

    const std::string& label = mPlaying ? mPauseLabel : mPlayLabel;
    if (label != mShownLabel)
    {
        mPlayBtn->setLabel(label);
        mShownLabel = label;
        onRetainedDirty();      // (LLButton::setLabel does not report)
    }

    std::string text;
    switch (state)
    {
    case NO_AUDIO:      text = getString("no_audio"); break;
    case BLOCKED:       text = getString("blocked"); break;
    case LOADING:       text = getString("loading"); break;
    case UNAVAILABLE:   text = getString("unavailable"); break;
    case READY:
    {
        // While the playhead is dragged: where letting go will seek to.
        const U32 frame = mBar->isDragging() ? SLSoundScrub::frameAt(mBar->getPlayhead(), mFrames) : mCursor;
        LLStringUtil::format_map_t args;
        args["[POSITION]"] = SLSoundScrub::formatTime(frame, mRate);
        args["[LENGTH]"] = SLSoundScrub::formatTime(mFrames, mRate);
        text = getString("time", args);
        break;
    }
    case NO_SOUND:
        break;
    }
    if (text != mShownTime)
    {
        mTimeText->setText(text);
        mShownTime = text;
    }

    mBar->setPlayhead(SLSoundScrub::fractionAt(mCursor, mFrames));
}

LLAudioEngine* SLPanelSoundPlayer::clipEngine() const
{
    return (mClip && mClipEngine == gAudiop) ? gAudiop : nullptr;
}

void SLPanelSoundPlayer::releaseRequest()
{
    if (mRequestEngine && mRequestEngine == gAudiop)
    {
        gAudiop->releaseSound(mAssetID);
    }
    mRequestEngine = nullptr;
}

void SLPanelSoundPlayer::destroyClip()
{
    if (LLAudioEngine* engine = clipEngine())
    {
        engine->destroyLocalLoop(mClip);    // (frees a clip too)
    }
    mClipEngine = nullptr;
    mClip = 0;
    mPlaying = false;
}

// static
F32 SLPanelSoundPlayer::gain()
{
    // The Sound Effects level (0 while Sound Effects is muted), as Play Locally has always used;
    // the master volume and mute are the engine's listener gain.
    return gAudiop ? gAudiop->getSecondaryGain(LLAudioEngine::AUDIO_TYPE_SFX) : 0.f;
}
