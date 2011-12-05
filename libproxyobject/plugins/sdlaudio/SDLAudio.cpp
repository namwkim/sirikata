// Copyright (c) 2011 Sirikata Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.

#include "SDLAudio.hpp"

#include <sirikata/core/transfer/AggregatedTransferPool.hpp>
#include <sirikata/core/network/IOStrandImpl.hpp>

#include <sirikata/sdl/SDL.hpp>
#include "SDL_audio.h"

#include "FFmpegGlue.hpp"
#include "FFmpegMemoryProtocol.hpp"
#include "FFmpegStream.hpp"
#include "FFmpegAudioStream.hpp"

#define AUDIO_LOG(lvl, msg) SILOG(sdl-audio, lvl, msg)

namespace Sirikata {
namespace SDL {

namespace {

AudioSimulation* ToSimulation(void* data) {
    return reinterpret_cast<AudioSimulation*>(data);
}

extern void mixaudio(void* _sim, Uint8* raw_stream, int raw_len) {
    AUDIO_LOG(insane, "Mixing audio samples");

    AudioSimulation* sim = ToSimulation(_sim);
    sim->mix(raw_stream, raw_len);
}

}

AudioSimulation::AudioSimulation(Context* ctx)
 : mContext(ctx),
   mInitializedAudio(false),
   mOpenedAudio(false)
{}

void AudioSimulation::start() {
    AUDIO_LOG(detailed, "Starting SDLAudio");

    if (SDL::InitializeSubsystem(SDL::Subsystem::Audio) != 0)
        return;

    mInitializedAudio = true;

    SDL_AudioSpec fmt;

    /* Set 16-bit stereo audio at 44Khz */
    fmt.freq = 44100;
    fmt.format = AUDIO_S16;
    fmt.channels = 2;
    fmt.samples = 2048;
    fmt.callback = mixaudio;
    fmt.userdata = this;

    /* Open the audio device and start playing sound! */
    if ( SDL_OpenAudio(&fmt, NULL) < 0 ) {
        AUDIO_LOG(error, "Unable to open audio: " << SDL_GetError());
        return;
    }

    mOpenedAudio = true;

    FFmpegGlue::getSingleton().ref();

    mTransferPool = Transfer::TransferMediator::getSingleton().registerClient<Transfer::AggregatedTransferPool>("SDLAudio");
}

bool AudioSimulation::ready() const {
    return (mInitializedAudio && mOpenedAudio && mTransferPool);
}

void AudioSimulation::stop() {
    AUDIO_LOG(detailed, "Stopping SDLAudio");

    mTransferPool.reset();
    mDownloads.clear();

    if (!mInitializedAudio)
        return;

    if (mOpenedAudio) {
        SDL_PauseAudio(1);
        SDL_CloseAudio();

        FFmpegGlue::getSingleton().unref();
    }

    SDL::QuitSubsystem(SDL::Subsystem::Audio);
    mInitializedAudio = false;
}

boost::any AudioSimulation::invoke(std::vector<boost::any>& params) {
    // Decode the command. First argument is the "function name"
    if (params.empty() || !Invokable::anyIsString(params[0]))
        return boost::any();

    std::string name = Invokable::anyAsString(params[0]);
    AUDIO_LOG(detailed, "Invoking the function " << name);

    if (name == "play") {
        // Ignore if we didn't initialize properly
        if (!ready())
            return Invokable::asAny(false);

        if (params.size() < 2 || !Invokable::anyIsString(params[1]))
            return Invokable::asAny(false);
        String sound_url_str = Invokable::anyAsString(params[1]);
        Transfer::URI sound_url(sound_url_str);
        if (sound_url.empty()) return Invokable::asAny(false);

        AUDIO_LOG(detailed, "Play request for " << sound_url.toString());
        DownloadTaskMap::iterator task_it = mDownloads.find(sound_url);
        // If we're already working on it, we don't need to do
        // anything.  TODO(ewencp) actually we should track the number
        // of requests and play it that many times when it completes...
        if (task_it != mDownloads.end()) {
            AUDIO_LOG(insane, "Already downloading " << sound_url.toString());
            return Invokable::asAny(true);
        }

        AUDIO_LOG(insane, "Issuing download request for " << sound_url.toString());
        Transfer::ResourceDownloadTaskPtr dl = Transfer::ResourceDownloadTask::construct(
            sound_url, mTransferPool,
            1.0,
            mContext->mainStrand->wrap(
                std::tr1::bind(&AudioSimulation::handleFinishedDownload, this, _1, _2)
            )
        );
        mDownloads[sound_url] = dl;
        dl->start();
    }
    else {
        AUDIO_LOG(warn, "Function " << name << " was invoked but this function was not found.");
    }

    return boost::any();
}

void AudioSimulation::handleFinishedDownload(Transfer::ChunkRequestPtr request, Transfer::DenseDataPtr response) {
    const Transfer::URI& sound_url = request->getMetadata().getURI();
    // We may have stopped the simulation and then gotten the callback. Ignore
    // in this case.
    if (mDownloads.find(sound_url) == mDownloads.end()) return;

    // Otherwise remove the record
    mDownloads.erase(sound_url);

    // If the download failed, just log it
    if (!response) {
        AUDIO_LOG(error, "Failed to download " << sound_url << " sound file.");
        return;
    }

    if (response->size() == 0) {
        AUDIO_LOG(error, "Got zero sized audio file download for " << sound_url);
        return;
    }

    AUDIO_LOG(detailed, "Finished download for audio file " << sound_url << ": " << response->size() << " bytes");

    FFmpegMemoryProtocol* dataSource = new FFmpegMemoryProtocol(sound_url.toString(), response);
    FFmpegStreamPtr stream(FFmpegStream::construct<FFmpegStream>(static_cast<FFmpegURLProtocol*>(dataSource)));
    if (stream->numAudioStreams() == 0) {
        AUDIO_LOG(error, "Found zero audio streams in " << sound_url << ", ignoring");
        return;
    }
    if (stream->numAudioStreams() > 1)
        AUDIO_LOG(detailed, "Found more than one audio stream in " << sound_url << ", only playing first");
    FFmpegAudioStreamPtr audio_stream = stream->getAudioStream(0, 2);

    Lock lck(mStreamsMutex);
    mStreams.push_back(audio_stream);
    // Enable playback if we didn't have any active streams before
    if (mStreams.size() == 1)
        SDL_PauseAudio(0);
}

void AudioSimulation::mix(uint8* raw_stream, int32 raw_len) {
    int16* stream = (int16*)raw_stream;
    // Length in individual samples
    int32 stream_len = raw_len / sizeof(int16);
    // Length in samples for all channels
    int32 nchannels = 2; // Assuming stereo, see SDL audio setup
    int32 samples_len = stream_len / nchannels;

    Lock lck(mStreamsMutex);

    for(int i = 0; i < samples_len; i++) {
        int32 mixed[nchannels];
        for(int c = 0; c < nchannels; c++)
            mixed[c] = 0;

        for(uint32 st = 0; st < mStreams.size(); st++) {
            int16 samples[nchannels];
            mStreams[st]->samples(samples);

            for(int c = 0; c < nchannels; c++)
                mixed[c] += samples[c];
        }

        for(int c = 0; c < nchannels; c++)
            stream[i*nchannels + c] = (int16)std::min(std::max(mixed[c], -32768), 32767);
    }

    // Clean out streams that have finished
    for(int32 idx = mStreams.size()-1; idx >= 0; idx--)
        if (mStreams[idx]->finished()) mStreams.erase(mStreams.begin() + idx);

    // Disable playback if we've run out of sounds
    if (mStreams.empty())
        SDL_PauseAudio(1);
}

} //namespace SDL
} //namespace Sirikata