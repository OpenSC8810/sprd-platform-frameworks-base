/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SPRD_AVC_ENCODER_H_
#define SPRD_AVC_ENCODER_H_

#include "SprdSimpleOMXComponent.h"

#include "avc_enc_api.h"

namespace android {


struct SPRDAVCEncoder :  public SprdSimpleOMXComponent {
    SPRDAVCEncoder(
            const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

    // Override SimpleSoftOMXComponent methods
    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);

virtual OMX_ERRORTYPE getExtensionIndex(
            const char *name, OMX_INDEXTYPE *index);
            
protected:
    virtual ~SPRDAVCEncoder();

private:
    enum {
        kNumBuffers = 2,
    };

    // OMX input buffer's timestamp and flags
    typedef struct {
        int64_t mTimeUs;
        int32_t mFlags;
    } InputBufferInfo;


    OMX_BOOL mStoreMetaData;
    unsigned char* mYUVIn;
    sp<MemoryHeapIon> mYUVInPmemHeap;

    unsigned char* mBuf_inter;
    unsigned char* mBuf_extra;
	unsigned char* mBuf_stream;
		
    sp<MemoryHeapIon> mPmem_inter;
	sp<MemoryHeapIon> mPmem_extra;
	sp<MemoryHeapIon> mPmem_stream;

    MMEncVideoInfo mEncInfo;

    int32_t  mVideoWidth;
    int32_t  mVideoHeight;
    int32_t  mVideoFrameRate;
    int32_t  mVideoBitRate;
    int32_t  mVideoColorFormat;
    int32_t  mIDRFrameRefreshIntervalInSec;
    AVCProfile mAVCEncProfile;
    AVCLevel   mAVCEncLevel;

    int64_t  mNumInputFrames;
    int64_t  mPrevTimestampUs;
    bool     mStarted;
    bool     mSpsPpsHeaderReceived;
    bool     mReadyForNextFrame;
    bool     mSawInputEOS;
    bool     mSignalledError;
    bool     mIsIDRFrame;

    tagAVCHandle          *mHandle;
    tagAVCEncParam        *mEncParams;
    MMEncConfig *mEncConfig;
    uint8_t               *mInputFrameData;
    uint32_t              *mSliceGroup;
//    Vector<MediaBuffer *> mOutputBuffers;
    Vector<InputBufferInfo> mInputBufferInfoVec;

    void* mLibHandle;
    FT_H264EncInit        mH264EncInit;
    FT_H264EncSetConf        mH264EncSetConf;
    FT_H264EncGetConf        mH264EncGetConf;
    FT_H264EncStrmEncode        mH264EncStrmEncode;
    FT_H264EncGenHeader        mH264EncGenHeader;
    FT_H264EncRelease        mH264EncRelease;

    void initPorts();
    OMX_ERRORTYPE initEncParams();
    OMX_ERRORTYPE initEncoder();
    OMX_ERRORTYPE releaseEncoder();
//    void releaseOutputBuffers();
    bool openEncoder(const char* libName);

    DISALLOW_EVIL_CONSTRUCTORS(SPRDAVCEncoder);
};

}  // namespace android

#endif  // SPRD_AVC_ENCODER_H_
