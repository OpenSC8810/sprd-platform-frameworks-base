/*
 * Copyright (C) 2011 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "SPRDAVCDecoder"
#include <utils/Log.h>

#include "SPRDAVCDecoder.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/IOMX.h>
#include <HardwareAPI.h>
#include <ui/GraphicBufferMapper.h>

#include "gralloc_priv.h"
#include "avc_dec_api.h"
#include <dlfcn.h>
#include "ion_sprd.h"

namespace android {

#define H264_DECODER_INTERNAL_BUFFER_SIZE (2000*1024) 
#define H264_DECODER_STREAM_BUFFER_SIZE 1024*1024

static const CodecProfileLevel kProfileLevels[] = {
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel1  },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel1b },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel11 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel12 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel13 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel2  },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel21 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel22 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel3  },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel31 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel32 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel4  },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel41 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel42 },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel5  },
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel51 },

    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel1  },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel1b },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel11 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel12 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel13 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel2  },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel21 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel22 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel3  },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel31 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel32 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel4  },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel41 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel42 },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel5  },
    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel51 },

    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel1  },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel1b },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel11 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel12 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel13 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel2  },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel21 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel22 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel3  },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel31 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel32 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel4  },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel41 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel42 },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel5  },
    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel51 },
};

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SPRDAVCDecoder::SPRDAVCDecoder(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SprdSimpleOMXComponent(name, callbacks, appData, component),
      mHandle(new tagAVCHandle),
      mInputBufferCount(0),
      mWidth(320),
      mHeight(240),
      mPictureSize(mWidth * mHeight * 3 / 2),
      mCropLeft(0),
      mCropTop(0),
      mCropWidth(mWidth),
      mCropHeight(mHeight),
      mPicId(0),
      mHeadersDecoded(false),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mOutputPortSettingsChange(NONE),
      mSignalledError(false),
      mCodecExtraBufferMalloced(false),
      ipMemBufferWasSet(false),
      mLibHandle(NULL),
      mDecoderSwFlag(false),
      mChangeToSwDec(false),
      mNeedIVOP(true),
      mH264DecInit(NULL),
      mH264DecGetInfo(NULL),
      mH264DecMemInit(NULL),
      mH264DecDecode(NULL),
      mH264_DecReleaseDispBfr(NULL),
      mH264DecRelease(NULL),
      mH264Dec_SetCurRecPic(NULL),
      mH264Dec_GetLastDspFrm(NULL),
      mH264Dec_ReleaseRefBuffers(NULL){

    bool ret = false;
    ret = openDecoder("libomx_avcdec_hw_sprd.so");
    if(ret == false) {
        ret = openDecoder("libomx_avcdec_sw_sprd.so");
        mDecoderSwFlag = true;
    }

    CHECK_EQ(ret, true);

    if(mDecoderSwFlag) {
        CHECK_EQ(initDecoder(), (status_t)OK);
    } else {
        if(initDecoder() != OK) {
            ret = openDecoder("libomx_avcdec_sw_sprd.so");
            mDecoderSwFlag = true;
            CHECK_EQ(ret, true);
            CHECK_EQ(initDecoder(), (status_t)OK);
        }
    }

    initPorts();

    iUseAndroidNativeBuffer[OMX_DirInput] = OMX_FALSE;
    iUseAndroidNativeBuffer[OMX_DirOutput] = OMX_FALSE;
}

SPRDAVCDecoder::~SPRDAVCDecoder() {

    (*mH264DecRelease)(mHandle);

    delete mHandle;
    mHandle = NULL;
    free(mCodecInterBuffer);
    mCodecInterBuffer = NULL;

    free(mStreamBuffer);
    mStreamBuffer = NULL;

    if (mCodecExtraBufferMalloced)
    {
        free(mCodecExtraBuffer);
        mCodecExtraBuffer = NULL;

        mCodecExtraBufferMalloced = false;
    }

    if (ipMemBufferWasSet)
    {
        iDecExtPmemHeap.clear();
        if(iDecExtVAddr)
        {
            iDecExtVAddr = NULL;
        }
                
        iCMDbufferPmemHeap.clear();
        if(iCMDbufferVAddr)
        {
            iCMDbufferVAddr = NULL;
        }
        ipMemBufferWasSet = false;
    }

    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    CHECK(outQueue.empty());
    CHECK(inQueue.empty());

    if(mLibHandle)
    {
        dlclose(mLibHandle);
        mLibHandle = NULL;
    }

}

void SPRDAVCDecoder::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = kInputPortIndex;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = 1;
    def.nBufferCountActual = kNumInputBuffers;
    def.nBufferSize = 256*1024;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_AVC);
    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mWidth;
    def.format.video.nFrameHeight = mHeight;
    def.format.video.nStride = def.format.video.nFrameWidth;
    def.format.video.nSliceHeight = def.format.video.nFrameHeight;
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
    def.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    def.format.video.pNativeWindow = NULL;

    addPort(def);

    def.nPortIndex = kOutputPortIndex;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = 2;
    def.nBufferCountActual = kNumOutputBuffers;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_RAW);
    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mWidth;
    def.format.video.nFrameHeight = mHeight;
    def.format.video.nStride = def.format.video.nFrameWidth;
    def.format.video.nSliceHeight = def.format.video.nFrameHeight;
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    def.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    def.format.video.pNativeWindow = NULL;

    def.nBufferSize =
        (def.format.video.nFrameWidth * def.format.video.nFrameHeight * 3) / 2;

    addPort(def);
}

status_t SPRDAVCDecoder::initDecoder() {

    memset(mHandle, 0, sizeof(tagAVCHandle));

    mHandle->userdata = (void *)this;
    mHandle->VSP_flushCacheCb = flushCacheWrapper;
    mHandle->VSP_bindCb = BindFrameWrapper;
    mHandle->VSP_unbindCb = UnbindFrameWrapper;
    mHandle->VSP_spsCb = ActivateSPSWrapper;

    mStreamBufferSize = H264_DECODER_STREAM_BUFFER_SIZE;
    mStreamBuffer = (uint8 *)malloc(mStreamBufferSize);

    mCodecInterBufferSize = H264_DECODER_INTERNAL_BUFFER_SIZE;
    mCodecInterBuffer = (uint8 *)malloc(mCodecInterBufferSize);

    MMCodecBuffer codec_buf;
    MMDecVideoFormat video_format;
    
    codec_buf.int_buffer_ptr = (uint8 *)( mCodecInterBuffer);
    codec_buf.int_size = mCodecInterBufferSize;

    video_format.video_std = H264;
    video_format.frame_width = 0;
    video_format.frame_height = 0;	
    video_format.p_extra = NULL;
    video_format.i_extra = 0;

    MMDecRet ret = (*mH264DecInit)(mHandle, &codec_buf,&video_format);

    if (ret == MMDEC_OK)
    {
        return OK;        
    }

    return UNKNOWN_ERROR;
}

void SPRDAVCDecoder::releaseDecoder()
{
    (*mH264DecRelease)(mHandle);

    free(mCodecInterBuffer);
    mCodecInterBuffer = NULL;

    free(mStreamBuffer);
    mStreamBuffer = NULL;

    if (mCodecExtraBufferMalloced)
    {
        free(mCodecExtraBuffer);
        mCodecExtraBuffer = NULL;

        mCodecExtraBufferMalloced = false;
    }

    if (ipMemBufferWasSet)
    {
        iDecExtPmemHeap.clear();
        if(iDecExtVAddr)
        {
            iDecExtVAddr = NULL;
        }

        iCMDbufferPmemHeap.clear();
        if(iCMDbufferVAddr)
        {
            iCMDbufferVAddr = NULL;
        }
        ipMemBufferWasSet = false;
    }

    if(mLibHandle)
    {
        dlclose(mLibHandle);
        mLibHandle = NULL;
    }
}

OMX_ERRORTYPE SPRDAVCDecoder::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex != 0) {
                return OMX_ErrorNoMore;
            }

            if (formatParams->nPortIndex == kInputPortIndex) {
                formatParams->eCompressionFormat = OMX_VIDEO_CodingAVC;
                formatParams->eColorFormat = OMX_COLOR_FormatUnused;
                formatParams->xFramerate = 0;
            } else {
                CHECK(formatParams->nPortIndex == kOutputPortIndex);

                PortInfo *pOutPort = editPortInfo(OMX_DirOutput);
                ALOGI("internalGetParameter, OMX_IndexParamVideoPortFormat, eColorFormat: 0x%x",pOutPort->mDef.format.video.eColorFormat);
                formatParams->eCompressionFormat = OMX_VIDEO_CodingUnused;
                formatParams->eColorFormat = pOutPort->mDef.format.video.eColorFormat;//OMX_COLOR_FormatYUV420Planar;
                formatParams->xFramerate = 0;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoProfileLevelQuerySupported:
        {
            OMX_VIDEO_PARAM_PROFILELEVELTYPE *profileLevel =
                    (OMX_VIDEO_PARAM_PROFILELEVELTYPE *) params;

            if (profileLevel->nPortIndex != kInputPortIndex) {
                ALOGE("Invalid port index: %ld", profileLevel->nPortIndex);
                return OMX_ErrorUnsupportedIndex;
            }

            size_t index = profileLevel->nProfileIndex;
            size_t nProfileLevels =
                    sizeof(kProfileLevels) / sizeof(kProfileLevels[0]);
            if (index >= nProfileLevels) {
                return OMX_ErrorNoMore;
            }

            profileLevel->eProfile = kProfileLevels[index].mProfile;
            profileLevel->eLevel = kProfileLevels[index].mLevel;
            return OMX_ErrorNone;
        }

        case OMX_IndexParamEnableAndroidBuffers:
        {
            EnableAndroidNativeBuffersParams *peanbp = (EnableAndroidNativeBuffersParams *)params;			
            peanbp->enable = iUseAndroidNativeBuffer[OMX_DirOutput];
            ALOGI("internalGetParameter, OMX_IndexParamEnableAndroidBuffers %d",peanbp->enable);
            return OMX_ErrorNone;
        }

        case OMX_IndexParamGetAndroidNativeBuffer:
        {
            GetAndroidNativeBufferUsageParams *pganbp;

            pganbp = (GetAndroidNativeBufferUsageParams *)params;
            if(mDecoderSwFlag) {
                pganbp->nUsage = GRALLOC_USAGE_SW_READ_OFTEN |GRALLOC_USAGE_SW_WRITE_OFTEN;
            } else {
                pganbp->nUsage = GRALLOC_USAGE_PRIVATE_0 | GRALLOC_USAGE_SW_READ_OFTEN |GRALLOC_USAGE_SW_WRITE_OFTEN;
            }
            ALOGI("internalGetParameter, OMX_IndexParamGetAndroidNativeBuffer %x",pganbp->nUsage);
            return OMX_ErrorNone;
        }

        default:
            return SprdSimpleOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SPRDAVCDecoder::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;

            if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.avc",
                        OMX_MAX_STRINGNAME_SIZE - 1)) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex != 0) {
                return OMX_ErrorNoMore;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamEnableAndroidBuffers:
        {
            EnableAndroidNativeBuffersParams *peanbp = (EnableAndroidNativeBuffersParams *)params;
            PortInfo *pOutPort = editPortInfo(1);
            if (peanbp->enable == OMX_FALSE) {
                ALOGI("internalSetParameter, disable AndroidNativeBuffer");
                iUseAndroidNativeBuffer[OMX_DirOutput] = OMX_FALSE;

                pOutPort->mDef.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
            } else {
                ALOGI("internalSetParameter, enable AndroidNativeBuffer");
                iUseAndroidNativeBuffer[OMX_DirOutput] = OMX_TRUE;

                pOutPort->mDef.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
            }
            return OMX_ErrorNone;
        }

        case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *defParams =
                (OMX_PARAM_PORTDEFINITIONTYPE *)params;

            if (defParams->nPortIndex > 1
                    || defParams->nSize
                            != sizeof(OMX_PARAM_PORTDEFINITIONTYPE)) {
                return OMX_ErrorUndefined;
            }

            PortInfo *port = editPortInfo(defParams->nPortIndex);

            if (defParams->nBufferSize != port->mDef.nBufferSize) {
                CHECK_GE(defParams->nBufferSize, port->mDef.nBufferSize);
                port->mDef.nBufferSize = defParams->nBufferSize;
            }

            if (defParams->nBufferCountActual
                    != port->mDef.nBufferCountActual) {
                CHECK_GE(defParams->nBufferCountActual,
                         port->mDef.nBufferCountMin);

                port->mDef.nBufferCountActual = defParams->nBufferCountActual;
            }

            memcpy(&port->mDef.format.video, &defParams->format.video, sizeof(OMX_VIDEO_PORTDEFINITIONTYPE));

            if(defParams->nPortIndex == kOutputPortIndex) {
                port->mDef.format.video.nStride = port->mDef.format.video.nFrameWidth;
                port->mDef.format.video.nSliceHeight = port->mDef.format.video.nFrameHeight;
                mWidth = port->mDef.format.video.nFrameWidth;
                mHeight = port->mDef.format.video.nFrameHeight;
                mCropWidth = mWidth;
                mCropHeight = mHeight;
                port->mDef.nBufferSize =(((mWidth + 15) & -16)* ((mHeight + 15) & -16) * 3) / 2;
                mPictureSize = port->mDef.nBufferSize;
            }

            return OMX_ErrorNone;
        }

        default:
            return SprdSimpleOMXComponent::internalSetParameter(index, params);
    }
}

OMX_ERRORTYPE SPRDAVCDecoder::allocateBuffer(
        OMX_BUFFERHEADERTYPE **header,
        OMX_U32 portIndex,
        OMX_PTR appPrivate,
        OMX_U32 size) {
    switch(portIndex)
    {
        case OMX_DirInput:
            return SprdSimpleOMXComponent::allocateBuffer(header, portIndex, appPrivate, size);

        case OMX_DirOutput:
        {
            if(mDecoderSwFlag) {
                return SprdSimpleOMXComponent::allocateBuffer(header, portIndex, appPrivate, size);
            } else {
                MemoryHeapIon* pMem = NULL;
                int phyAddr = 0;
                int bufferSize = 0;
                unsigned char* pBuffer = NULL;
                OMX_U32 size64word = (size + 1024*4 - 1) & ~(1024*4 - 1);

                pMem = new MemoryHeapIon(SPRD_ION_DEV, size64word, MemoryHeapBase::NO_CACHING, ION_HEAP_CARVEOUT_MASK);

                if(pMem->getHeapID() < 0) {
                    ALOGE("Failed to alloc outport pmem buffer");
                    return OMX_ErrorInsufficientResources;
                }
                if(pMem->get_phy_addr_from_ion(&phyAddr, &bufferSize)) {
                    ALOGE("get_phy_addr_from_ion fail");
                    return OMX_ErrorInsufficientResources;
                }

                pBuffer = (unsigned char*)(pMem->base());
                BufferPrivateStruct* bufferPrivate = new BufferPrivateStruct();
                bufferPrivate->pMem = pMem;
                bufferPrivate->phyAddr = phyAddr;
                ALOGI("allocateBuffer, allocate buffer from pmem, pBuffer: 0x%x, phyAddr: 0x%x, size: %d", pBuffer, phyAddr, bufferSize);

                SprdSimpleOMXComponent::useBuffer(header, portIndex, appPrivate, bufferSize, pBuffer, bufferPrivate);
                delete bufferPrivate;

                return OMX_ErrorNone;
            }
        }

        default:
            return OMX_ErrorUnsupportedIndex;

    }
}

OMX_ERRORTYPE SPRDAVCDecoder::freeBuffer(
        OMX_U32 portIndex,
        OMX_BUFFERHEADERTYPE *header) {
    switch(portIndex)
    {
        case OMX_DirInput:
            return SprdSimpleOMXComponent::freeBuffer(portIndex, header);

        case OMX_DirOutput:
        {
            BufferCtrlStruct* pBufCtrl= (BufferCtrlStruct*)(header->pOutputPortPrivate);
            if(pBufCtrl != NULL) {
                if(pBufCtrl->pMem != NULL) {
                    ALOGI("freeBuffer, phyAddr: 0x%x", pBufCtrl->phyAddr);
                    pBufCtrl->pMem.clear();
                }
                return SprdSimpleOMXComponent::freeBuffer(portIndex, header);
            } else {
                ALOGE("freeBuffer, pBufCtrl==NULL");
                return OMX_ErrorUndefined;
            }
        }

        default:
            return OMX_ErrorUnsupportedIndex;
    }
}

OMX_ERRORTYPE SPRDAVCDecoder::getConfig(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexConfigCommonOutputCrop:
        {
            OMX_CONFIG_RECTTYPE *rectParams = (OMX_CONFIG_RECTTYPE *)params;

            if (rectParams->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            rectParams->nLeft = mCropLeft;
            rectParams->nTop = mCropTop;
            rectParams->nWidth = mCropWidth;
            rectParams->nHeight = mCropHeight;

            return OMX_ErrorNone;
        }

        default:
            return OMX_ErrorUnsupportedIndex;
    }
}

void dump_bs( uint8* pBuffer,int32 aInBufSize)
{
	FILE *fp = fopen("/data/video_es.m4v","ab");
	fwrite(pBuffer,1,aInBufSize,fp);
	fclose(fp);
}

void dump_yuv( uint8* pBuffer,int32 aInBufSize)
{
	FILE *fp = fopen("/data/video.yuv","ab");
	fwrite(pBuffer,1,aInBufSize,fp);
	fclose(fp);
}

#define mmin(aa,bb)		(((aa) < (bb)) ? (aa) : (bb))

void SPRDAVCDecoder::onQueueFilled(OMX_U32 portIndex) {
    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    if (mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    if(mChangeToSwDec){

        mChangeToSwDec = false;

        ALOGI("%s, %d, change to sw decoder", __FUNCTION__, __LINE__);

        releaseDecoder();

        if(!openDecoder("libomx_avcdec_sw_sprd.so")){
            ALOGE("onQueueFilled, open  libomx_avcdec_sw_sprd.so failed.");
            notify(OMX_EventError, OMX_ErrorDynamicResourcesUnavailable, 0, NULL);
            mSignalledError = true;
            mDecoderSwFlag = false;
            return;
        }

        if(initDecoder() != OK) {
            ALOGE("onQueueFilled, init sw decoder failed.");
            notify(OMX_EventError, OMX_ErrorDynamicResourcesUnavailable, 0, NULL);
            mSignalledError = true;
            return;
        }
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    bool portSettingsChanged = false;
    while ((mEOSStatus != INPUT_DATA_AVAILABLE || !inQueue.empty())
            && outQueue.size() != 0) {

        if (mEOSStatus == INPUT_EOS_SEEN) {
            drainAllOutputBuffers();
            return;
        }

        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

        List<BufferInfo *>::iterator itBuffer = outQueue.begin();
        OMX_BUFFERHEADERTYPE *outHeader = NULL;
        BufferCtrlStruct *pBufCtrl = NULL;
        uint32 count = 0;
        do
        {
            if(count >= outQueue.size()){
                ALOGI("onQueueFilled, get outQueue buffer, return, count=%d, queue_size=%d",count, outQueue.size());
                return;
            }
            
            outHeader = (*itBuffer)->mHeader;
            pBufCtrl= (BufferCtrlStruct*)(outHeader->pOutputPortPrivate);
            if(pBufCtrl == NULL){
                ALOGE("onQueueFilled, pBufCtrl == NULL, fail");
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }

            itBuffer++;
            count++;
        }
        while(pBufCtrl->iRefCount > 0);

//        ALOGI("%s, %d, mBuffer=0x%x, outHeader=0x%x, iRefCount=%d", __FUNCTION__, __LINE__, *itBuffer, outHeader, pBufCtrl->iRefCount);
        ALOGI("%s, %d, outHeader:0x%x, inHeader: 0x%x, len: %d, nOffset: %d, time: %lld, EOS: %d",
            __FUNCTION__, __LINE__,outHeader,inHeader, inHeader->nFilledLen,inHeader->nOffset, inHeader->nTimeStamp,inHeader->nFlags & OMX_BUFFERFLAG_EOS);
        
        ++mPicId;
        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
            mEOSStatus = INPUT_EOS_SEEN;
            continue;
        }

        if(inHeader->nFilledLen == 0) {
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;
            continue;
        }

        MMDecInput dec_in;
        MMDecOutput dec_out;

        //int32 iSkipToIDR = 1;
        uint32 dataLen = mmin( inHeader->nFilledLen, mStreamBufferSize);

        memcpy(mStreamBuffer, inHeader->pBuffer + inHeader->nOffset, dataLen);
        dec_in.pStream = mStreamBuffer;
        dec_in.dataLen = dataLen;
        dec_in.beLastFrm = 0;
        dec_in.expected_IVOP = mNeedIVOP;
        dec_in.beDisplayed = 1;
        dec_in.err_pkt_num = 0;

        dec_out.frameEffective = 0;

        ALOGV("%s, %d, dec_in.dataLen: %d, mPicId: %d", __FUNCTION__, __LINE__, dec_in.dataLen, mPicId);

        outHeader->nTimeStamp = inHeader->nTimeStamp;
        outHeader->nFlags = inHeader->nFlags;

        unsigned int picPhyAddr = 0;
    if(!mDecoderSwFlag) {
        pBufCtrl= (BufferCtrlStruct*)(outHeader->pOutputPortPrivate);
        if(pBufCtrl->phyAddr != 0) {
            picPhyAddr = pBufCtrl->phyAddr;
        } else {
            native_handle_t *pNativeHandle = (native_handle_t *)outHeader->pBuffer;
            struct private_handle_t *private_h = (struct private_handle_t *)pNativeHandle;
            picPhyAddr = (unsigned int)(private_h->phyaddr);
        }
    }
    ALOGV("%s, %d, outHeader: 0x%x, pBuffer: 0x%x, phyAddr: 0x%x",__FUNCTION__, __LINE__, outHeader, outHeader->pBuffer, picPhyAddr);
//    ALOGI("%s, %d, header: %0x, mPictureSize: %d", __FUNCTION__, __LINE__, header_tmp, mPictureSize);
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
    if(iUseAndroidNativeBuffer[OMX_DirOutput]){
         OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(OMX_DirOutput)->mDef;
    	int width = def->format.video.nStride;
    	int height = def->format.video.nSliceHeight;
    	Rect bounds(width, height);
    	void *vaddr;
         int usage;

        usage = GRALLOC_USAGE_SW_READ_OFTEN|GRALLOC_USAGE_SW_WRITE_OFTEN;

    	if(mapper.lock((const native_handle_t*)outHeader->pBuffer, usage, bounds, &vaddr)){
            ALOGE("onQueueFilled, mapper.lock fail %x",outHeader->pBuffer);
            return ;
    	}
        ALOGV("%s, %d, outHeader: 0x%x, pBuffer: 0x%x, vaddr: 0x%x, phyAddr: 0x%x",
            __FUNCTION__, __LINE__, outHeader, outHeader->pBuffer,vaddr, picPhyAddr);
        uint8 *yuv = (uint8 *)(vaddr + outHeader->nOffset);
        ALOGV("%s, %d, yuv: %0x, mPicId: %d, outHeader->pBuffer: %0x, outHeader->nOffset: %d, outHeader->nFlags: %d, outHeader->nTimeStamp: %lld", 
            __FUNCTION__, __LINE__, yuv, mPicId,outHeader->pBuffer, outHeader->nOffset, outHeader->nFlags, outHeader->nTimeStamp);
        (*mH264Dec_SetCurRecPic)(mHandle, yuv, (uint8 *)picPhyAddr, (void *)outHeader, mPicId);
    }else
    {
        (*mH264Dec_SetCurRecPic)(mHandle, outHeader->pBuffer, (uint8 *)picPhyAddr, (void *)outHeader, mPicId);
    }

//        dump_bs( dec_in.pStream, dec_in.dataLen);
        
        MMDecRet decRet = (*mH264DecDecode)(mHandle, &dec_in,&dec_out);

        ALOGI("%s, %d, decRet: %d, dataLen: %d, dec_out.frameEffective: %d, mDecoderSwFlag: %d",
            __FUNCTION__, __LINE__, decRet, dec_in.dataLen, dec_out.frameEffective, mDecoderSwFlag);

        if( decRet == MMDEC_OK){
            mNeedIVOP = false;
        }

        if(iUseAndroidNativeBuffer[OMX_DirOutput]){
            if(mapper.unlock((const native_handle_t*)outHeader->pBuffer)){
                ALOGE("onQueueFilled, mapper.unlock fail %x",outHeader->pBuffer);
            }
	}

        H264SwDecInfo decoderInfo;
        MMDecRet ret;
       ret = (*mH264DecGetInfo)(mHandle, &decoderInfo);
//       ALOGI("%s, %d, decRet: %d", __FUNCTION__, __LINE__, decRet);
        if(ret == MMDEC_OK) {
            if (handlePortSettingChangeEvent(&decoderInfo)) {
                return;
            } else if(mChangeToSwDec == true) {
                return;
            }
        } else {
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;

            continue;
        }

        inHeader->nOffset += dataLen - dec_in.dataLen;
        inHeader->nFilledLen -= dataLen - dec_in.dataLen;

        if (inHeader->nFilledLen == 0 || dataLen <= dec_in.dataLen){
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;
        }

        while (!outQueue.empty() &&
                mHeadersDecoded &&
                    dec_out.frameEffective) {
//dump_yuv( dec_out.pOutFrameY, dec_out.frame_height*dec_out.frame_width*3/2);

            ALOGV("%s, %d, dec_out.pOutFrameY: %0x, dec_out.mPicId: %d", __FUNCTION__, __LINE__, dec_out.pOutFrameY, dec_out.mPicId);
            int32_t picId = dec_out.mPicId;//decodedPicture.picId;
            drainOneOutputBuffer(picId, dec_out.pBufferHeader);
            dec_out.frameEffective = false;
        }
    }
}

bool SPRDAVCDecoder::handlePortSettingChangeEvent(const H264SwDecInfo *info) {
//    ALOGI("%s, %d, mWidth: %d, mHeight: %d,  info->picWidth: %d,info->picHeight:%d, mPictureSize:%d ",
//                __FUNCTION__, __LINE__,mWidth, mHeight,  info->picWidth, info->picHeight, mPictureSize);

    if(!mDecoderSwFlag){
        ALOGI("%s, %d, picWidth: %d, picHeight: %d, numRef: %d, profile: 0x%x",
            __FUNCTION__, __LINE__,info->picWidth, info->picHeight, info->numRefFrames, info->profile);
        if ((!((info->picWidth <= 720 && info->picHeight <= 576) || (info->picWidth <= 576 && info->picHeight <= 720))) || (info->profile == 0x64) || (info->profile == 0x4d)){
            mDecoderSwFlag = true;
            mChangeToSwDec = true;
        }
    }

    OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(kOutputPortIndex)->mDef;
    if (mWidth != info->picWidth || mHeight != info->picHeight || info->numRefFrames > def->nBufferCountActual-(2+1+info->has_b_frames)) {
        mWidth  = info->picWidth;
        mHeight = info->picHeight;
        mPictureSize = mWidth * mHeight * 3 / 2;
        ALOGI("%s, %d, mWidth: %d, mHeight: %d, numRefFrames: %d, profile: 0x%x",
            __FUNCTION__, __LINE__,mWidth, mHeight, info->numRefFrames, info->profile);
        mCropWidth = mWidth;
        mCropHeight = mHeight;
        def->nBufferCountActual = info->numRefFrames + (2+1+info->has_b_frames);
        ALOGI("%s, %d, info->numRefFrames: %d, info->has_b_frames: %d, def->nBufferCountActual: %d", __FUNCTION__, __LINE__, info->numRefFrames, info->has_b_frames, def->nBufferCountActual);

        updatePortDefinitions();
        notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
        mOutputPortSettingsChange = AWAITING_DISABLED;
        return true;
    }

    return false;
}

bool SPRDAVCDecoder::handleCropRectEvent(const CropParams *crop) {
    if (mCropLeft != crop->cropLeftOffset ||
        mCropTop != crop->cropTopOffset ||
        mCropWidth != crop->cropOutWidth ||
        mCropHeight != crop->cropOutHeight) {
        mCropLeft = crop->cropLeftOffset;
        mCropTop = crop->cropTopOffset;
        mCropWidth = crop->cropOutWidth;
        mCropHeight = crop->cropOutHeight;

        notify(OMX_EventPortSettingsChanged, 1,
                OMX_IndexConfigCommonOutputCrop, NULL);

        return true;
    }
    return false;
}

void SPRDAVCDecoder::drainOneOutputBuffer(int32_t picId, void* pBufferHeader) {

    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    List<BufferInfo *>::iterator it = outQueue.begin();
    while ((*it)->mHeader != (OMX_BUFFERHEADERTYPE*)pBufferHeader) {
        ++it;
    }

    BufferInfo *outInfo = *it;
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    outHeader->nFilledLen = mPictureSize;

    ALOGI("%s, %d, outHeader: %0x, outHeader->pBuffer: %0x, outHeader->nOffset: %d, outHeader->nFlags: %d, outHeader->nTimeStamp: %lld", 
        __FUNCTION__, __LINE__, outHeader , outHeader->pBuffer, outHeader->nOffset, outHeader->nFlags, outHeader->nTimeStamp);

//    LOGI("%s, %d, outHeader->nTimeStamp: %d, outHeader->nFlags: %d, mPictureSize: %d", __FUNCTION__, __LINE__, outHeader->nTimeStamp, outHeader->nFlags, mPictureSize);
 //   LOGI("%s, %d, out: %0x", __FUNCTION__, __LINE__, outHeader->pBuffer + outHeader->nOffset);

//    dump_yuv(data, mPictureSize);
    outInfo->mOwnedByUs = false;
    outQueue.erase(it);
    outInfo = NULL;
        
    BufferCtrlStruct* pOutBufCtrl= (BufferCtrlStruct*)(outHeader->pOutputPortPrivate);
    pOutBufCtrl->iRefCount++;
    notifyFillBufferDone(outHeader);
}

bool SPRDAVCDecoder::drainAllOutputBuffers() {
    ALOGI("%s, %d", __FUNCTION__, __LINE__);

    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    int32_t picId;
    uint8 *yuv;

    while (!outQueue.empty()) {
        BufferInfo *outInfo = *outQueue.begin();
        outQueue.erase(outQueue.begin());
        OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;
        if (mHeadersDecoded &&
            MMDEC_OK == (*mH264Dec_GetLastDspFrm)(mHandle, &yuv, &picId) ) {
            outHeader->nFilledLen = mPictureSize;
        } else {
            outHeader->nTimeStamp = 0;
            outHeader->nFilledLen = 0;
            outHeader->nFlags = OMX_BUFFERFLAG_EOS;
            mEOSStatus = OUTPUT_FRAMES_FLUSHED;
        }

        outInfo->mOwnedByUs = false;
        BufferCtrlStruct* pOutBufCtrl= (BufferCtrlStruct*)(outHeader->pOutputPortPrivate);
        pOutBufCtrl->iRefCount++;
        notifyFillBufferDone(outHeader);
    }

    return true;
}

void SPRDAVCDecoder::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == kInputPortIndex) {
        mEOSStatus = INPUT_DATA_AVAILABLE;
        mNeedIVOP = true;
    }
}

void SPRDAVCDecoder::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    switch (mOutputPortSettingsChange) {
        case NONE:
            break;

        case AWAITING_DISABLED:
        {
            CHECK(!enabled);
            mOutputPortSettingsChange = AWAITING_ENABLED;
            break;
        }

        default:
        {
            CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
            CHECK(enabled);
            mOutputPortSettingsChange = NONE;
            break;
        }
    }
}

void SPRDAVCDecoder::onPortFlushPrepare(OMX_U32 portIndex) {
    if(portIndex == OMX_DirOutput){
        (*mH264Dec_ReleaseRefBuffers)(mHandle);
    }
}

void SPRDAVCDecoder::updatePortDefinitions() {
    OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(0)->mDef;
    def->format.video.nFrameWidth = mWidth;
    def->format.video.nFrameHeight = mHeight;
    def->format.video.nStride = def->format.video.nFrameWidth;
    def->format.video.nSliceHeight = def->format.video.nFrameHeight;

    def = &editPortInfo(1)->mDef;
    def->format.video.nFrameWidth = mWidth;
    def->format.video.nFrameHeight = mHeight;
    def->format.video.nStride = def->format.video.nFrameWidth;
    def->format.video.nSliceHeight = def->format.video.nFrameHeight;

    def->nBufferSize =
        (def->format.video.nFrameWidth
            * def->format.video.nFrameHeight * 3) / 2;
}

int32_t SPRDAVCDecoder::flushCacheWrapper(
        void* aUserData,int* vaddr,int* paddr,int size)
{
    return static_cast<SPRDAVCDecoder *>(aUserData)->flushCache(vaddr, paddr, size);
}

// static
int32_t SPRDAVCDecoder::ActivateSPSWrapper(
        void *userData, unsigned int width,unsigned int height, unsigned int numBuffers) {
    return static_cast<SPRDAVCDecoder *>(userData)->activateSPS(width, height, numBuffers);
}

// static
int32_t SPRDAVCDecoder::BindFrameWrapper(void *aUserData, void *pHeader) {
    return static_cast<SPRDAVCDecoder *>(aUserData)->VSP_bind_cb(pHeader);
}

// static
int32_t SPRDAVCDecoder::UnbindFrameWrapper(void *aUserData, void *pHeader) {
    return static_cast<SPRDAVCDecoder *>(aUserData)->VSP_unbind_cb(pHeader);
}

int SPRDAVCDecoder::activateSPS(unsigned int width,unsigned int height, unsigned int numBuffers) {
    ALOGI("%s, %d, mPictureSize: %d, numBuffers: %d", __FUNCTION__, __LINE__, mPictureSize, numBuffers);

    MMCodecBuffer extra_mem[3];
    uint32 extra_mem_size;
    uint32 mb_num_x = (width + 15)>>4;
    uint32 mb_num_y = (height + 15)>>4;
    uint32 mb_num_total = mb_num_x * mb_num_y;
    uint32 frm_size = (mb_num_total * 256);

    if (mCodecExtraBufferMalloced)
    {
        free(mCodecExtraBuffer);
        mCodecExtraBuffer = NULL;

        mCodecExtraBufferMalloced = false;
    }

    if (!mDecoderSwFlag)
    {
        if (ipMemBufferWasSet)
        {
            iDecExtPmemHeap.clear();
            if(iDecExtVAddr)
            {
                iDecExtVAddr = NULL;
            }
                    
            iCMDbufferPmemHeap.clear();
            if(iCMDbufferVAddr)
            {
                iCMDbufferVAddr = NULL;
            }
            ipMemBufferWasSet = false;
        }

        if (!ipMemBufferWasSet)
	{
            extra_mem_size = 100*40*4 /*vld_cabac_table_ptr*/
		+ mb_num_x*32 /*ipred_top_line_buffer*/
		+ 10*1024; //rsv

            iDecExtPmemHeap = new MemoryHeapIon(SPRD_ION_DEV, extra_mem_size, MemoryHeapBase::NO_CACHING, ION_HEAP_CARVEOUT_MASK);
            int fd = iDecExtPmemHeap->getHeapID();
	    if(fd>=0){
	        int ret,phy_addr, buffer_size;
                ret = iDecExtPmemHeap->get_phy_addr_from_ion(&phy_addr, &buffer_size);
                if(ret)
                {
                    ALOGE ("%s, %d: iDecExtPmemHeap get_phy_addr_from_ion fail", __FUNCTION__, __LINE__);
                }

                iDecExtPhyAddr =(OMX_U32)phy_addr;
                ALOGI("ext mem pmempool %x,%x,%x,%x\n",iDecExtPmemHeap->getHeapID(),iDecExtPmemHeap->base(),phy_addr,buffer_size);
                iDecExtVAddr = (void *)iDecExtPmemHeap->base();
                extra_mem[HW_NO_CACHABLE].common_buffer_ptr =(uint8 *)iDecExtVAddr;
                extra_mem[HW_NO_CACHABLE].common_buffer_ptr_phy = (void *)iDecExtPhyAddr;
                extra_mem[HW_NO_CACHABLE].size = extra_mem_size;
    	    }
        
            /////////////////////CMD BUFFER
            extra_mem_size = frm_size*8 *2;/*vsp_cmd_data, vsp_cmd_info*/
            extra_mem_size += 1024*1024; //pingpang buffer for stream
	
            iCMDbufferPmemHeap = new MemoryHeapIon(SPRD_ION_DEV, extra_mem_size, 0, (1<<31)|ION_HEAP_CARVEOUT_MASK);
            fd = iCMDbufferPmemHeap->getHeapID();
            if(fd>=0){
                int ret,phy_addr, buffer_size;
                ret = iCMDbufferPmemHeap->get_phy_addr_from_ion(&phy_addr, &buffer_size);
                if(ret)
                {
                    ALOGE("%s, %d: get_phy_addr_from_ion fail", __FUNCTION__, __LINE__);
                }

                iCMDbufferPhyAddr =(OMX_U32)phy_addr;
                ALOGI("iCMDbufferPmemHeap pmempool %x,%x,%x,%x\n",iCMDbufferPmemHeap->getHeapID(),iCMDbufferPmemHeap->base(),phy_addr,buffer_size);

                iCMDbufferVAddr= (void *)iCMDbufferPmemHeap->base();
                extra_mem[HW_CACHABLE].common_buffer_ptr =(uint8 *) iCMDbufferVAddr;
                extra_mem[HW_CACHABLE].common_buffer_ptr_phy = (void *)iCMDbufferPhyAddr;
                extra_mem[HW_CACHABLE].size = extra_mem_size;
            
                ALOGI ("iCMDbufferPmemHeap allocate successful!");
            }
            ipMemBufferWasSet = true;
        }
    }
    
    extra_mem_size = (2+mb_num_y)*mb_num_x*8 /*MB_INFO*/
        + (mb_num_total*16) /*i4x4pred_mode_ptr*/
        + (mb_num_total*16) /*direct_ptr*/
        + (mb_num_total*24) /*nnz_ptr*/
        + (mb_num_total*2*16*2*2) /*mvd*/
        + (mb_num_total*4) /*slice_nr_ptr*/
        + 3*4*17 /*fs, fs_ref, fs_ltref*/
        + 17*(7*4+(23+150*2*17)*4+mb_num_total*16*(2*2*2 + 1 + 1 + 4 + 4)+((mb_num_x*16+48)*(mb_num_y*16+48)*3/2)) /*dpb_ptr*/
        + mb_num_total /*g_MbToSliceGroupMap*/
        +200*1024; //rsv ,for buffer wasted by addr align

    if (mDecoderSwFlag)
    {
        int32 ext_width, ext_height;
        int32 ext_frm_size;

        ext_width = width + 24*2;
        ext_height = height + 24*2;
        ext_frm_size = ext_width*ext_height*3/2;
        
        extra_mem_size += 24*16*sizeof(int16); /*g_halfPixTemp*/
        extra_mem_size + 17 *ext_frm_size;  /*dpb_ptr->fs[i]->frame->imgYUV*/
        extra_mem_size += 1*1024*1024;
    }
    
    mCodecExtraBuffer = (uint8_t *)malloc(extra_mem_size+4);
    mCodecExtraBufferMalloced = true;

    extra_mem[SW_CACHABLE].common_buffer_ptr = mCodecExtraBuffer;
    extra_mem[SW_CACHABLE].size = extra_mem_size;

    (*mH264DecMemInit)(mHandle, extra_mem);

    mHeadersDecoded = true;
    return 1;
}

int SPRDAVCDecoder::VSP_bind_cb(void *pHeader)
{
    BufferCtrlStruct *pBufCtrl = (BufferCtrlStruct *)(((OMX_BUFFERHEADERTYPE *)pHeader)->pOutputPortPrivate);
    ALOGI("VSP_bind_cb, ref frame: 0x%x, %x; iRefCount=%d",
            ((OMX_BUFFERHEADERTYPE *)pHeader)->pBuffer, pHeader,pBufCtrl->iRefCount);
    pBufCtrl->iRefCount++;
    return 0;
}

int SPRDAVCDecoder::VSP_unbind_cb(void *pHeader)
{
    BufferCtrlStruct *pBufCtrl = (BufferCtrlStruct *)(((OMX_BUFFERHEADERTYPE *)pHeader)->pOutputPortPrivate);

    ALOGI("VSP_unbind_cb, ref frame: 0x%x, %x; iRefCount=%d",
            ((OMX_BUFFERHEADERTYPE *)pHeader)->pBuffer, pHeader,pBufCtrl->iRefCount);

    if (pBufCtrl->iRefCount  > 0)
    {   
        pBufCtrl->iRefCount--;
    }
    
    return 0;
}

int SPRDAVCDecoder::flushCache (int* vaddr,int* paddr,int size)
{
    int ret = iCMDbufferPmemHeap->flush_ion_buffer((void *)vaddr,(void* )paddr,size);

    return ret;
}

OMX_ERRORTYPE SPRDAVCDecoder::getExtensionIndex(
        const char *name, OMX_INDEXTYPE *index) {

    ALOGI("getExtensionIndex, name: %s",name);
    if(strcmp(name, SPRD_INDEX_PARAM_ENABLE_ANB) == 0)
    {
    		ALOGI("getExtensionIndex:%s",SPRD_INDEX_PARAM_ENABLE_ANB);
		*index = (OMX_INDEXTYPE) OMX_IndexParamEnableAndroidBuffers;
		return OMX_ErrorNone;
    }else if (strcmp(name, SPRD_INDEX_PARAM_GET_ANB) == 0)
    {
     		ALOGI("getExtensionIndex:%s",SPRD_INDEX_PARAM_GET_ANB);   
		*index = (OMX_INDEXTYPE) OMX_IndexParamGetAndroidNativeBuffer;
		return OMX_ErrorNone;
    }	else if (strcmp(name, SPRD_INDEX_PARAM_USE_ANB) == 0)
    {
     		ALOGI("getExtensionIndex:%s",SPRD_INDEX_PARAM_USE_ANB);     
		*index = OMX_IndexParamUseAndroidNativeBuffer2;
		return OMX_ErrorNone;
    }

    return OMX_ErrorNotImplemented;
}

bool SPRDAVCDecoder::openDecoder(const char* libName)
{
    if(mLibHandle){
        dlclose(mLibHandle);
    }
    
    ALOGI("openDecoder, lib: %s",libName);

    mLibHandle = dlopen(libName, RTLD_NOW);
    if(mLibHandle == NULL){
        ALOGE("openDecoder, can't open lib: %s",libName);
        return false;
    }

    mH264DecGetNALType = (FT_H264DecGetNALType)dlsym(mLibHandle, "H264DecGetNALType");
    if(mH264DecGetNALType == NULL){
        ALOGE("Can't find H264DecGetNALType in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264GetBufferDimensions = (FT_H264GetBufferDimensions)dlsym(mLibHandle, "H264GetBufferDimensions");
    if(mH264GetBufferDimensions == NULL){
        ALOGE("Can't find H264GetBufferDimensions in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264DecGetInfo = (FT_H264DecGetInfo)dlsym(mLibHandle, "H264DecGetInfo");
    if(mH264DecGetInfo == NULL){
        ALOGE("Can't find H264DecGetInfo in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264DecInit = (FT_H264DecInit)dlsym(mLibHandle, "H264DecInit");
    if(mH264DecInit == NULL){
        ALOGE("Can't find H264DecInit in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264DecMemInit = (FT_H264DecMemInit)dlsym(mLibHandle, "H264DecMemInit");
    if(mH264DecMemInit == NULL){
        ALOGE("Can't find H264DecMemInit in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264DecDecode = (FT_H264DecDecode)dlsym(mLibHandle, "H264DecDecode");
    if(mH264DecDecode == NULL){
        ALOGE("Can't find H264DecDecode in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264_DecReleaseDispBfr = (FT_H264_DecReleaseDispBfr)dlsym(mLibHandle, "H264_DecReleaseDispBfr");
    if(mH264_DecReleaseDispBfr == NULL){
        ALOGE("Can't find H264_DecReleaseDispBfr in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264DecRelease = (FT_H264DecRelease)dlsym(mLibHandle, "H264DecRelease");
    if(mH264DecRelease == NULL){
        ALOGE("Can't find H264DecRelease in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264Dec_SetCurRecPic = (FT_H264Dec_SetCurRecPic)dlsym(mLibHandle, "H264Dec_SetCurRecPic");
    if(mH264Dec_SetCurRecPic == NULL){
        ALOGE("Can't find H264Dec_SetCurRecPic in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264Dec_GetLastDspFrm = (FT_H264Dec_GetLastDspFrm)dlsym(mLibHandle, "H264Dec_GetLastDspFrm");
    if(mH264Dec_GetLastDspFrm == NULL){
        ALOGE("Can't find H264Dec_GetLastDspFrm in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264Dec_ReleaseRefBuffers = (FT_H264Dec_ReleaseRefBuffers)dlsym(mLibHandle, "H264Dec_ReleaseRefBuffers");
    if(mH264Dec_ReleaseRefBuffers == NULL){
        ALOGE("Can't find H264Dec_ReleaseRefBuffers in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    return true;
}

}  // namespace android

android::SprdOMXComponent *createSprdOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SPRDAVCDecoder(name, callbacks, appData, component);
}
