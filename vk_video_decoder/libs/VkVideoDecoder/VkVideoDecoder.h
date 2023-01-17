/*
* Copyright 2020 NVIDIA Corporation.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#pragma once

#include <assert.h>
#include <atomic>
#include <iostream>
#include <queue>
#include <sstream>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "vulkan_interfaces.h"
#include "VkCodecUtils/VulkanVideoReferenceCountedPool.h"
#include "VkCodecUtils/VulkanDeviceContext.h"
#include "VkCodecUtils/Helpers.h"
#include "VkVideoDecoder/VulkanBistreamBufferImpl.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkCodecUtils/NvVideoSession.h"
#include "VulkanVideoFrameBuffer/VulkanVideoFrameBuffer.h"
#include "VulkanVideoParser.h"
#include "VulkanVideoParserIf.h"
#include "VkParserVideoPictureParameters.h"
#include "StdVideoPictureParametersSet.h"

struct Rect {
    int32_t l;
    int32_t t;
    int32_t r;
    int32_t b;
};

struct Dim {
    int w, h;
};

struct NvVkDecodeFrameDataSlot {
    uint32_t                                            slot;
    VkCommandBuffer                                     commandBuffer;
};

using VulkanBitstreamBufferPool = VulkanVideoRefCountedPool<VulkanBitstreamBufferImpl, 64>;

class NvVkDecodeFrameData {

public:
    NvVkDecodeFrameData(const VulkanDeviceContext* vkDevCtx)
       : m_vkDevCtx(vkDevCtx),
         m_maxCodedWidth(),
         m_videoCommandPool(),
         m_bitstreamBuffersQueue() {}

    void deinit() {

        if (m_videoCommandPool) {
            assert(m_vkDevCtx);
            m_vkDevCtx->FreeCommandBuffers(*m_vkDevCtx, m_videoCommandPool, (uint32_t)m_commandBuffers.size(), &m_commandBuffers[0]);
            m_vkDevCtx->DestroyCommandPool(*m_vkDevCtx, m_videoCommandPool, NULL);
            m_videoCommandPool = VkCommandPool();
        }
    }

    ~NvVkDecodeFrameData() {
        deinit();
    }

    size_t resize(size_t maxDecodeFramesCount,
                  uint32_t maxCodedWidth,uint32_t maxCodedHeight,
                  VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling,
                  VkDeviceSize minBitstreamBufferOffsetAlignment,
                  VkDeviceSize minBitstreamBufferSizeAlignment) {

        assert(m_vkDevCtx);

        if (!m_videoCommandPool) {
            VkCommandPoolCreateInfo cmdPoolInfo = {};
            cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cmdPoolInfo.queueFamilyIndex = m_vkDevCtx->GetVideoDecodeQueueFamilyIdx();
            VkResult result = m_vkDevCtx->CreateCommandPool(*m_vkDevCtx, &cmdPoolInfo, nullptr, &m_videoCommandPool);
            assert(result == VK_SUCCESS);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "\nERROR: CreateCommandPool() result: 0x%x\n", result);
            }
        }

        const size_t oldCommandBuffersCount = m_commandBuffers.size();
        VkCommandBufferAllocateInfo cmdInfo = {};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandBufferCount = (uint32_t)(maxDecodeFramesCount - oldCommandBuffersCount);
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandPool = m_videoCommandPool;

        m_commandBuffers.resize(maxDecodeFramesCount);
        VkResult result = m_vkDevCtx->AllocateCommandBuffers(*m_vkDevCtx, &cmdInfo, &m_commandBuffers[oldCommandBuffersCount]);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: AllocateCommandBuffers() result: 0x%x\n", result);
        }

        m_maxCodedWidth = maxCodedWidth;

        return oldCommandBuffersCount;
    }

    VkCommandBuffer GetCommandBuffer(uint32_t slot) {
        assert(slot < m_commandBuffers.size());
        return m_commandBuffers[slot];
    }

    size_t size() {
        return m_commandBuffers.size();
    }

    VulkanBitstreamBufferPool& GetBitstreamBuffersQueue() { return m_bitstreamBuffersQueue; }

private:
    const VulkanDeviceContext*                                m_vkDevCtx;
    uint32_t                                                  m_maxCodedWidth;
    VkCommandPool                                             m_videoCommandPool;
    std::vector<VkCommandBuffer>                              m_commandBuffers;
    VulkanBitstreamBufferPool                                 m_bitstreamBuffersQueue;
};

/**
 * @brief Base class for decoder interface.
 */
class VkVideoDecoder : public IVulkanVideoDecoderHandler {
public:
    VkPhysicalDevice GetPhysDevice() { return m_vkDevCtx ? m_vkDevCtx->getPhysicalDevice() : VK_NULL_HANDLE; }
    enum { MAX_RENDER_TARGETS = 32 }; // Must be 32 or less (used as uint32_t bitmask of active render targets)

    static VkSharedBaseObj<VkVideoDecoder> invalidVkDecoder;

    static VkResult Create(const VulkanDeviceContext* vkDevCtx,
                           VkSharedBaseObj<VulkanVideoFrameBuffer>& videoFrameBuffer,
                           int32_t videoQueueIndx = 0,
                           bool useLinearOutput = false,
                           VkSharedBaseObj<VkVideoDecoder>& vkVideoDecoder = invalidVkDecoder);

    static const char* GetVideoCodecString(VkVideoCodecOperationFlagBitsKHR codec);
    static const char* GetVideoChromaFormatString(VkVideoChromaSubsamplingFlagBitsKHR chromaFormat);
    static uint32_t GetNumDecodeSurfaces(VkVideoCodecOperationFlagBitsKHR codec, uint32_t minNumDecodeSurfaces, uint32_t width,
        uint32_t height);

    virtual int32_t AddRef();
    virtual int32_t Release();

    /**
     *   @brief  This function is used to get information about the video stream (codec, display parameters etc)
     */
    const VkParserDetectedVideoFormat* GetVideoFormatInfo()
    {
        assert(m_videoFormat.coded_width);
        return &m_videoFormat;
    }

    /**
    *   @brief  This callback function gets called when when decoding of sequence starts,
    */
    virtual int32_t StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat);

    virtual bool UpdatePictureParameters(VkPictureParameters* pPictureParameters,
                                         VkSharedBaseObj<VkVideoRefCountBase>& pictureParametersObject,
                                         uint64_t updateSequenceCount);

    /**
     *   @brief  This callback function gets called when a picture is ready to be decoded.
     */
    virtual int32_t DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams, VkParserDecodePictureInfo* pDecodePictureInfo);

    virtual size_t GetBitstreamBuffer(size_t size, const uint8_t* pInitializeBufferMemory, size_t initializeBufferMemorySize,
                                      VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer);
private:

    VkVideoDecoder(const VulkanDeviceContext* vkDevCtx,
                   VkSharedBaseObj<VulkanVideoFrameBuffer>& videoFrameBuffer,
                   int32_t videoQueueIndx = 0, bool useLinearOutput = false)
        : m_vkDevCtx(vkDevCtx)
        , m_defaultVideoQueueIndx(videoQueueIndx)
        , m_refCount(0)
        , m_videoFormat {}
        , m_numDecodeSurfaces()
        , m_maxDecodeFramesCount(0)
        , m_capabilityFlags()
        , m_videoSession(nullptr)
        , m_videoFrameBuffer(videoFrameBuffer)
        , m_decodeFramesData(vkDevCtx)
        , m_decodePicCount(0)
        , m_lastIdInQueue{-1, -1, -1}
        , m_useImageArray(false)
        , m_useImageViewArray(false)
        , m_useSeparateOutputImages(useLinearOutput)
        , m_useLinearOutput(useLinearOutput)
        , m_resetDecoder(true)
        , m_dumpDecodeData(false)
        , m_maxStreamBufferSize()
    {

        assert(m_vkDevCtx->GetVideoDecodeQueueFamilyIdx() != -1);
        assert(m_vkDevCtx->GetVideoDecodeNumQueues() > 0);

        if (m_defaultVideoQueueIndx < 0) {
            m_defaultVideoQueueIndx = m_vkDevCtx->GetVideoDecodeDefaultQueueIndex();
        } else if (m_vkDevCtx->GetVideoDecodeNumQueues() > 1) {
            m_defaultVideoQueueIndx %= m_vkDevCtx->GetVideoDecodeNumQueues();
            assert(m_defaultVideoQueueIndx < m_vkDevCtx->GetVideoDecodeNumQueues());
            assert(m_defaultVideoQueueIndx >= 0);
        } else {
            m_defaultVideoQueueIndx = 0;
        }

    }

    virtual ~VkVideoDecoder();
    void Deinitialize();

    VkParserVideoPictureParameters*  AddPictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& vpsStdPictureParametersSet,
                                                          VkSharedBaseObj<StdVideoPictureParametersSet>& spsStdPictureParametersSet,
                                                          VkSharedBaseObj<StdVideoPictureParametersSet>& ppsStdPictureParametersSet);

    bool CheckStdObjectBeforeUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet);
    VkParserVideoPictureParameters* CheckStdObjectAfterUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& stdPictureParametersSet, VkParserVideoPictureParameters* pNewPictureParametersObject);
    uint32_t AddPictureParametersToQueue(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet);
    uint32_t FlushPictureParametersQueue();

    int CopyOptimalToLinearImage(VkCommandBuffer& commandBuffer,
                                 VkVideoPictureResourceInfoKHR& srcPictureResource,
                                 VulkanVideoFrameBuffer::PictureResourceInfo& srcPictureResourceInfo,
                                 VkVideoPictureResourceInfoKHR& dstPictureResource,
                                 VulkanVideoFrameBuffer::PictureResourceInfo& dstPictureResourceInfo,
                                 VulkanVideoFrameBuffer::FrameSynchronizationInfo *pFrameSynchronizationInfo);

    int32_t GetCurrentFrameData(uint32_t slotId, NvVkDecodeFrameDataSlot& frameDataSlot)
    {
        if (slotId < m_decodeFramesData.size()) {
            frameDataSlot.commandBuffer   = m_decodeFramesData.GetCommandBuffer(slotId);
            frameDataSlot.slot = slotId;
            return slotId;
        }
        return -1;
    }

private:
    const VulkanDeviceContext*  m_vkDevCtx;
    int32_t                     m_defaultVideoQueueIndx;
    std::atomic<int32_t>        m_refCount;
    // dimension of the output
    VkParserDetectedVideoFormat m_videoFormat;
    uint32_t                    m_numDecodeSurfaces;
    uint32_t                    m_maxDecodeFramesCount;

    VkVideoDecodeCapabilityFlagBitsKHR      m_capabilityFlags;
    VkSharedBaseObj<NvVideoSession>         m_videoSession;
    VkSharedBaseObj<VulkanVideoFrameBuffer> m_videoFrameBuffer;
    NvVkDecodeFrameData                     m_decodeFramesData;

    int32_t                                                    m_decodePicCount;
    int32_t                                                    m_lastIdInQueue[StdVideoPictureParametersSet::NUM_OF_TYPES];
    std::queue<VkSharedBaseObj<StdVideoPictureParametersSet>>  m_pictureParametersQueue;
    VkSharedBaseObj<StdVideoPictureParametersSet>              m_lastPictParamsQueue[StdVideoPictureParametersSet::NUM_OF_TYPES];
    VkSharedBaseObj<VkParserVideoPictureParameters>            m_currentPictureParameters;
    uint32_t m_useImageArray : 1;
    uint32_t m_useImageViewArray : 1;
    uint32_t m_useSeparateOutputImages : 1;
    uint32_t m_useLinearOutput : 1;
    uint32_t m_resetDecoder : 1;
    uint32_t m_dumpDecodeData : 1;
    size_t   m_maxStreamBufferSize;
};