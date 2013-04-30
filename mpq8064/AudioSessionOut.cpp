/* AudioSessionOutALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2011-2013, The Linux Foundation. All rights reserved
 ** Not a Contribution, Apache license notifications and license are retained
 ** for attribution purposes only.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioSessionOutALSA"
#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include <linux/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <linux/unistd.h>

#include "AudioHardwareALSA.h"

namespace sys_write {
    ssize_t lib_write(int fd, const void *buf, size_t count) {
        return write(fd, buf, count);
    }
};
namespace android_audio_legacy
{
// ----------------------------------------------------------------------------

AudioSessionOutALSA::AudioSessionOutALSA(AudioHardwareALSA *parent,
                                         uint32_t   devices,
                                         int        format,
                                         uint32_t   channels,
                                         uint32_t   samplingRate,
                                         int        sessionId,
                                         status_t   *status)
{
    devices |= AudioSystem::DEVICE_OUT_SPDIF;
    Mutex::Autolock autoLock(mLock);
    // Default initilization
    mParent         = parent;
    mALSADevice     = mParent->mALSADevice;
    mUcMgr          = mParent->mUcMgr;
    mFrameCount     = 0;
    mFormat         = format;
    mSampleRate     = samplingRate;
    ALOGV("channel map = %d", channels);
    mChannels = channels = channelMapToChannels(channels);
    if(format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_HE_AAC_V1 ||
       format == AUDIO_FORMAT_HE_AAC_V2 || format == AUDIO_FORMAT_AAC_ADIF) {
        if(samplingRate > 24000) {
            mSampleRate     = 48000;
        }
        mChannels       = 6;
    } else if (format == AUDIO_FORMAT_AC3 || format == AUDIO_FORMAT_EAC3) {
        mChannels   = 6;
    }
    // NOTE: This has to be changed when Multi channel PCM has to be
    // sent over HDMI
    mDevices        = devices;
    mSecDevices     = 0;
    mBufferSize     = 0;
    *status         = BAD_VALUE;
    mSessionId      = sessionId;
    hw_ptr[0]       = 0;
    hw_ptr[1]       = 0;

    mUseTunnelDecoder   = false;
    mUseDualTunnel      = false;
    mCaptureFromProxy   = false;
    mRoutePcmAudio      = false;
    mSpdifFormat        = INVALID_FORMAT;
    mHdmiFormat         = INVALID_FORMAT;
    mRouteAudioToA2dp   = false;
    mUseMS11Decoder     = false;
    mChannelStatusSet   = false;
    mTunnelPaused       = false;
    mDtsTranscode       = false;
    mPaused             = false;
    mTunnelSeeking      = false;
    mReachedExtractorEOS= false;
    mSkipWrite          = false;
    strlcpy(mSpdifOutputFormat,mParent->mSpdifOutputFormat,
                                      sizeof(mSpdifOutputFormat));
    strlcpy(mHdmiOutputFormat,mParent->mHdmiOutputFormat,
                                      sizeof(mHdmiOutputFormat));
    updateOutputFormat();

    mPcmRxHandle        = NULL;
    mCompreRxHandle     = NULL;
    mSecCompreRxHandle  = NULL;
    mTranscodeHandle    = NULL;
    mMS11Decoder        = NULL;
    mBitstreamSM        = NULL;

    mInputBufferSize    = MULTI_CHANNEL_MAX_PERIOD_SIZE;
    mInputBufferCount   = MULTI_CHANNEL_PERIOD_COUNT;
    mEfd = -1;

    mWMAConfigDataSet    = false;
    mAacConfigDataSet    = false; // flags if AAC config to be set(which is sent in first buffer)
    mEventThread         = NULL;
    mEventThreadAlive    = false;
    mKillEventThread     = false;
    mMinBytesReqToDecode = 0;
    mObserver            = NULL;
    mCurDevice           = 0;
    mOutputMetadataLength = 0;
    mTranscodeDevices     = 0;
    mFirstBuffer         = true;
    mADTSHeaderPresent    = false;
    mFirstAACBuffer      = NULL;
    mFirstAACBufferLength = 0;
    mA2dpUseCase = AudioHardwareALSA::USECASE_NONE;

    if(devices == 0) {
        ALOGE("No output device specified");
        *status = BAD_VALUE;
        return;
    }
    if((format == AUDIO_FORMAT_PCM_16_BIT) && (channels <= 0 || channels > 8)) {
        ALOGE("Invalid number of channels %d", channels);
        *status = BAD_VALUE;
        return;
    }


    if(mDevices & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        ALOGE("Set Capture from proxy true");
        mRouteAudioToA2dp = true;
        devices  &= ~AudioSystem::DEVICE_OUT_ALL_A2DP;
        devices  &= ~AudioSystem::DEVICE_OUT_SPDIF;
        devices |=  AudioSystem::DEVICE_OUT_PROXY;
        mDevices = devices;
    }

    if(format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_HE_AAC_V1 ||
       format == AUDIO_FORMAT_HE_AAC_V2 || format == AUDIO_FORMAT_AAC_ADIF ||
       format == AUDIO_FORMAT_AC3 || format == AUDIO_FORMAT_EAC3) {
        if(!(dlopen("libms11.so", RTLD_NOW))) {
            ALOGE("MS11 library is not available to decode");
            *status = BAD_VALUE;
            return;
        }
        // Instantiate MS11 decoder for single decode use case
        int32_t format_ms11;
        mMS11Decoder = new SoftMS11;
        if(mMS11Decoder->initializeMS11FunctionPointers() == false) {
            ALOGE("Could not resolve all symbols Required for MS11");
            delete mMS11Decoder;
            return;
        }
        mBitstreamSM = new AudioBitstreamSM;
        if(false == mBitstreamSM->initBitstreamPtr()) {
            ALOGE("Unable to allocate Memory for i/p and o/p buffering for MS11");
            delete mMS11Decoder;
            delete mBitstreamSM;
            return;
        }
        if(format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_HE_AAC_V1 ||
           format == AUDIO_FORMAT_HE_AAC_V2 || format == AUDIO_FORMAT_AAC_ADIF)
        {
            if(format == AUDIO_FORMAT_AAC_ADIF)
                mMinBytesReqToDecode = AAC_BLOCK_PER_CHANNEL_MS11*mChannels-1;
            else
                mMinBytesReqToDecode = 0;
            format_ms11 = FORMAT_DOLBY_PULSE_MAIN;
        } else {
            format_ms11 = FORMAT_DOLBY_DIGITAL_PLUS_MAIN;
            mMinBytesReqToDecode = 0;
        }
        if(mMS11Decoder->setUseCaseAndOpenStream(format_ms11,mChannels,samplingRate)) {
            ALOGE("SetUseCaseAndOpen MS11 failed");
            delete mMS11Decoder;
            delete mBitstreamSM;
            return;
        }
        mUseMS11Decoder   = true; // indicates if MS11 decoder is instantiated
        mAacConfigDataSet = false; // flags if AAC config to be set(which is sent in first buffer)

    } else if(format == AUDIO_FORMAT_WMA || format == AUDIO_FORMAT_WMA_PRO ||
              format == AUDIO_FORMAT_DTS || format == AUDIO_FORMAT_MP3 ||
              format == AUDIO_FORMAT_DTS_LBR || format == AUDIO_FORMAT_MP2){
        // In this case, DSP will decode and route the PCM data to output devices
        mUseTunnelDecoder = true;
        mWMAConfigDataSet = false;

    } else if(format == AUDIO_FORMAT_PCM_16_BIT) {
        mMinBytesReqToDecode = PCM_BLOCK_PER_CHANNEL_MS11*mChannels-1;
/* Enable this when Support of 6 channel to AC3 is required. Till then PCM is pass throughed
        if(channels > 2 && channels <= 6) {
            // Instantiate MS11 decoder for downmix and re-encode
            mMS11Decoder = new SoftMS11;
            if(mMS11Decoder->initializeMS11FunctionPointers() == false) {
                ALOGE("Could not resolve all symbols Required for MS11");
                delete mMS11Decoder;
                return;
            }
            if(mMS11Decoder->setUseCaseAndOpenStream(FORMAT_EXTERNAL_PCM,channels,samplingRate)) {
                ALOGE("SetUseCaseAndOpen MS11 failed");
                delete mMS11Decoder;
                return;
            }
            mUseMS11Decoder  = true;
        }
*/
        mBitstreamSM = new AudioBitstreamSM;
        if(false == mBitstreamSM->initBitstreamPtr()) {
            ALOGE("Unable to allocate Memory for i/p and o/p buffering for MS11");
            delete mBitstreamSM;
            return;
        }

    } else {
        ALOGE("Unsupported format %d", format);
        return;
    }

    updateRoutingFlags(mDevices);

    if(mRoutePcmAudio) {
        // If the same audio PCM is to be routed to SPDIF also, do not remove from
        // device list
        *status = openPcmDevice(devices);
        if (*status != NO_ERROR)
            return;
    }
    if (mUseTunnelDecoder) {
        ALOGV("Tunnel decoder case use mSecDevices=%d, mUseDualTunnel=%d \
        mSecCompreRxHandle=%u", mSecDevices, mUseDualTunnel, mSecCompreRxHandle);
        if (format != AUDIO_FORMAT_WMA && format != AUDIO_FORMAT_WMA_PRO)
            *status = openTunnelDevice(mDevices);
        else
            *status = NO_ERROR;
        if(*status != NO_ERROR)
            return;
        createThreadsForTunnelDecode();
    } else if ((mSpdifFormat == COMPRESSED_FORMAT) ||
               (mHdmiFormat == COMPRESSED_FORMAT)) {
        devices = 0;
        if(mSpdifFormat == COMPRESSED_FORMAT)
            devices = AudioSystem::DEVICE_OUT_SPDIF;
        if(mHdmiFormat == COMPRESSED_FORMAT)
            devices |= AudioSystem::DEVICE_OUT_AUX_DIGITAL;
        if(mCaptureFromProxy) {
            devices |= AudioSystem::DEVICE_OUT_PROXY;
        }
        *status = openTunnelDevice(devices);
        if(*status != NO_ERROR)
            return;
        createThreadsForTunnelDecode();
    }
    ALOGV("mRouteAudioToA2dp = %d", mRouteAudioToA2dp);
    if (mRouteAudioToA2dp) {
        alsa_handle_t *handle = NULL;
        if (mPcmRxHandle && (mPcmRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mPcmRxHandle;
        else if (mCompreRxHandle && (mCompreRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mCompreRxHandle;
        else if (mSecCompreRxHandle && (mSecCompreRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mSecCompreRxHandle;
        if (handle) {
            mA2dpUseCase = mParent->useCaseStringToEnum(handle->useCase);
            ALOGD("startA2dpPlayback_l - usecase %x", mA2dpUseCase);
            status_t err = mParent->startA2dpPlayback_l(mA2dpUseCase);
            if(err != NO_ERROR) {
                ALOGW("startA2dpPlayback_l returned = %d", err);
                *status = err;
            }
        }
        // if handle is null(WMA), take care of this in write call
    }
    mCurDevice = devices;
}

AudioSessionOutALSA::~AudioSessionOutALSA()
{
    ALOGV("~AudioSessionOutALSA");

    mSkipWrite = true;
    mWriteCv.signal();

    //TODO: This might need to be Locked using Parent lock
    reset();

    if (mRouteAudioToA2dp) {
        ALOGD("destructor - stopA2dpPlayback_l - usecase %x", mA2dpUseCase);
        status_t err = mParent->stopA2dpPlayback_l(mA2dpUseCase);
        if(err) {
            ALOGE("destructor - stopA2dpPlayback_l return err = %d", err);
        }
        mRouteAudioToA2dp = false;
    }
}

status_t AudioSessionOutALSA::setParameters(const String8& keyValuePairs)
{
    Mutex::Autolock autoLock(mControlLock);
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    int device;
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        if(device) {
            device |= AudioSystem::DEVICE_OUT_SPDIF;
            ALOGD("setParameters(): keyRouting with device %d", device);
            doRouting(device);
        }
        param.remove(key);
    }else {
        mControlLock.unlock();
        mParent->setParameters(keyValuePairs);
    }
    return NO_ERROR;
}

String8 AudioSessionOutALSA::getParameters(const String8& keys)
{
    Mutex::Autolock autoLock(mControlLock);
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);
    int devices = mDevices;
    ALOGV("getParameters mDevices %d mRouteAudioToA2dp %d", mDevices, mRouteAudioToA2dp);
    if (param.get(key, value) == NO_ERROR) {
        if((mDevices & AudioSystem::DEVICE_OUT_PROXY) && mRouteAudioToA2dp) {
            devices |= AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP;
            devices &= ~AudioSystem::DEVICE_OUT_PROXY;
        }
        param.addInt(key, (int)devices);
    }

    ALOGV("getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioSessionOutALSA::setVolume(float left, float right)
{
    Mutex::Autolock autoLock(mRoutingLock);
    float volume;
    status_t status = NO_ERROR;

    volume = (left + right) / 2;
    if (volume < 0.0) {
        ALOGW("AudioSessionOutALSA::setVolume(%f) under 0.0, assuming 0.0\n", volume);
        volume = 0.0;
    } else if (volume > 1.0) {
        ALOGW("AudioSessionOutALSA::setVolume(%f) over 1.0, assuming 1.0\n", volume);
        volume = 1.0;
    }
   mStreamVol = lrint((volume * 0x2000)+0.5);

    ALOGD("Setting stream volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
    if(mPcmRxHandle) {
        ALOGD("setPCM 1 Volume(%f)\n", volume);
        ALOGD("Setting PCM volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
        status = mPcmRxHandle->module->setPlaybackVolume(mStreamVol,
                                               mPcmRxHandle->useCase);
        return status;
    }
    else if(mCompreRxHandle) {
        if (mSpdifFormat != COMPRESSED_FORMAT && mHdmiFormat != COMPRESSED_FORMAT) {
            ALOGD("set compressed Volume(%f) handle->type %d\n", volume, mCompreRxHandle->type);
            ALOGD("Setting Compressed volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
            status = mCompreRxHandle->module->setPlaybackVolume(mStreamVol,
                                                 mCompreRxHandle->useCase);
        }
        return status;
    }
    return INVALID_OPERATION;
}


status_t AudioSessionOutALSA::openTunnelDevice(int devices)
{
    // If audio is to be capture back from proxy device, then route
    // audio to SPDIF and Proxy devices only
    char *use_case;
    status_t status = NO_ERROR;
    if(mCaptureFromProxy) {
        devices = AudioSystem::DEVICE_OUT_PROXY;
    }
    mInputBufferSize    = TUNNEL_DECODER_BUFFER_SIZE;
    mInputBufferCount   = TUNNEL_DECODER_BUFFER_COUNT;

    status = setPlaybackFormat();
    if(status != NO_ERROR) {
        ALOGE("setPlaybackFormat Failed");
        return BAD_VALUE;
    }

    mOutputMetadataLength = sizeof(output_metadata_handle_t);
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if (devices & ~mSecDevices & ~mTranscodeDevices) {
        hw_ptr[0] = 0;
        if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                 strlen(SND_USE_CASE_VERB_INACTIVE)))) {
            status = openDevice(SND_USE_CASE_VERB_HIFI_TUNNEL, true, devices & ~mSecDevices & ~mTranscodeDevices);
        } else {
            status = openDevice(SND_USE_CASE_MOD_PLAY_TUNNEL1, false, devices & ~mSecDevices & ~mTranscodeDevices);
        }
        if(use_case) {
            free(use_case);
            use_case = NULL;
        }
        if(status != NO_ERROR)
            return status;
        ALOGD("openTunnelDevice - mOutputMetadataLength = %d", mOutputMetadataLength);
        ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
        mCompreRxHandle = &(*it);

        //mmap the buffers for playback
        status_t err = mmap_buffer(mCompreRxHandle->handle);
        if(err) {
            ALOGE("MMAP buffer failed - playback err = %d", err);
            return err;
        }
   }

    if (mDtsTranscode) {
        mTranscodeHandle = (alsa_handle_t *) calloc(1, sizeof(alsa_handle_t));
        mTranscodeHandle->devices = mTranscodeDevices;
        mTranscodeHandle->activeDevice= mTranscodeDevices;
        mTranscodeHandle->mode = mParent->mode();
        mTranscodeHandle->ucMgr = mUcMgr;
        mTranscodeHandle->module = mALSADevice;
        //Passthrough to be configured with 2 channels
        mTranscodeHandle->channels = 2;
        mTranscodeHandle->sampleRate = mSampleRate > 48000 ? 48000: mSampleRate;
        ALOGV("Transcode devices = %d", mTranscodeDevices);
        strlcpy(mTranscodeHandle->useCase, mCompreRxHandle->useCase, sizeof(mTranscodeHandle->useCase));
        strncat(mTranscodeHandle->useCase, SND_USE_CASE_PSEUDO_TUNNEL_SUFFIX, sizeof(mTranscodeHandle->useCase));
        mALSADevice->setUseCase(mTranscodeHandle, false);
        mALSADevice->configureTranscode(mTranscodeHandle);
    }

    if ((devices & ~mSecDevices & ~mTranscodeDevices) && (mCompreRxHandle != NULL)) {
        //prepare the driver for playback
        status = pcm_prepare(mCompreRxHandle->handle);
        if (status) {
            ALOGE("PCM Prepare failed - playback status = %d", status);
            return status;
        }
        bufferAlloc(mCompreRxHandle, DECODEQUEUEINDEX);
        mBufferSize = mCompreRxHandle->periodSize;

    }

    //Check and open if Secound Tunnel is also required
    if(mUseDualTunnel && mSecDevices) {
        hw_ptr[1] = 0;
        ALOGE("opening second compre device");
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                 strlen(SND_USE_CASE_VERB_INACTIVE)))) {
            status = openDevice(SND_USE_CASE_VERB_HIFI_TUNNEL2, true, mSecDevices);
        } else {
            status = openDevice(SND_USE_CASE_MOD_PLAY_TUNNEL2, false, mSecDevices);
        }
        if(use_case) {
            free(use_case);
            use_case = NULL;
        }
        if(status != NO_ERROR) {
            return status;
        }
        ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
        mSecCompreRxHandle = &(*it);
        //mmap the buffers for second playback
        status_t err = mmap_buffer(mSecCompreRxHandle->handle);
        if(err) {
            ALOGE("MMAP buffer failed - playback err = %d", err);
            return err;
        }
        //prepare the driver for second playback
        status = pcm_prepare(mSecCompreRxHandle->handle);
        if (status) {
            ALOGE("PCM Prepare failed - playback err = %d", err);
            return status;
        }
        bufferAlloc(mSecCompreRxHandle, PASSTHRUQUEUEINDEX);
        ALOGD("Buffer allocated");
    }

    return status;
}

ssize_t AudioSessionOutALSA::write(const void *buffer, size_t bytes)
{
    int period_size;
    char *use_case;

    Mutex::Autolock autoLock(mLock);
    ALOGV("write:: buffer %p, bytes %d", buffer, bytes);
    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioSessionOutLock");
        mPowerLock = true;
    }
    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;
    if (mUseTunnelDecoder && mWMAConfigDataSet == false &&
            (mFormat == AUDIO_FORMAT_WMA || mFormat == AUDIO_FORMAT_WMA_PRO)) {
        ALOGV("Configuring the WMA params");
        status_t err = mALSADevice->setWMAParams(mCompreRxHandle,
                                         (int *)buffer, bytes/sizeof(int));
        if (err) {
            ALOGE("WMA param config failed");
            return -1;
        }
        err = openTunnelDevice(mDevices);
        if (err) {
            ALOGE("opening of tunnel device failed");
            if (mObserver)
                mObserver->postEOS(0);
            return -1;
        }
        if (mSpdifFormat != COMPRESSED_FORMAT && mHdmiFormat != COMPRESSED_FORMAT) {
            ALOGD("Setting Compressed volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
            err = mCompreRxHandle->module->setPlaybackVolume(mStreamVol,
                                                 mCompreRxHandle->useCase);
            if(err){
               ALOGE("setPlaybackVolume returned error %d",err);
               if (mObserver)
                   mObserver->postEOS(0);
               return -1;
            }
        }
        mWMAConfigDataSet = true;
        return bytes;
    }
    if (mRouteAudioToA2dp && mA2dpUseCase == AudioHardwareALSA::USECASE_NONE) {
        alsa_handle_t *handle = NULL;
        if (mPcmRxHandle && (mPcmRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mPcmRxHandle;
        else if (mCompreRxHandle && (mCompreRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mCompreRxHandle;
        else if (mSecCompreRxHandle && (mSecCompreRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mSecCompreRxHandle;
        if (handle) {
            mA2dpUseCase = mParent->useCaseStringToEnum(handle->useCase);
            ALOGD("startA2dpPlayback_l - usecase %x", mA2dpUseCase);
            status_t err = mParent->startA2dpPlayback_l(mA2dpUseCase);
            if(err != NO_ERROR) {
                ALOGE("write:startA2dpPlayback_l returned = %d", err);
                return -1;
            }
        }
        else
            return -1;
    }
#ifdef DEBUG
    mFpDumpInput = fopen("/data/input.raw","a");
    if(mFpDumpInput != NULL) {
        fwrite((char *)buffer, 1, bytes, mFpDumpInput);
        fclose(mFpDumpInput);
    }
#endif
    if (mUseTunnelDecoder && mCompreRxHandle) {

        writeToCompressedDriver((char *)buffer, bytes);

        if(mChannelStatusSet == false) {
            if(mSpdifFormat == PCM_FORMAT) {
                if (mALSADevice->get_linearpcm_channel_status(mSampleRate,
                                      mChannelStatus)) {
                    ALOGE("channel status set error ");
                    return -1;
                }
                mALSADevice->setChannelStatus(mChannelStatus);
            } else if(mSpdifFormat == COMPRESSED_FORMAT) {
                if (mALSADevice->get_compressed_channel_status(
                                     (char *)buffer, bytes, mChannelStatus,
                                     AUDIO_PARSER_CODEC_DTS)) {
                    ALOGE("channel status set error ");
                    return -1;
                }
                mALSADevice->setChannelStatus(mChannelStatus);
            }
            mChannelStatusSet = true;
        }
    } else if(mMS11Decoder != NULL) {
    // 1. Check if MS11 decoder instance is present and if present we need to
    //    preserve the data and supply it to MS 11 decoder.
        // If MS11, the first buffer in AAC format has the AAC config data.

        if(mFormat == AUDIO_FORMAT_AAC || mFormat == AUDIO_FORMAT_HE_AAC_V1 ||
           mFormat == AUDIO_FORMAT_AAC_ADIF || mFormat == AUDIO_FORMAT_HE_AAC_V2) {
            if(mAacConfigDataSet == false) {
                if(mMS11Decoder->setAACConfig((unsigned char *)buffer, bytes) == true){
                    mAacConfigDataSet = true;
                    mFirstAACBufferLength = bytes;
                    mFirstAACBuffer = malloc(mFirstAACBufferLength);
                    memcpy(mFirstAACBuffer, buffer, mFirstAACBufferLength);
                  }
                return bytes;
            }
        }
        if(bytes == 0) {
            if(mFormat == AUDIO_FORMAT_AAC_ADIF)
                mBitstreamSM->appendSilenceToBitstreamInternalBuffer(mMinBytesReqToDecode,0);
            else if(mCompreRxHandle){
                writeToCompressedDriver((char *)buffer, bytes);
                return bytes;
            }
        }

        /* check for sync word, if present then configure MS11 for fileplayback mode OFF
           This is specifically done to handle Widevine usecase, in which the ADTS HEADER is
           not stripped off by the Widevine parser */
        if((mFirstBuffer == true) && (mFormat == AUDIO_FORMAT_AAC || mFormat == AUDIO_FORMAT_HE_AAC_V1 ||
            mFormat == AUDIO_FORMAT_AAC_ADIF || mFormat == AUDIO_FORMAT_HE_AAC_V2)){

            uint16_t uData = (*((char *)buffer) << 8) + *((char *)buffer + 1) ;

            ALOGD("Check for ADTS SYNC WORD ");
            if(ADTS_HEADER_SYNC_RESULT == (uData & ADTS_HEADER_SYNC_MASK)){
                ALOGD("Sync word found hence configure MS11 in file_playback Mode OFF");
                delete mMS11Decoder;
                mMS11Decoder = new SoftMS11;
                if(mMS11Decoder->initializeMS11FunctionPointers() == false) {
                    ALOGE("Could not resolve all symbols Required for MS11");
                    delete mMS11Decoder;
                    return -1;
                }
                ALOGD("mChannels %d mSampleRate %d",mChannels,mSampleRate);
                if(mMS11Decoder->setUseCaseAndOpenStream(FORMAT_DOLBY_PULSE_MAIN,mChannels,
                    mSampleRate,false)){
                    ALOGE("SetUseCaseAndOpen MS11 failed");
                    delete mMS11Decoder;
                    return -1;
                }
              mADTSHeaderPresent= true;
            }
        }

        bool    continueDecode=false;
        size_t  bytesConsumedInDecode = 0;
        size_t  copyBytesMS11 = 0;
        char    *bufPtr;
        uint32_t outSampleRate=mSampleRate,outChannels=mChannels;
        mBitstreamSM->copyBitstreamToInternalBuffer((char *)buffer, bytes);
        mFirstBuffer = false;

        do
        {
            // flag indicating if the decoding has to be continued so as to
            // get all the output held up with MS11. Examples, AAC can have an
            // output frame of 4096 bytes. While the output of MS11 is 1536, the
            // decoder has to be called more than twice to get the reside samples.
            continueDecode=false;
            if(mBitstreamSM->sufficientBitstreamToDecode(mMinBytesReqToDecode) == true)
            {
                bufPtr = mBitstreamSM->getInputBufferPtr();
                copyBytesMS11 = mBitstreamSM->bitStreamBufSize();

                mMS11Decoder->copyBitstreamToMS11InpBuf(bufPtr,copyBytesMS11);
                bytesConsumedInDecode = mMS11Decoder->streamDecode(&outSampleRate,&outChannels);
                mBitstreamSM->copyResidueBitstreamToStart(bytesConsumedInDecode);
            }
            // Close and open the driver again if the output sample rate change is observed
            // in decode.
            if( (mSampleRate != outSampleRate) || (mChannels != outChannels)) {
                ALOGD("change in sample rate/channel mode - sampleRate:%d,channel mode: %d",
                          outSampleRate, outChannels);
                uint32_t devices = mDevices;
                mSampleRate = outSampleRate;
                mChannels = outChannels;
                if(mPcmRxHandle && mRoutePcmAudio) {
                    status_t status = closeDevice(mPcmRxHandle);
                    if(status != NO_ERROR)
                        break;
                    mPcmRxHandle = NULL;
                    status = openPcmDevice(devices);

                    if(mPcmRxHandle){
                        status = mPcmRxHandle->module->setPlaybackVolume(mStreamVol,
                                                mPcmRxHandle->useCase);
                    }
                    if(status != NO_ERROR)
                        break;
                }
                mChannelStatusSet = false;
            }

            // copy the output of MS11 to HAL internal buffers for PCM and SPDIF
            if(mRoutePcmAudio) {
                bufPtr=mBitstreamSM->getOutputBufferWritePtr(PCM_MCH_OUT);
                copyBytesMS11 = mMS11Decoder->copyOutputFromMS11Buf(PCM_MCH_OUT,bufPtr);
                // Note: Set the output Buffer to start for for changein sample rate and channel
                // This has to be done.
                mBitstreamSM->setOutputBufferWritePtr(PCM_MCH_OUT,copyBytesMS11);
                // If output samples size is zero, donot continue and wait for next
                // write for decode
            }
            if((mSpdifFormat == COMPRESSED_FORMAT) ||
               (mHdmiFormat == COMPRESSED_FORMAT)) {
                bufPtr=mBitstreamSM->getOutputBufferWritePtr(SPDIF_OUT);
                copyBytesMS11 = mMS11Decoder->copyOutputFromMS11Buf(SPDIF_OUT,bufPtr);
                mBitstreamSM->setOutputBufferWritePtr(SPDIF_OUT,copyBytesMS11);
            }
            if(copyBytesMS11)
                continueDecode = true;

            // Set the channel status after first frame decode/transcode and for change
            // in sample rate or channel mode as we close and open the device again
            if(mChannelStatusSet == false) {
                if(mSpdifFormat == PCM_FORMAT) {
                    if(mALSADevice->get_linearpcm_channel_status(mSampleRate,
                                                    mChannelStatus)) {
                        ALOGE("channel status set error ");
                        return -1;
                    }
                    mALSADevice->setChannelStatus(mChannelStatus);
                } else if(mSpdifFormat == COMPRESSED_FORMAT) {
                    if(mALSADevice->get_compressed_channel_status(
                                   mBitstreamSM->getOutputBufferPtr(SPDIF_OUT),
                                                                 copyBytesMS11,
                                        mChannelStatus,AUDIO_PARSER_CODEC_AC3)) {
                        ALOGE("channel status set error ");
                        return -1;
                    }
                    mALSADevice->setChannelStatus(mChannelStatus);
                }
                mChannelStatusSet = true;
            }

            // Write the output of MS11 to appropriate drivers
            if(mPcmRxHandle && mRoutePcmAudio) {
                period_size = mPcmRxHandle->periodSize;
                while(mBitstreamSM->sufficientSamplesToRender(PCM_MCH_OUT,period_size) == true) {
#ifdef DEBUG
                    mFpDumpPCMOutput = fopen("/data/pcm_output.raw","a");
                    if(mFpDumpPCMOutput != NULL) {
                        fwrite(mBitstreamSM->getOutputBufferPtr(PCM_MCH_OUT), 1,
                                   period_size, mFpDumpPCMOutput);
                        fclose(mFpDumpPCMOutput);
                    }
#endif
                    n = pcm_write(mPcmRxHandle->handle,
                              mBitstreamSM->getOutputBufferPtr(PCM_MCH_OUT),
                              period_size);
                    ALOGD("mPcmRxHandle - pcm_write returned with %d", n);
                    if (n == -EBADFD) {
                        // Somehow the stream is in a bad state. The driver probably
                        // has a bug and snd_pcm_recover() doesn't seem to handle this.
                        mPcmRxHandle->module->open(mPcmRxHandle);
                    } else if (n < 0) {
                        // Recovery is part of pcm_write. TODO split is later.
                        ALOGE("mPcmRxHandle - pcm_write returned n < 0");
                        return static_cast<ssize_t>(n);
                    } else {
                        mFrameCountMutex.lock();
                        mFrameCount++;
                        mFrameCountMutex.unlock();
                        sent += static_cast<ssize_t>((period_size));
                        mBitstreamSM->copyResidueOutputToStart(PCM_MCH_OUT,period_size);
                    }
                }
            }
            if((mCompreRxHandle) &&
               ((mSpdifFormat == COMPRESSED_FORMAT) ||
               (mHdmiFormat == COMPRESSED_FORMAT))) {
                int availableSize = mBitstreamSM->getOutputBufferWritePtr(SPDIF_OUT) -
                                        mBitstreamSM->getOutputBufferPtr(SPDIF_OUT);
                while(mBitstreamSM->sufficientSamplesToRender(SPDIF_OUT,1) == true) {
                    n = writeToCompressedDriver(
                                   mBitstreamSM->getOutputBufferPtr(SPDIF_OUT),
                                   availableSize);
                    ALOGD("mCompreRxHandle - pcm_write returned with %d", n);
                    if (n == -EBADFD) {
                        // Somehow the stream is in a bad state. The driver probably
                        // has a bug and snd_pcm_recover() doesn't seem to handle this.
                        mCompreRxHandle->module->open(mCompreRxHandle);
                    } else if (n < 0) {
                        // Recovery is part of pcm_write. TODO split is later.
                        ALOGE("mCompreRxHandle - pcm_write returned n < 0");
                        return static_cast<ssize_t>(n);
                    } else {
                        mBitstreamSM->copyResidueOutputToStart(SPDIF_OUT, availableSize);
                    }
                }
            }
	} while( (continueDecode == true) && (mBitstreamSM->sufficientBitstreamToDecode(mMinBytesReqToDecode) == true));
    } else {
        // 2. Get the output data from Software decoder and write to PCM driver
        if(bytes == 0)
            return bytes;
        if(mPcmRxHandle && mRoutePcmAudio) {
            int write_pending = bytes;
            period_size = mPcmRxHandle->periodSize;

            mBitstreamSM->copyBitstreamToInternalBuffer((char *)buffer, bytes);

            while(mPcmRxHandle->handle &&
                  (mBitstreamSM->sufficientBitstreamToDecode(period_size)
                                     == true)) {
                ALOGV("Calling pcm_write");
#ifdef DEBUG
                mFpDumpPCMOutput = fopen("/data/pcm_output.raw","a");
                if(mFpDumpPCMOutput != NULL) {
                    fwrite(mBitstreamSM->getInputBufferPtr(), 1,
                               period_size, mFpDumpPCMOutput);
                    fclose(mFpDumpPCMOutput);
                }
#endif
                n = pcm_write(mPcmRxHandle->handle,
                         mBitstreamSM->getInputBufferPtr(),
                          period_size);
                ALOGD("pcm_write returned with %d", n);
                if (n == -EBADFD) {
                    // Somehow the stream is in a bad state. The driver probably
                    // has a bug and snd_pcm_recover() doesn't seem to handle this.
                    mPcmRxHandle->module->open(mPcmRxHandle);
                }
                else if (n < 0) {
                    // Recovery is part of pcm_write. TODO split is later.
                    ALOGE("pcm_write returned n < 0");
                    return static_cast<ssize_t>(n);
                }
                else {
                    mFrameCountMutex.lock();
                    mFrameCount++;
                    mFrameCountMutex.unlock();
                    sent += static_cast<ssize_t>((period_size));
                    mBitstreamSM->copyResidueBitstreamToStart(period_size);
                }
            }
        }
    }
    return sent;
}

int32_t AudioSessionOutALSA::writeToCompressedDriver(char *buffer, int bytes)
{
        int n;
        ALOGV("Signal Event thread\n");
        mEventCv.signal();
        mInputMemMutex.lock();
        ALOGV("write Empty Queue1 size() = %d, Queue2 size() = %d\
        Filled Queue1 size() = %d, Queue2 size() = %d",\
        mInputMemEmptyQueue[0].size(), mInputMemEmptyQueue[1].size(),\
        mInputMemFilledQueue[0].size(), mInputMemFilledQueue[1].size());
        while (mInputMemEmptyQueue[0].empty() ||
            (mSecCompreRxHandle!=NULL && mInputMemEmptyQueue[1].empty())) {
            ALOGV("Write: waiting on mWriteCv");
            mLock.unlock();
            mInputMemMutex.unlock();
            mWriteCvMutex.lock();
            mWriteCv.wait(mWriteCvMutex);
            mWriteCvMutex.unlock();
            mLock.lock();
            mInputMemMutex.lock();
            if (mSkipWrite) {
                ALOGV("Write: Flushing the previous write buffer");
                mSkipWrite = false;
                mInputMemMutex.unlock();
                return 0;
            }
            ALOGV("Write: received a signal to wake up");
        }

        List<BuffersAllocated>::iterator it = mInputMemEmptyQueue[0].begin();
        BuffersAllocated buf = *it;
        if (bytes)
            mInputMemEmptyQueue[0].erase(it);
        updateMetaData(bytes);
        memcpy(buf.memBuf, &mOutputMetadataTunnel, mOutputMetadataLength);
        ALOGD("Copy Metadata1 = %d, bytes = %d", mOutputMetadataLength, bytes);
        memcpy(((uint8_t *)buf.memBuf + mOutputMetadataLength), buffer, bytes);
        buf.bytesToWrite = bytes;
        if (bytes)
            mInputMemFilledQueue[0].push_back(buf);
        mInputMemMutex.unlock();

        ALOGV("PCM write start");
        pcm * local_handle = (struct pcm *)mCompreRxHandle->handle;
        // Set the channel status after first frame decode/transcode and for change
        // in sample rate or channel mode as we close and open the device again
        if (bytes < (local_handle->period_size - mOutputMetadataLength )) {
            if(!((bytes != 0) && (mUseMS11Decoder == true))) {
                ALOGD("Last buffer case");
                mReachedExtractorEOS = true;
            }
        }
        {
            Mutex::Autolock autoLock(mSyncLock);
            n = pcm_write(local_handle, buf.memBuf, local_handle->period_size);
        }
        // write to second handle if present
        if (mSecCompreRxHandle) {
                mInputMemMutex.lock();
                it = mInputMemEmptyQueue[1].begin();
                buf = *it;
                if (bytes)
                    mInputMemEmptyQueue[1].erase(it);
                memcpy(buf.memBuf, &mOutputMetadataTunnel, mOutputMetadataLength);
                ALOGD("Copy Metadata2 = %d, bytes = %d", mOutputMetadataLength, bytes);
                memcpy(((uint8_t *)buf.memBuf + mOutputMetadataLength), buffer, bytes);
                buf.bytesToWrite = bytes;
                if (bytes)
                    mInputMemFilledQueue[1].push_back(buf);
                mInputMemMutex.unlock();

                ALOGV("PCM write start second compressed device");
                    local_handle = (struct pcm *)mSecCompreRxHandle->handle;
                {
                    Mutex::Autolock autoLock(mSyncLock);
                    n = pcm_write(local_handle, buf.memBuf, local_handle->period_size);
                }
        }
        if (bytes < (local_handle->period_size - mOutputMetadataLength )) {
            //TODO : Is this code reqd - start seems to fail?
            if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_START) < 0)
                ALOGE("AUDIO Start failed");
            else
                local_handle->start = 1;
            if(mSecCompreRxHandle){
                // last handle used was for secCompreRx, need to set handle to CompreRx
                local_handle = (struct pcm *)mCompreRxHandle->handle;
                if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_START) < 0)
                    ALOGE("AUDIO Start failed");
                else
                    local_handle->start = 1;
                }
        }
        ALOGV("PCM write complete");
        return n;
}

void AudioSessionOutALSA::bufferAlloc(alsa_handle_t *handle, int bufIndex) {
    void  *mem_buf = NULL;
    int i = 0;

    struct pcm * local_handle = (struct pcm *)handle->handle;
    int32_t nSize = local_handle->period_size;
    mInputMemEmptyQueue[bufIndex].clear();
    mInputBufPool[bufIndex].clear();
    mInputMemFilledQueue[bufIndex].clear();
    ALOGV("number of input buffers = %d", mInputBufferCount);
    ALOGV("memBufferAlloc calling with required size %d", nSize);
    for (i = 0; i < mInputBufferCount; i++) {
        mem_buf = (int32_t *)local_handle->addr + (nSize * i/sizeof(int));
        BuffersAllocated buf(mem_buf, nSize);
        //memset(buf.memBuf, 0x0, nSize);
        mInputMemEmptyQueue[bufIndex].push_back(buf);
        mInputBufPool[bufIndex].push_back(buf);
        ALOGD("The MEM that is allocated - buffer is %x",\
            (unsigned int)mem_buf);
    }
}

void AudioSessionOutALSA::bufferDeAlloc(int bufIndex) {
     ALOGD("Removing input buffer from Buffer Pool ");
     mInputMemFilledQueue[bufIndex].clear();
     mInputMemEmptyQueue[bufIndex].clear();
     mInputBufPool[bufIndex].clear();
}

void AudioSessionOutALSA::requestAndWaitForEventThreadExit() {

    if (!mEventThreadAlive)
        return;
    mEventMutex.lock();
    mKillEventThread = true;
    if(mEfd != -1) {
        ALOGE("Writing to mEfd %d",mEfd);
        uint64_t writeValue = KILL_EVENT_THREAD;
        sys_write::lib_write(mEfd, &writeValue, sizeof(uint64_t));
    }
    mEventCv.signal();
    mEventMutex.unlock();
    pthread_join(mEventThread,NULL);
    ALOGD("event thread killed");
}

void * AudioSessionOutALSA::eventThreadWrapper(void *me) {
    static_cast<AudioSessionOutALSA *>(me)->eventThreadEntry();
    return NULL;
}

void  AudioSessionOutALSA::eventThreadEntry() {

    int rc = 0;
    int err_poll = 0;
    int avail = 0;
    int i = 0;
    struct pcm * local_handle = NULL;
    struct pcm * local_handle2 = NULL;
    int timeout = -1;
    bool freeBuffer = false;
    bool mCompreCBk = false;
    bool mSecCompreCBk = false;
    struct snd_timer_tread rbuf[4];
    mPostedEOS = false;
    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"HAL Audio EventThread", 0, 0, 0);
    mEventMutex.lock();
    if(!mKillEventThread){
       ALOGV("eventThreadEntry wait for signal \n");
       mEventCv.wait(mEventMutex);
       ALOGV("eventThreadEntry ready to work \n");
    }
    mEventMutex.unlock();

    ALOGV("Allocating poll fd");
    if(!mKillEventThread) {
        ALOGV("Allocating poll fd");
        pfd[0].fd = mCompreRxHandle->handle->timer_fd;
        pfd[0].events = (POLLIN | POLLERR | POLLNVAL);
        ALOGV("Allocated poll fd");
        mEfd = eventfd(0,0);
        pfd[1].fd = mEfd;
        pfd[1].events = (POLLIN | POLLERR | POLLNVAL);
        if (mSecCompreRxHandle) {
            local_handle2 = (struct pcm *)mSecCompreRxHandle->handle;
            pfd[2].fd = local_handle2->timer_fd;
            pfd[2].events = (POLLIN | POLLERR | POLLNVAL);
            ALOGV("poll fd2 set to valid mSecCompreRxHandle fd =%d \n", pfd[2].fd);
        }
        else {
            pfd[2].fd = -1;
            pfd[2].events = (POLLIN | POLLERR | POLLNVAL);
                ALOGV("poll fd2 set to -1");
        }
    }
    while(!mKillEventThread && ((err_poll = poll(pfd, NUM_AUDIOSESSION_FDS, timeout)) >=0)) {
        ALOGV("pfd[0].revents =%d ", pfd[0].revents);
        ALOGV("pfd[1].revents =%d ", pfd[1].revents);
        ALOGV("pfd[2].revents =%d ", pfd[2].revents);
        if (err_poll == EINTR)
            ALOGE("Poll is interrupted");
        if ((pfd[1].revents & POLLERR) || (pfd[1].revents & POLLNVAL)) {
            ALOGE("POLLERR or INVALID POLL");
            pfd[1].revents = 0;
        }
        if ((pfd[1].revents & POLLIN)) {
            pfd[1].revents = 0;
            uint64_t readValue;
            read(mEfd, &readValue, sizeof(uint64_t));
        }
        while(!mKillEventThread && mRoutingLock.tryLock()) {
             usleep(100);
        }
        if(mKillEventThread) {
            mRoutingLock.unlock();
            continue;
        }
        mCompreCBk = false;
        mSecCompreCBk = false;
        if (pfd[0].revents & POLLIN ) {
            pfd[0].revents = 0;
            read(pfd[0].fd, rbuf, sizeof(struct snd_timer_tread) * 4 );
            mCompreCBk=true;
            freeBuffer=true;
            ALOGV("calling free compre buf");
        }
        if (mSecCompreRxHandle) {
            local_handle2 = (struct pcm *)mSecCompreRxHandle->handle;
            if (pfd[2].revents & POLLIN) {
                read(local_handle2->timer_fd, rbuf, sizeof(struct snd_timer_tread) * 4 );
                pfd[2].revents = 0;
                mSecCompreCBk=true;
                ALOGV("calling free sec compre buf");
                freeBuffer=true;
            }
        }

        if (freeBuffer && !mKillEventThread) {
            if (mTunnelPaused || mPostedEOS) {
                mRoutingLock.unlock();
                continue;
            }
            ALOGV("After an event occurs");
            if (mCompreCBk) {
                {
                    Mutex::Autolock autoLock(mSyncLock);
                    mCompreRxHandle->handle->sync_ptr->flags = (SNDRV_PCM_SYNC_PTR_AVAIL_MIN |
                                                                SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL);
                    sync_ptr(mCompreRxHandle->handle);
                }
                while(hw_ptr[0] < mCompreRxHandle->handle->sync_ptr->s.status.hw_ptr) {
                    mInputMemMutex.lock();
                    if (mInputMemFilledQueue[0].empty()) {
                        ALOGV("Filled queue1 is empty");
                        mInputMemMutex.unlock();
                        break;
                    }
                    BuffersAllocated buf = *(mInputMemFilledQueue[0].begin());
                    mInputMemFilledQueue[0].erase(mInputMemFilledQueue[0].begin());
                    ALOGV("mInputMemFilledQueue1 %d", mInputMemFilledQueue[0].size());

                    mInputMemEmptyQueue[0].push_back(buf);
                    mInputMemMutex.unlock();
                    hw_ptr[0] += mCompreRxHandle->bufferSize/(2*mCompreRxHandle->channels);

                    {
                        Mutex::Autolock autoLock(mSyncLock);
                        mCompreRxHandle->handle->sync_ptr->flags = (SNDRV_PCM_SYNC_PTR_AVAIL_MIN |
                                                                    SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL);
                        sync_ptr(mCompreRxHandle->handle);
                    }
                    ALOGE("hw_ptr1 = %lld status.hw_ptr1 = %ld appl_ptr = %ld", hw_ptr[0], mCompreRxHandle->handle->sync_ptr->s.status.hw_ptr,
                        mCompreRxHandle->handle->sync_ptr->c.control.appl_ptr);
                }
            }

            if (mSecCompreCBk) {
                {
                    Mutex::Autolock autoLock(mSyncLock);
                    mSecCompreRxHandle->handle->sync_ptr->flags = (SNDRV_PCM_SYNC_PTR_AVAIL_MIN |
                                                                   SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL);
                    sync_ptr(mSecCompreRxHandle->handle);
                }
                while (hw_ptr[1] < mSecCompreRxHandle->handle->sync_ptr->s.status.hw_ptr) {
                    mInputMemMutex.lock();
                    if (mInputMemFilledQueue[1].empty()) {
                        ALOGV("Filled queue2 is empty");
                        mInputMemMutex.unlock();
                        break;
                    }
                    BuffersAllocated buf = *(mInputMemFilledQueue[1].begin());
                    mInputMemFilledQueue[1].erase(mInputMemFilledQueue[1].begin());
                    ALOGV("mInputMemFilledQueue2 %d", mInputMemFilledQueue[1].size());

                   mInputMemEmptyQueue[1].push_back(buf);
                   mInputMemMutex.unlock();
                   hw_ptr[1] += mSecCompreRxHandle->bufferSize/(2*mSecCompreRxHandle->channels);
                   {
                        Mutex::Autolock autoLock(mSyncLock);
                        mSecCompreRxHandle->handle->sync_ptr->flags = (SNDRV_PCM_SYNC_PTR_AVAIL_MIN |
                                                                        SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL);
                        sync_ptr(mSecCompreRxHandle->handle);
                   }
                   ALOGE("hw_ptr2 = %lld status.hw_ptr2 = %ld appl_ptr = %ld", hw_ptr[1], mSecCompreRxHandle->handle->sync_ptr->s.status.hw_ptr,
                        mSecCompreRxHandle->handle->sync_ptr->c.control.appl_ptr);
                }
            }

            freeBuffer = false;
            bool unlockMlock = true;
            while(mLock.tryLock()) {
                if(mKillEventThread) {
                    unlockMlock = false;
                    break;
                }
                usleep(100);
            }

            if(mKillEventThread) {
                mRoutingLock.unlock();
                if(unlockMlock)
                   mLock.unlock();
                continue;
            }
            if (mInputMemFilledQueue[0].empty() && mInputMemFilledQueue[1].empty() && mReachedExtractorEOS) {
                ALOGV("Posting the EOS to the MPQ player");
                //post the EOS To MPQ player
                if (mObserver) {
                     if (mPaused) {
                         ALOGD("Resume the driver before posting for the DRAIN");
                         resume_l();
                     }
                     ALOGD("Audio Drain1 DONE ++");
                     if ( ioctl(mCompreRxHandle->handle->fd, SNDRV_COMPRESS_DRAIN) < 0 )
                         ALOGE("Audio Drain1 failed");
                     ALOGD("Audio Drain1 DONE --");
                     if (mSecCompreRxHandle) {
                         ALOGD("Audio Drain2 DONE ++");
                         if ( ioctl(mSecCompreRxHandle->handle->fd, SNDRV_COMPRESS_DRAIN) < 0 )
                             ALOGE("Audio Drain2 failed");
                         ALOGD("Audio Drain2 DONE --");
                     }
                     mObserver->postEOS(0);
                     mPostedEOS = true;
                }
            }
            else
                mWriteCv.signal();
            mLock.unlock();
         }
         mRoutingLock.unlock();
    }
    mEventThreadAlive = false;
    if (mEfd != -1) {
        close(mEfd);
        mEfd = -1;
    }

    ALOGD("Event Thread is dying.");
    return;

}

void AudioSessionOutALSA::createThreadsForTunnelDecode() {
    mKillEventThread = false;
    ALOGD("Creating Event Thread");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if(!(pthread_create(&mEventThread, &attr, eventThreadWrapper, this))){
       ALOGD("Event Thread created");
       mEventThreadAlive=true;
    }
}

status_t AudioSessionOutALSA::start()
{
    Mutex::Autolock autoLock(mLock);
    status_t err = NO_ERROR;

    if(mPaused) {
        if (mRouteAudioToA2dp) {
            ALOGD("startA2dpPlayback_l - resume - usecase %x", mA2dpUseCase);
            err = mParent->startA2dpPlayback_l(mA2dpUseCase);
            if(err) {
                ALOGE("startA2dpPlayback_l from resume return error = %d", err);
                return err;
            }
        }
        err = resume_l();
        return err;
    }
    
    // 1. Set the absolute time stamp
    // ToDo: We need the ioctl from driver to set the time stamp
    // 2. Signal the driver to start rendering data if threshold is not to be honoured.
    return err;
}
status_t AudioSessionOutALSA::pause()
{
    Mutex::Autolock autoLock(mLock);
    status_t err = NO_ERROR;
    err = pause_l();

    if(err) {
        ALOGE("pause returned error");
        return err;
    }
#ifdef DEBUG
    char debugString[] = "Playback Paused";
    mFpDumpPCMOutput = fopen("/data/pcm_output.raw","a");
    if(mFpDumpPCMOutput != NULL) {
        fwrite(debugString, 1, sizeof(debugString), mFpDumpPCMOutput);
        fclose(mFpDumpPCMOutput);
    }
    mFpDumpInput = fopen("/data/input.raw","a");
    if(mFpDumpInput != NULL) {
        fwrite(debugString, 1, sizeof(debugString), mFpDumpInput);
        fclose(mFpDumpInput);
    }
#endif
    if (mRouteAudioToA2dp) {
        ALOGD("Pause - suspendA2dpPlayback_l - usecase %x", mA2dpUseCase);
        err = mParent->suspendA2dpPlayback_l(mA2dpUseCase);
        if(err != NO_ERROR) {
            ALOGE("suspend Proxy from Pause returned error = %d",err);
            return err;
        }
    }
    return err;
}
status_t AudioSessionOutALSA::pause_l()
{
    ALOGE("pause");
    mPaused = true;
    if(mPcmRxHandle) {
        if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
            ALOGE("PAUSE failed for use case %s", mPcmRxHandle->useCase);
        }
    }
    if(mCompreRxHandle) {
        if (ioctl(mCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
            ALOGE("PAUSE failed on use case %s", mCompreRxHandle->useCase);
        }
        if (mSecCompreRxHandle){
                if (ioctl(mSecCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0)
                ALOGE("PAUSE failed on use case %s", mSecCompreRxHandle->useCase);
        }
        mTunnelPaused = true;
    }
    return NO_ERROR;
}

status_t AudioSessionOutALSA::resetBufferQueue()
{
        mInputMemMutex.lock();
        mInputMemFilledQueue[0].clear();
        mInputMemEmptyQueue[0].clear();
        List<BuffersAllocated>::iterator it = mInputBufPool[0].begin();
        for (;it!=mInputBufPool[0].end();++it) {
            memset((*it).memBuf, 0x0, (*it).memBufsize);
            mInputMemEmptyQueue[0].push_back(*it);
        }
        ALOGV("Transferred all the buffers from response queue1 to\
            request queue1 to handle seek");
        if (mSecCompreRxHandle) {
                mInputMemFilledQueue[1].clear();
                mInputMemEmptyQueue[1].clear();
                it = mInputBufPool[1].begin();
                for (;it!=mInputBufPool[1].end();++it) {
                    memset((*it).memBuf, 0x0, (*it).memBufsize);
                    mInputMemEmptyQueue[1].push_back(*it);
                }
                ALOGV("Transferred all the buffers from response queue2 to\
                    request queue2 to handle seek");
        }
        mReachedExtractorEOS = false;
        mPostedEOS = false;
        mInputMemMutex.unlock();
        return NO_ERROR;
}

status_t AudioSessionOutALSA::drainTunnel()
{
    status_t err = OK;
    if (!mCompreRxHandle)
        return -1;
    mCompreRxHandle->handle->start = 0;
    err = pcm_prepare(mCompreRxHandle->handle);
    if(err != OK) {
        ALOGE("pcm_prepare1 -seek = %d",err);
        //Posting EOS
        if (mObserver)
            mObserver->postEOS(0);
    }
    ALOGV("drainTunnel Empty Queue1 size() = %d,"
       " Filled Queue1 size() = %d ",\
        mInputMemEmptyQueue[0].size(),\
        mInputMemFilledQueue[0].size());
    {
        Mutex::Autolock autoLock(mSyncLock);
        mCompreRxHandle->handle->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
        sync_ptr(mCompreRxHandle->handle);
    }
    hw_ptr[0] = 0;
    ALOGV("appl_ptr1= %d",\
        (int)mCompreRxHandle->handle->sync_ptr->c.control.appl_ptr);
    if ((mSecCompreRxHandle))
    {
            mSecCompreRxHandle->handle->start = 0;
            err = pcm_prepare(mSecCompreRxHandle->handle);
            if(err != OK) {
                ALOGE("pcm_prepare2 -seek = %d",err);
                //Posting EOS
                if (mObserver)
                    mObserver->postEOS(0);
            }
        ALOGV("drainTunnel Empty Queue2 size() = %d,"
               " Filled Queue2 size() = %d ",\
                mInputMemEmptyQueue[1].size(),\
                mInputMemFilledQueue[1].size());
        {
            Mutex::Autolock autoLock(mSyncLock);
            mSecCompreRxHandle->handle->sync_ptr->flags =
                SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
           sync_ptr(mSecCompreRxHandle->handle);
        }
        hw_ptr[1] = 0;
        ALOGV("appl_ptr2= %d",\
        (int)mSecCompreRxHandle->handle->sync_ptr->c.control.appl_ptr);
    }
    resetBufferQueue();
    ALOGV("Reset, drain and prepare completed");
    return err;
}

status_t AudioSessionOutALSA::flush()
{
    Mutex::Autolock autoLock(mLock);
    ALOGD("AudioSessionOutALSA::flush E");
    int err = 0;
    if (mCompreRxHandle) {
        struct pcm * local_handle = mCompreRxHandle->handle;
        ALOGV("Paused case, %d",mTunnelPaused);
        if (!mTunnelPaused && mCompreRxHandle->handle->start == 1) {
            if ((err = ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1)) < 0) {
                ALOGE("Audio Pause failed");
                return err;
            }
            if (mSecCompreRxHandle && (mSecCompreRxHandle->handle->start == 1)) {
                local_handle = mSecCompreRxHandle->handle;
                if ((err = ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1)) < 0) {
                        ALOGE("Audio Pause failed");
                        return err;
                }
            }
            if ((err = drainTunnel()) != OK)
                return err;
        } else {
            if (mTunnelPaused)
                mTunnelSeeking = true;
            else {
                 ALOGW("Audio is not started yet, clearing Queue without drain");
                 mCompreRxHandle->handle->sync_ptr->c.control.appl_ptr = 0;
                 if (mSecCompreRxHandle)
                     mSecCompreRxHandle->handle->sync_ptr->c.control.appl_ptr = 0;

                 {
                     Mutex::Autolock autoLock(mSyncLock);

                     mCompreRxHandle->handle->sync_ptr->flags = 0;
                     sync_ptr(mCompreRxHandle->handle);
                     if (mSecCompreRxHandle) {
                         mSecCompreRxHandle->handle->sync_ptr->flags = 0;
                         sync_ptr(mSecCompreRxHandle->handle);
                     }
                 }
                 resetBufferQueue();
            }
        }
        mSkipWrite = true;
        mWriteCv.signal();
    }
    if(mPcmRxHandle) {
        if(!mPaused) {
            ALOGE("flush(): Move to pause state if flush without pause");
            if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0)
                ALOGE("flush(): Audio Pause failed");
        }
        pcm_prepare(mPcmRxHandle->handle);
        ALOGV("flush(): prepare completed");
    }
    mFrameCountMutex.lock();
    mFrameCount = 0;
    mFrameCountMutex.unlock();
    if(mBitstreamSM)
       mBitstreamSM->resetBitstreamPtr();
    if(mUseMS11Decoder == true) {
      mMS11Decoder->flush();
      if (mFormat == AUDIO_FORMAT_AC3 || mFormat == AUDIO_FORMAT_EAC3 ||
            mFormat == AUDIO_FORMAT_AAC || mFormat == AUDIO_FORMAT_AAC_ADIF){
         delete mMS11Decoder;
         mMS11Decoder = new SoftMS11;
         int32_t format_ms11;
         if(mMS11Decoder->initializeMS11FunctionPointers() == false){
            ALOGE("Could not resolve all symbols Required for MS11");
            delete mMS11Decoder;
            return -1;
         }
         if(mFormat == AUDIO_FORMAT_AAC || mFormat == AUDIO_FORMAT_AAC_ADIF){
            format_ms11 = FORMAT_DOLBY_PULSE_MAIN;
         } else {
            format_ms11 = FORMAT_DOLBY_DIGITAL_PLUS_MAIN;
         }
         /*Check if flush was issued for a widevine clip, having ADTS header, if yes then configure MS11
           in Fileplayback mode OFF*/
         if(mADTSHeaderPresent == true){
            if(mMS11Decoder->setUseCaseAndOpenStream(format_ms11,mChannels, mSampleRate,false)){
               ALOGE("SetUseCaseAndOpen MS11 failed");
               delete mMS11Decoder;
               return -1;
            }
         } else {
            if(mMS11Decoder->setUseCaseAndOpenStream(format_ms11,mChannels, mSampleRate)){
               ALOGE("SetUseCaseAndOpen MS11 failed");
               delete mMS11Decoder;
               return -1;
            }
            if(mFirstAACBuffer != NULL && mFirstAACBufferLength != 0 &&
               format_ms11 == FORMAT_DOLBY_PULSE_MAIN &&
               mMS11Decoder->setAACConfig((unsigned char *)mFirstAACBuffer, mFirstAACBufferLength) == true)
               mAacConfigDataSet = true;
         }
      }
    }
#ifdef DEBUG
    char debugString[] = "Playback Flushd";
    mFpDumpPCMOutput = fopen("/data/pcm_output.raw","a");
    if(mFpDumpPCMOutput != NULL) {
        fwrite(debugString, 1, sizeof(debugString), mFpDumpPCMOutput);
        fclose(mFpDumpPCMOutput);
    }
    mFpDumpInput = fopen("/data/input.raw","a");
    if(mFpDumpInput != NULL) {
        fwrite(debugString, 1, sizeof(debugString), mFpDumpInput);
        fclose(mFpDumpInput);
    }
#endif
    ALOGD("AudioSessionOutALSA::flush X");
    return NO_ERROR;
}

status_t AudioSessionOutALSA::resume_l()
{
    if(mPcmRxHandle) {
        if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,0) < 0) {
            ALOGE("RESUME failed for use case %s", mPcmRxHandle->useCase);
        }
    }
    if(mCompreRxHandle) {
        if (mTunnelSeeking) {
            drainTunnel();
            mTunnelSeeking = false;
        } else if (mTunnelPaused) {
            if (mCompreRxHandle->handle->start != 1) {
                if(ioctl(mCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_START) < 0)
                    ALOGE("AUDIO Start failed");
                else
                    mCompreRxHandle->handle->start = 1;
            } else if (ioctl(mCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,0) < 0)
                ALOGE("RESUME failed on use case %s", mCompreRxHandle->useCase);

            if (mSecCompreRxHandle) {
                if (mSecCompreRxHandle->handle->start != 1) {
                    if(ioctl(mSecCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_START) < 0)
                        ALOGE("AUDIO Start failed");
                    else
                        mSecCompreRxHandle->handle->start = 1;
                } else if (ioctl(mSecCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,0) < 0)
                    ALOGE("RESUME failed on use case %s", mSecCompreRxHandle->useCase);
            }
        }
        mTunnelPaused = false;
    }
    mPaused = false;
#ifdef DEBUG
    char debugString[] = "Playback Resumd";
    mFpDumpPCMOutput = fopen("/data/pcm_output.raw","a");
    if(mFpDumpPCMOutput != NULL) {
        fwrite(debugString, 1, sizeof(debugString), mFpDumpPCMOutput);
        fclose(mFpDumpPCMOutput);
    }
    mFpDumpInput = fopen("/data/input.raw","a");
    if(mFpDumpInput != NULL) {
        fwrite(debugString, 1, sizeof(debugString), mFpDumpInput);
        fclose(mFpDumpInput);
    }
#endif
    return NO_ERROR;
}

status_t AudioSessionOutALSA::stop()
{
    Mutex::Autolock autoLock(mLock);
    ALOGV("AudioSessionOutALSA- stop");
    // close all the existing PCM devices
    // ToDo: How to make sure all the data is rendered before closing
    mSkipWrite = true;
    mWriteCv.signal();

    //TODO: This might need to be Locked using Parent lock
    reset();

    if (mRouteAudioToA2dp) {
        ALOGD("stop - suspendA2dpPlayback_l - usecase %x", mA2dpUseCase);
        status_t err = mParent->suspendA2dpPlayback_l(mA2dpUseCase);
        if(err) {
            ALOGE("stop-suspendA2dpPlayback- return err = %d", err);
            return err;
        }
        mA2dpUseCase = AudioHardwareALSA::USECASE_NONE;
    }

    return NO_ERROR;
}

status_t AudioSessionOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioSessionOutALSA::standby()
{
    Mutex::Autolock autoLock(mLock);
    ALOGD("standby");

    mSkipWrite = true;
    mWriteCv.signal();

    //TODO: This might need to be Locked using Parent lock
    reset();

    if (mRouteAudioToA2dp) {
         ALOGD("standby - stopA2dpPlayback_l - usecase %x", mA2dpUseCase);
         status_t err = mParent->stopA2dpPlayback_l(mA2dpUseCase);
         if(err) {
             ALOGE("standby-stopA2dpPlayback- return er = %d", err);
         }
         mRouteAudioToA2dp = false;
    }
    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }
    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioSessionOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    int32_t err;
    if(mPcmRxHandle && mPcmRxHandle->handle){
         mPcmRxHandle->handle->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL|
                                                 SNDRV_PCM_SYNC_PTR_AVAIL_MIN|
                                                 SNDRV_PCM_SYNC_PTR_HWSYNC;

         err = sync_ptr(mPcmRxHandle->handle);

         if(err == NO_ERROR){
              return USEC_TO_MSEC(((((uint64_t)(mPcmRxHandle->handle->sync_ptr->c.control.appl_ptr -
                     mPcmRxHandle->handle->sync_ptr->s.status.hw_ptr)) * 1000000 )/(uint64_t)mSampleRate));
         }
         else
              return 0;
    }
    else
        return 0;
}

status_t AudioSessionOutALSA::setObserver(void *observer)
{
    ALOGV("Registering the callback \n");
    mObserver = reinterpret_cast<AudioEventObserver *>(observer);
    return NO_ERROR;
}

status_t AudioSessionOutALSA::getNextWriteTimestamp(int64_t *timeStamp)
{
    ALOGV("getTimeStamp \n");

    struct snd_compr_tstamp tstamp;
    if (!timeStamp)
        return -1;

    *timeStamp = -1;
    if (mCompreRxHandle && mUseTunnelDecoder) {
        Mutex::Autolock autoLock(mLock);
        tstamp.timestamp = -1;
        if ( mCompreRxHandle==NULL ||  ioctl( mCompreRxHandle->handle->fd, SNDRV_COMPRESS_TSTAMP, &tstamp)){
            ALOGE("Failed SNDRV_COMPRESS_TSTAMP\n");
            return -1;
        } else {
            ALOGV("Timestamp returned = %lld\n", tstamp.timestamp);
            *timeStamp = tstamp.timestamp;
            return NO_ERROR;
        }
    } else if(mMS11Decoder) {
        if(mPcmRxHandle) {
            mFrameCountMutex.lock();
            *timeStamp = -(uint64_t)(latency()*1000) + (((uint64_t)(((int64_t)(
                        (mFrameCount * mPcmRxHandle->periodSize)/ (2*mChannels)))
                     * 1000000)) / mSampleRate);
            mFrameCountMutex.unlock();
        } else if(mCompreRxHandle){
            Mutex::Autolock autoLock(mLock);
            if ( mCompreRxHandle==NULL ||  ioctl(mCompreRxHandle->handle->fd, SNDRV_COMPRESS_TSTAMP,
                      &tstamp)) {
                ALOGE("Failed SNDRV_COMPRESS_TSTAMP\n");
                return -1;
            } else {
                *timeStamp = tstamp.timestamp;
            }
        } else {
            *timeStamp = 0;
        }
        ALOGV("Timestamp returned = %lld\n",*timeStamp);
    } else {
        int bitFormat = audio_bytes_per_sample((audio_format_t)mFormat);
        if(mPcmRxHandle && mSampleRate && mChannels && bitFormat) {
            mFrameCountMutex.lock();
            *timeStamp = -(uint64_t)(latency()*1000) + (((uint64_t)(((int64_t)(
                    (mFrameCount * mPcmRxHandle->periodSize)/ (mChannels*(bitFormat))))
                     * 1000000)) / mSampleRate);
            mFrameCountMutex.unlock();
        }
        else
            *timeStamp = 0;
        ALOGV("Timestamp returned = %lld\n",*timeStamp);
    }
    return NO_ERROR;
}

// return the number of audio frames written by the audio dsp to DAC since
// the output has exited standby
status_t AudioSessionOutALSA::getRenderPosition(uint32_t *dspFrames)
{
    Mutex::Autolock autoLock(mLock);
    mFrameCountMutex.lock();
    *dspFrames = mFrameCount;
    mFrameCountMutex.unlock();
    return NO_ERROR;
}

status_t AudioSessionOutALSA::openDevice(char *useCase, bool bIsUseCase, int devices)
{
    alsa_handle_t alsa_handle;
    status_t status = NO_ERROR;
    ALOGD("openDevice: E");
    alsa_handle.module      = mALSADevice;
    alsa_handle.bufferSize  = mInputBufferSize * mInputBufferCount;
    alsa_handle.periodSize  = mInputBufferSize;
    alsa_handle.devices     = devices;
    alsa_handle.activeDevice= devices;
    alsa_handle.handle      = 0;
    alsa_handle.format      = (mFormat == AUDIO_FORMAT_PCM_16_BIT ? SNDRV_PCM_FORMAT_S16_LE : mFormat);
    alsa_handle.channels    = mChannels;
    alsa_handle.sampleRate  = mSampleRate;
    alsa_handle.mode        = mParent->mode();
    alsa_handle.latency     = PLAYBACK_LATENCY;
    alsa_handle.rxHandle    = 0;
    alsa_handle.ucMgr       = mUcMgr;
    alsa_handle.timeStampMode = SNDRV_PCM_TSTAMP_NONE;

    if ((!strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                          strlen(SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
        (!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL1,
                          strlen(SND_USE_CASE_MOD_PLAY_TUNNEL1)))) ||
        (!strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                          strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2)) ||
        (!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                          strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2))) ||
        (!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL3,
                          strlen(SND_USE_CASE_MOD_PLAY_TUNNEL3))))) {
        if (mUseMS11Decoder == true)
            alsa_handle.type = COMPRESSED_PASSTHROUGH_FORMAT;
        else if (mFormat == AUDIO_FORMAT_DTS || mFormat == AUDIO_FORMAT_DTS_LBR) {
            if ((mSpdifFormat == COMPRESSED_FORMAT && mHdmiFormat == COMPRESSED_FORMAT)
                || (mUseDualTunnel == true && devices == mSecDevices))
                alsa_handle.type = COMPRESSED_PASSTHROUGH_FORMAT;
            else
                alsa_handle.type = COMPRESSED_FORMAT;
        }
        else
            alsa_handle.type = COMPRESSED_FORMAT;

        if (mSampleRate <= 16000) {
            ALOGD("low frequency in tunnel mode, reducing buff size to 1/4th");
            // make sure that buffersize is 32 byte aligned
            // and greater minimum value supported in driver
            mInputBufferSize = 32*((int)(mInputBufferSize + 2*32)/(int)(4*32));
            alsa_handle.bufferSize = mInputBufferSize * mInputBufferCount;
            alsa_handle.periodSize = mInputBufferSize;
        }
    }
    else
       alsa_handle.type = PCM_FORMAT;
    strlcpy(alsa_handle.useCase, useCase, sizeof(alsa_handle.useCase));

    if (mALSADevice->setUseCase(&alsa_handle, bIsUseCase))
        return NO_INIT;
    status = mALSADevice->open(&alsa_handle);
    if(status != NO_ERROR) {
        ALOGE("Could not open the ALSA device for use case %s", alsa_handle.useCase);
        mALSADevice->close(&alsa_handle);
    } else{
        mParent->mDeviceList.push_back(alsa_handle);
    }
    ALOGD("openDevice: X");
    return status;
}

status_t AudioSessionOutALSA::closeDevice(alsa_handle_t *pHandle)
{
    status_t status = NO_ERROR;
    ALOGV("closeDevice");
    if(pHandle) {
        ALOGV("useCase %s", pHandle->useCase);
        status = mALSADevice->close(pHandle);
    }
    return status;
}

void AudioSessionOutALSA::copyBuffers(alsa_handle_t *destHandle, List<BuffersAllocated> filledQueue) {

    int32_t nSize = destHandle->periodSize;
    List<BuffersAllocated>::iterator it;
    int index;
    for(it = filledQueue.begin(), index = 0; it != filledQueue.end(); it++, index++) {
        BuffersAllocated buf = *it;
        void * mem = (int32_t *)destHandle->handle->addr + (index*nSize/sizeof(int));
        memcpy(mem, buf.memBuf, nSize);
    }
    return;
}

void AudioSessionOutALSA::initFilledQueue(alsa_handle_t *handle, int queueIndex, int consumedIndex) {

    struct pcm * local_handle = (struct pcm *)handle->handle;
    int32_t nSize = local_handle->period_size;
    void  *mem_buf = NULL;
    int i;
    for (i = 0; i < consumedIndex; i++) {
        mem_buf = (int32_t *)local_handle->addr + ((nSize * i)/sizeof(int));
        BuffersAllocated buf(mem_buf, nSize);
        mInputMemFilledQueue[queueIndex].push_back(buf);
        mInputBufPool[queueIndex].push_back(buf);
    }

    for (i = 0; i < mInputBufferCount - consumedIndex; i++) {
        mem_buf = (int32_t *)local_handle->addr + ((nSize * (i + consumedIndex))/sizeof(int));
        BuffersAllocated buf(mem_buf, nSize);
        memset(buf.memBuf, 0x0, nSize);
        mInputMemEmptyQueue[queueIndex].push_back(buf);
        mInputBufPool[queueIndex].push_back(buf);
    }

}


status_t AudioSessionOutALSA::doRouting(int devices)
{
    status_t status = NO_ERROR;
    char *use_case;
    bool stopA2DP = false;
    ALOGV("doRouting: devices 0x%x, mDevices = 0x%x", devices,mDevices);
    Mutex::Autolock autoLock(mParent->mLock);
    Mutex::Autolock autoLock1(mRoutingLock);

    if(devices & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        ALOGV("doRouting - Capture from Proxy");
        devices &= ~AudioSystem::DEVICE_OUT_ALL_A2DP;
        devices &= ~AudioSystem::DEVICE_OUT_SPDIF;
//NOTE: TODO - donot remove SPDIF device for A2DP device switch
        devices |=  AudioSystem::DEVICE_OUT_PROXY;
        mRouteAudioToA2dp = true;

    } else if(!(devices & AudioSystem::DEVICE_OUT_ALL_A2DP)) {
        if(mRouteAudioToA2dp) {
            mRouteAudioToA2dp = false;
            stopA2DP = true;
        }
        if(devices & AudioSystem::DEVICE_OUT_PROXY)
            devices &= ~AudioSystem::DEVICE_OUT_SPDIF;
    }
    if(devices == mDevices) {
        ALOGW("Return same device ");
        if(stopA2DP == true) {
            ALOGD("doRouting-stopA2dpPlayback_l- usecase %x", mA2dpUseCase);
            status = mParent->stopA2dpPlayback_l(mA2dpUseCase);
        }
        return status;
    }
    mDevices = devices;

    updateRoutingFlags(devices);
    if(mUseTunnelDecoder) {
        if(mCompreRxHandle) {
            status = setPlaybackFormat();
            if(status != NO_ERROR)
                return BAD_VALUE;
            if(mSecDevices) {
                if(mSecCompreRxHandle == NULL) {
                    Mutex::Autolock autoLock(mLock);
                    uint64_t writeValue = POLL_FD_MODIFIED;
                    sys_write::lib_write(mEfd, &writeValue, sizeof(uint64_t));
                    status = openTunnelDevice(mSecDevices);
                    if(status != NO_ERROR) {
                        ALOGE("Error opening Sec Tunnel devices in doRouting");
                        return BAD_VALUE;
                    }
                    bufferDeAlloc(PASSTHRUQUEUEINDEX);
                    {
                        Mutex::Autolock autoLock(mInputMemMutex);
                        copyBuffers(mSecCompreRxHandle, mInputMemFilledQueue[0]);
                        initFilledQueue(mSecCompreRxHandle, PASSTHRUQUEUEINDEX, mInputMemFilledQueue[0].size());
                    }
                    struct pcm * local_handle = (struct pcm *)mSecCompreRxHandle->handle;
                    pfd[2].fd = local_handle->timer_fd;
                    pfd[2].events = (POLLIN | POLLERR | POLLNVAL);
                    pfd[2].revents = 0;
                    ALOGV("poll fd2 set to valid mSecCompreRxHandle fd =%d\n", pfd[2].fd);

                    /*Init the appl pointer and sync it*/
                    local_handle->sync_ptr->c.control.appl_ptr =
                                     (mInputMemFilledQueue[0].size() * mSecCompreRxHandle->bufferSize)/
                                     (2*mCompreRxHandle->channels);
                    local_handle->sync_ptr->flags = 0;

                    sync_ptr(local_handle);
                    if(!mTunnelPaused) {
                        if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_START) < 0)
                            ALOGE("AUDIO Start failed");
                        else
                            local_handle->start = 1;
                    }
                }
            } else if(mSecCompreRxHandle != NULL) {
                Mutex::Autolock autoLock(mLock);
                uint64_t writeValue = POLL_FD_MODIFIED;
                sys_write::lib_write(mEfd, &writeValue, sizeof(uint64_t));
                status = closeDevice(mSecCompreRxHandle);
                if(status != NO_ERROR) {
                    ALOGE("Error closing the pcm device in doRouting");
                    return BAD_VALUE;
                }
                mSecCompreRxHandle = NULL;
                mUseDualTunnel = false;
                pfd[2].fd = -1;
                pfd[2].revents = 0;
                ALOGV("poll fd2 set to -1");
                bufferDeAlloc(PASSTHRUQUEUEINDEX);
            }
            /*Device handling for the DTS transcode*/
            if (!mDtsTranscode && mTranscodeHandle) {
                Mutex::Autolock autoLock(mLock);
                closeDevice(mTranscodeHandle);
                free(mTranscodeHandle);
                mTranscodeHandle = NULL;
                struct snd_compr_routing params;
                params.session_type = TRANSCODE_SESSION;
                params.operation = DISCONNECT_STREAM;
                if (ioctl(mCompreRxHandle->handle->fd, SNDRV_COMPRESS_SET_ROUTING, &params) < 0) {
                    ALOGE("disconnect stream failed");
                }
            }
            if ((mDtsTranscode && mTranscodeHandle == NULL)) {
                Mutex::Autolock autoLock(mLock);
                status = openTunnelDevice(mTranscodeDevices);
                if(status != NO_ERROR) {
                    ALOGE("Error opening Transocde device in doRouting");
                    return BAD_VALUE;
                }
                struct snd_compr_routing params;
                params.session_type = TRANSCODE_SESSION;
                params.operation = CONNECT_STREAM;
                if (ioctl(mCompreRxHandle->handle->fd, SNDRV_COMPRESS_SET_ROUTING, &params) < 0) {
                    ALOGE("Connect stream failed");
                }
            }
            mALSADevice->switchDeviceUseCase(mCompreRxHandle,
                  devices & ~mSecDevices & ~mTranscodeDevices, mParent->mode());
        }
    } else {
        /*
           Handles the following
           device switch    -- pcm out --> pcm out
           open pcm device  -- compressed out --> pcm out

           device switch       -- compressed out --> compressed out
           create thread and
           open tunnel device  -- pcm out --> compressed out
           exit thread and
           close tunnel device -- compressed out --> pcm out
        */
        int pcmDevices = 0;
        int comprDevices = 0;

        if(mSpdifFormat == COMPRESSED_FORMAT)
            comprDevices |= AudioSystem::DEVICE_OUT_SPDIF;
        if(mHdmiFormat == COMPRESSED_FORMAT)
            comprDevices |= AudioSystem::DEVICE_OUT_AUX_DIGITAL;

        pcmDevices = devices & ~comprDevices;
 
        status = setPlaybackFormat();
        if(status != NO_ERROR)
            return BAD_VALUE;

        if((mSpdifFormat == COMPRESSED_FORMAT) ||
           (mHdmiFormat == COMPRESSED_FORMAT)) {
           if (mCompreRxHandle == NULL) {
                Mutex::Autolock autoLock(mLock);
                status = openTunnelDevice(comprDevices);
                if(status != NO_ERROR) {
                   ALOGE("Tunnel device open failure in do routing");
                    return BAD_VALUE;
                }
                createThreadsForTunnelDecode();
            }
        } else {
            ALOGE("Clear the compr handle");
            requestAndWaitForEventThreadExit();
            Mutex::Autolock autoLock(mLock);
            status = closeDevice(mCompreRxHandle);
            if(status != NO_ERROR) {
                ALOGE("Error closing compr device in doRouting");
                return BAD_VALUE;
            }
            mCompreRxHandle = NULL;
            bufferDeAlloc(DECODEQUEUEINDEX);
       }

       if(mCompreRxHandle && comprDevices) {
           mALSADevice->switchDeviceUseCase(mCompreRxHandle,
                            comprDevices, mParent->mode());
       }
       if(pcmDevices && mPcmRxHandle) {
           mALSADevice->switchDeviceUseCase(mPcmRxHandle, pcmDevices,
                                       mParent->mode());
       }
 
    }

    if(stopA2DP){
        ALOGD("doRouting-stopA2dpPlayback_l- usecase %x", mA2dpUseCase);
        status = mParent->stopA2dpPlayback_l(mA2dpUseCase);
    } else if (mRouteAudioToA2dp) {
        alsa_handle_t *handle = NULL;
        if (mPcmRxHandle && (mPcmRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mPcmRxHandle;
        else if (mCompreRxHandle && (mCompreRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mCompreRxHandle;
        else if (mSecCompreRxHandle && (mSecCompreRxHandle->devices & AudioSystem::DEVICE_OUT_PROXY))
            handle = mSecCompreRxHandle;
        if (handle) {
            mA2dpUseCase = mParent->useCaseStringToEnum(handle->useCase);
            ALOGD("doRouting - startA2dpPlayback_l - usecase %x", mA2dpUseCase);
            status = mParent->startA2dpPlayback_l(mA2dpUseCase);
            if(status != NO_ERROR) {
                ALOGW("startA2dpPlayback_l returned = %d", status);
                mParent->stopA2dpPlayback_l(mA2dpUseCase);
                mRouteAudioToA2dp = false;
            }
        }
        if (handle == NULL || status != NO_ERROR) {
            mDevices = mCurDevice;
            if(mPcmRxHandle) {
                mALSADevice->switchDeviceUseCase(mPcmRxHandle, devices, mParent->mode());
            }
            if(mCompreRxHandle) {
                mALSADevice->switchDeviceUseCase(mCompreRxHandle, devices, mParent->mode());
            }
        }
    }
    if(status)
        mCurDevice = mDevices;
    ALOGD("doRouting status = %d ",status);
    return status;
}

void AudioSessionOutALSA::updateOutputFormat()
{
    char value[128];

    property_get("mpq.audio.spdif.format",value,"0");
    if (!strncmp(value,"lpcm",sizeof(value)) ||
           !strncmp(value,"ac3",sizeof(value)) ||
           !strncmp(value,"dts",sizeof(value)))
        strlcpy(mSpdifOutputFormat, value, sizeof(mSpdifOutputFormat));
    else
        strlcpy(mSpdifOutputFormat, "lpcm", sizeof(mSpdifOutputFormat));


    property_get("mpq.audio.hdmi.format",value,"0");
    if (!strncmp(value,"lpcm",sizeof(value)) ||
           !strncmp(value,"ac3",sizeof(value)) ||
           !strncmp(value,"dts",sizeof(value)))
        strlcpy(mHdmiOutputFormat, value, sizeof(mHdmiOutputFormat));
    else
        strlcpy(mHdmiOutputFormat, "lpcm", sizeof(mHdmiOutputFormat));

    ALOGV("mSpdifOutputFormat: %s", mSpdifOutputFormat);
    ALOGV("mHdmiOutputFormat: %s", mHdmiOutputFormat);
    return;
}

void AudioSessionOutALSA::updateRoutingFlags(int devices)
{
    mUseDualTunnel = false;
    mSecDevices = 0;
    mCaptureFromProxy = false;

    if(devices & AudioSystem::DEVICE_OUT_PROXY)
        mCaptureFromProxy = true;

    setSpdifHdmiRoutingFlags(devices);

    if((mSpdifFormat == PCM_FORMAT) ||
       (mSpdifFormat == COMPRESSED_FORCED_PCM_FORMAT) ||
       (mHdmiFormat == PCM_FORMAT) ||
       (mHdmiFormat == COMPRESSED_FORCED_PCM_FORMAT) ||
       ((devices & ~(AudioSystem::DEVICE_OUT_SPDIF |
          AudioSystem::DEVICE_OUT_AUX_DIGITAL)))) {
        mRoutePcmAudio = true;
    }

    if(mUseTunnelDecoder)
        mRoutePcmAudio = false;
    if(mFormat == AUDIO_FORMAT_DTS || mFormat == AUDIO_FORMAT_DTS_LBR)
    {
        if(((devices & AudioSystem::DEVICE_OUT_SPDIF) && (devices & ~AudioSystem::DEVICE_OUT_SPDIF)
             && (mSpdifFormat == COMPRESSED_FORMAT)) || ((devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL)
              && mHdmiFormat == COMPRESSED_FORMAT && (devices & ~AudioSystem::DEVICE_OUT_AUX_DIGITAL))) {
            mUseDualTunnel = true;
            if(mSpdifFormat == COMPRESSED_FORMAT)
                mSecDevices |= AudioSystem::DEVICE_OUT_SPDIF;
            if(mHdmiFormat == COMPRESSED_FORMAT)
                mSecDevices |= AudioSystem::DEVICE_OUT_AUX_DIGITAL;
        }
    }
    return;
}

void AudioSessionOutALSA::setSpdifHdmiRoutingFlags(int devices)
{
    if(!(devices & AudioSystem::DEVICE_OUT_SPDIF))
        mSpdifFormat = INVALID_FORMAT;
    if(!(devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL))
        mHdmiFormat = INVALID_FORMAT;
    mDtsTranscode = false;
    mTranscodeDevices = 0;

    if(!strncmp(mSpdifOutputFormat,"lpcm",sizeof(mSpdifOutputFormat))) {
        if(devices & AudioSystem::DEVICE_OUT_SPDIF)
            mSpdifFormat = PCM_FORMAT;
    }
    if(!strncmp(mHdmiOutputFormat,"lpcm",sizeof(mHdmiOutputFormat))) {
        if(devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL)
            mHdmiFormat = PCM_FORMAT;
    }
    if(!strncmp(mSpdifOutputFormat,"ac3",sizeof(mSpdifOutputFormat))) {
        if(devices & AudioSystem::DEVICE_OUT_SPDIF) {
            if(mSampleRate > 24000 && mUseMS11Decoder)
                mSpdifFormat = COMPRESSED_FORMAT;
            else
                mSpdifFormat = PCM_FORMAT;
        // 44.1, 22.05 and 11.025K are not supported on Spdif for Passthrough
            if (mSampleRate == 44100)
                mSpdifFormat = COMPRESSED_FORCED_PCM_FORMAT;
        }
    }
    if(!strncmp(mHdmiOutputFormat,"ac3",sizeof(mHdmiOutputFormat))) {
        if(devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL)
            if(mSampleRate > 24000 && mUseMS11Decoder)
                mHdmiFormat = COMPRESSED_FORMAT;
            else
                mHdmiFormat = PCM_FORMAT;
    }
    if(!strncmp(mSpdifOutputFormat,"dts",sizeof(mSpdifOutputFormat))) {
        if(devices & AudioSystem::DEVICE_OUT_SPDIF) {
            if(mFormat != AUDIO_FORMAT_PCM_16_BIT && mUseTunnelDecoder == true) {
                mSpdifFormat = COMPRESSED_FORMAT;
                if(mFormat != AUDIO_FORMAT_DTS && mFormat != AUDIO_FORMAT_DTS_LBR) {
                     mTranscodeDevices |= AudioSystem::DEVICE_OUT_SPDIF;
                     mDtsTranscode = true;
                }
            }
            else
                mSpdifFormat = COMPRESSED_FORCED_PCM_FORMAT;
        // 44.1, 22.05 and 11.025K are not supported on Spdif for Passthrough
            if ((mSampleRate == 44100 || mSampleRate == 22050 ||
                mSampleRate == 11025) && !mDtsTranscode)
                mSpdifFormat = COMPRESSED_FORCED_PCM_FORMAT;
        }
    }
    if(!strncmp(mHdmiOutputFormat,"dts",sizeof(mHdmiOutputFormat))) {
        if(devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL)
            if(mFormat != AUDIO_FORMAT_PCM_16_BIT && mUseTunnelDecoder == true) {
                mHdmiFormat = COMPRESSED_FORMAT;
                if(mFormat != AUDIO_FORMAT_DTS && mFormat != AUDIO_FORMAT_DTS_LBR) {
                    mTranscodeDevices |= AudioSystem::DEVICE_OUT_AUX_DIGITAL;
                     mDtsTranscode = true;
                }
            }
            else
                mHdmiFormat = COMPRESSED_FORCED_PCM_FORMAT;
    }
    ALOGV("mSpdifFormat- %d, mHdmiFormat- %d", mSpdifFormat, mHdmiFormat);

    return;
}
uint32_t AudioSessionOutALSA::channelMapToChannels(uint32_t channelMap) {
    uint32_t channels = 0;
    while(channelMap) {
        channels++;
        channelMap = channelMap & (channelMap - 1);
    }
    return channels;
}

void AudioSessionOutALSA::reset() {

    alsa_handle_t *compreRxHandle_dup = mCompreRxHandle;
    alsa_handle_t *pcmRxHandle_dup = mPcmRxHandle;
    alsa_handle_t *secCompreRxHandle_dup = mSecCompreRxHandle;

    if(mPcmRxHandle) {
        closeDevice(mPcmRxHandle);
        mPcmRxHandle = NULL;
    }

    if ((mUseTunnelDecoder) ||
        (mHdmiFormat == COMPRESSED_FORMAT) ||
        (mSpdifFormat == mUseMS11Decoder))
        requestAndWaitForEventThreadExit();

    if(mCompreRxHandle) {
        closeDevice(mCompreRxHandle);
        mCompreRxHandle = NULL;
    }

    if(mTranscodeHandle) {
        closeDevice(mTranscodeHandle);
        free(mTranscodeHandle);
        mTranscodeHandle = NULL;
    }

    if(mSecCompreRxHandle) {
        closeDevice(mSecCompreRxHandle);
        mSecCompreRxHandle = NULL;
    }

    if(mMS11Decoder) {
        delete mMS11Decoder;
        mMS11Decoder = NULL;
    }
    if(mBitstreamSM) {
        delete mBitstreamSM;
        mBitstreamSM = NULL;
    }

    for(ALSAHandleList::iterator it = mParent->mDeviceList.begin();
        it != mParent->mDeviceList.end(); ++it) {
        alsa_handle_t *it_dup = &(*it);
        if(compreRxHandle_dup == it_dup || pcmRxHandle_dup == it_dup ||
            secCompreRxHandle_dup == it_dup) {
            mParent->mDeviceList.erase(it);
        }
    }

    mSessionId = -1;

    mRoutePcmAudio    = false;
    mSpdifFormat     = INVALID_FORMAT;
    mHdmiFormat      = INVALID_FORMAT;
    mUseTunnelDecoder = false;
    mCaptureFromProxy = false;
    mUseMS11Decoder   = false;
    mAacConfigDataSet = false;
    mWMAConfigDataSet = false;
    mUseDualTunnel    = false;
    mSecDevices = 0;
    if(mFirstAACBuffer){
        free(mFirstAACBuffer);
        mFirstAACBuffer = NULL;
    }


}

status_t AudioSessionOutALSA::openPcmDevice(int devices)
{
    char *use_case;
    status_t status = NO_ERROR;

    if(mSpdifFormat == COMPRESSED_FORMAT) {
        devices = devices & ~AudioSystem::DEVICE_OUT_SPDIF;
    }
    if(mHdmiFormat == COMPRESSED_FORMAT) {
        devices = devices & ~AudioSystem::DEVICE_OUT_AUX_DIGITAL;
    }
    if(mCaptureFromProxy) {
        devices = AudioSystem::DEVICE_OUT_PROXY;
    }
    status = setPlaybackFormat();
    if(status != NO_ERROR) {
        ALOGE("setPlaybackFormat Failed");
        return BAD_VALUE;
    }
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) ||
        (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
            strlen(SND_USE_CASE_VERB_INACTIVE)))) {
        status = openDevice(SND_USE_CASE_VERB_HIFI2, true,
                     devices);
    } else {
        status = openDevice(SND_USE_CASE_MOD_PLAY_MUSIC2, false,
                     devices);
    }
    if(use_case) {
        free(use_case);
        use_case = NULL;
    }
    if(status != NO_ERROR) {
        return status;
    }
    ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
    mPcmRxHandle = &(*it);
    mBufferSize = mPcmRxHandle->periodSize;

    if(mUseMS11Decoder && (mPcmRxHandle->channels > 2))
        setChannelMap(mPcmRxHandle);
    else if(mPcmRxHandle->channels > 2)
        setPCMChannelMap(mPcmRxHandle);

    return status;
}

void AudioSessionOutALSA::setChannelMap(alsa_handle_t *handle)
{
    char channelMap[8];
    status_t status = NO_ERROR;

    memset(channelMap, 0, sizeof(channelMap));
    switch (handle->channels) {
    case 3:
    case 4:
    case 5:
        ALOGE("TODO: Investigate and add appropriate channel map appropriately");
        break;
    case 6:
        channelMap[0] = PCM_CHANNEL_FL;
        channelMap[1] = PCM_CHANNEL_FR;
        channelMap[2] = PCM_CHANNEL_FC;
        channelMap[3] = PCM_CHANNEL_LFE;
        channelMap[4] = PCM_CHANNEL_LB;
        channelMap[5] = PCM_CHANNEL_RB;
        break;
    case 7:
    case 8:
        ALOGE("TODO: Investigate and add appropriate channel map appropriately");
        break;
    default:
        ALOGE("un supported channels for setting channel map");
        return;
    }

    status = mALSADevice->setChannelMap(handle, sizeof(channelMap), channelMap);
    if(status)
        ALOGE("set channel map failed. Default channel map is used instead");

    return;
}

void AudioSessionOutALSA::setPCMChannelMap(alsa_handle_t *handle)
{
    char channelMap[8];
    status_t status = NO_ERROR;

    memset(channelMap, 0, sizeof(channelMap));
    switch (handle->channels) {
    case 3:
    case 4:
        channelMap[0] = PCM_CHANNEL_FL;
        channelMap[1] = PCM_CHANNEL_FR;
        channelMap[2] = PCM_CHANNEL_LB;
        channelMap[3] = PCM_CHANNEL_RB;
    case 5:
        ALOGE("TODO: Investigate and add appropriate channel map appropriately");
        break;
    case 6:
        channelMap[0] = PCM_CHANNEL_FL;
        channelMap[1] = PCM_CHANNEL_FR;
        channelMap[2] = PCM_CHANNEL_FC;
        channelMap[3] = PCM_CHANNEL_LFE;
        channelMap[4] = PCM_CHANNEL_LB;
        channelMap[5] = PCM_CHANNEL_RB;
        break;
    case 7:
        ALOGE("TODO: Investigate and add appropriate channel map appropriately");
        break;
    case 8:
        channelMap[0] = PCM_CHANNEL_FL;
        channelMap[1] = PCM_CHANNEL_FR;
        channelMap[2] = PCM_CHANNEL_FC;
        channelMap[3] = PCM_CHANNEL_LFE;
        channelMap[4] = PCM_CHANNEL_LB;
        channelMap[5] = PCM_CHANNEL_RB;
        channelMap[6] = PCM_CHANNEL_FLC;
        channelMap[7] = PCM_CHANNEL_FRC;
        break;
    default:
        ALOGE("un supported channels for setting channel map");
        return;
    }

    status = mALSADevice->setChannelMap(handle, sizeof(channelMap), channelMap);
    if(status)
        ALOGE("set channel map failed. Default channel map is used instead");

    return;
}

status_t AudioSessionOutALSA::setPlaybackFormat()
{
    status_t status = NO_ERROR;

    if(mSpdifFormat == PCM_FORMAT ||
              (mSpdifFormat == COMPRESSED_FORCED_PCM_FORMAT)) {
        status = mALSADevice->setPlaybackFormat("LPCM",
                                  AudioSystem::DEVICE_OUT_SPDIF , mDtsTranscode);
    } else if(mSpdifFormat == COMPRESSED_FORMAT) {
        status = mALSADevice->setPlaybackFormat("Compr",
                                  AudioSystem::DEVICE_OUT_SPDIF, mDtsTranscode);
    }
    if(mHdmiFormat == PCM_FORMAT ||
               (mHdmiFormat == COMPRESSED_FORCED_PCM_FORMAT)) {
        status = mALSADevice->setPlaybackFormat("LPCM",
                                  AudioSystem::DEVICE_OUT_AUX_DIGITAL, mDtsTranscode);
    } else if (mHdmiFormat == COMPRESSED_FORMAT) {
        status = mALSADevice->setPlaybackFormat("Compr",
                                  AudioSystem::DEVICE_OUT_AUX_DIGITAL, mDtsTranscode);
    }
    return status;
}

void AudioSessionOutALSA::updateMetaData(size_t bytes) {
    mOutputMetadataTunnel.metadataLength = sizeof(mOutputMetadataTunnel);
    mOutputMetadataTunnel.timestamp = 0;
    mOutputMetadataTunnel.bufferLength =  bytes;
    ALOGD("bytes = %d , mCompreRxHandle->handle->period_size = %d ",
            bytes, mCompreRxHandle->handle->period_size);
}

}       // namespace android_audio_legacy
