/**
 * @file slpanelsoundplayer.h
 * @brief The sound preview player (#1311): Play / Pause, a scrub bar over the waveform, the
 *        position and length, and a loop toggle. Registered as panel class @c sound_player
 *        (panel_sound_player.xml); the inventory sound preview and the sound upload preview
 *        use it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SL_PANELSOUNDPLAYER_H
#define SL_PANELSOUNDPLAYER_H

#include "llpanel.h"

#include <boost/signals2/connection.hpp>
#include <memory>
#include <vector>

class LLAudioEngine;
class LLButton;
class LLCheckBoxCtrl;
class LLTextBox;
class SLSoundScrubBar;

// Local preview only: it plays an engine local clip (LLAudioEngine::createLocalClip), never a
// channel, so it changes nothing in-world. Only you hear it, at Master x Sound Effects (the
// master volume is the engine's listener gain; the panel applies the Sound Effects level), and
// one player plays at a time: starting one pauses any other.
//
// The panel keeps the playhead itself whenever the clip is not playing (OpenAL reports offset
// 0 after a stop) and seeks the clip there on each play. The engine clip is made on the first
// play from the PCM the panel holds, and remade if the engine is replaced (its clips go with
// it), so a stale id never reaches another engine.
//
// Retained UI: a recorded floater is replayed without calling draw(), so nothing here runs
// from draw(). An idle callback, registered only while there is something to follow (a sound
// loading, or playing), picks up the decoded file and follows the playhead and the end; the
// blocklist's change signal handles a sound blocked or unblocked meanwhile. Every change to
// what the controls show reports onRetainedDirty() (the text box, enabled state and scrub bar
// report their own; the Play / Pause label is reported here).
class SLPanelSoundPlayer final : public LLPanel
{
public:
    SLPanelSoundPlayer();
    ~SLPanelSoundPlayer() override;

    bool postBuild() override;
    void onVisibilityChange(bool new_visibility) override;

    // A sound asset (the inventory preview): requested from the engine (fetched and decoded as
    // in-world, LLAudioEngine::requestSound), then its cached decoded file is played -- exactly
    // what in-world playback hears, LL's short edge fades included. The same id again is a
    // no-op; null clears the player.
    void setSound(const LLUUID& asset_id);
    // PCM already in memory (the upload preview's decoded file): interleaved 16-bit,
    // `channels` (1 or 2) per frame, at `rate`. Shared, not copied.
    void setPCM(std::shared_ptr<const std::vector<S16>> pcm, U32 channels, U32 rate);

private:
    enum EState
    {
        NO_SOUND,       // nothing set (or nothing playable in what was set)
        NO_AUDIO,       // no audio engine (no device, or audio disabled)
        BLOCKED,        // the sound is on the blocklist (#654)
        LOADING,        // fetching / decoding
        UNAVAILABLE,    // marked bad after failed fetches / decodes (tried again later)
        READY
    };
    EState getState() const;
    bool   isBlocked() const;

    void reset();                   // back to NO_SOUND (stops and frees everything)
    void pollAsset();               // LOADING: request, then read the decoded file once there
    bool adoptPCM(std::shared_ptr<const std::vector<S16>> pcm, U32 channels, U32 rate);
    void releasePCM();
    void followClip();              // while playing: the playhead, the end, the volume
    void forgetStaleEngine();       // the engine was replaced or is gone: drop its clip / request
    void onBlocklistChanged();

    static void onIdle(void* self);
    void idle();
    bool hasWork() const;           // something for the idle callback to follow
    // After any change: the controls, and the idle callback registered only while it has work.
    void refreshState();
    void updateControls();

    void play();
    void pause();
    void onPlayPause();
    void onScrub(F32 fraction);
    void onLoop();

    // The engine our clip lives on, if there is a clip and that engine is still the current one.
    LLAudioEngine* clipEngine() const;
    void releaseRequest();
    void destroyClip();
    static F32 gain();

    SLSoundScrubBar* mBar = nullptr;
    LLButton*        mPlayBtn = nullptr;
    LLTextBox*       mTimeText = nullptr;
    LLCheckBoxCtrl*  mLoopCheck = nullptr;

    // What plays: an asset on its way (mAssetID, mRequestEngine holds a requestSound), then its
    // PCM; or PCM handed over (setPCM).
    LLUUID           mAssetID;
    LLAudioEngine*   mRequestEngine = nullptr;
    std::shared_ptr<const std::vector<S16>> mPCM;
    U32              mChannels = 1;
    U32              mRate = 0;
    U32              mFrames = 0;
    U64              mPCMBytes = 0;
    F64              mNextLoadRetry = 0;

    // Playback: the engine's clip (0: none yet), whether it plays, and the playhead in frames.
    LLAudioEngine*   mClipEngine = nullptr;
    U32              mClip = 0;
    bool             mPlaying = false;
    U32              mCursor = 0;
    bool             mClipFailed = false;   // the engine refused the clip (logged once)

    bool             mIdling = false;       // onIdle is registered
    boost::signals2::scoped_connection mBlocklistConn;
    boost::signals2::scoped_connection mEngineConn;

    // The Play button's labels (Play Locally for an asset, Play for a file), and what the
    // controls show: set only on change (a new label or text re-lays out the widget).
    std::string      mPlayLabel;
    std::string      mPauseLabel;
    std::string      mShownLabel;
    std::string      mShownTime;
};

#endif // SL_PANELSOUNDPLAYER_H
