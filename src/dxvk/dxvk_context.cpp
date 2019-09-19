#include <cstring>

#include "dxvk_device.h"
#include "dxvk_context.h"
#include "dxvk_main.h"

namespace dxvk {
  
  DxvkContext::DxvkContext(const Rc<DxvkDevice>& device)
  : m_device      (device),
    m_common      (&device->m_objects),
    m_sdmaAcquires(DxvkCmdBuffer::SdmaBuffer),
    m_sdmaBarriers(DxvkCmdBuffer::SdmaBuffer),
    m_initBarriers(DxvkCmdBuffer::InitBuffer),
    m_execAcquires(DxvkCmdBuffer::ExecBuffer),
    m_execBarriers(DxvkCmdBuffer::ExecBuffer),
    m_queryManager(m_common->queryPool()),
    m_staging     (device) {

  }
  
  
  DxvkContext::~DxvkContext() {
    
  }
  
  
  void DxvkContext::beginRecording(const Rc<DxvkCommandList>& cmdList) {
    m_cmd = cmdList;
    m_cmd->beginRecording();

    // Mark all resources as untracked
    m_vbTracked.clear();
    m_rcTracked.clear();
    
    // The current state of the internal command buffer is
    // undefined, so we have to bind and set up everything
    // before any draw or dispatch command is recorded.
    m_flags.clr(
      DxvkContextFlag::GpRenderPassBound,
      DxvkContextFlag::GpXfbActive,
      DxvkContextFlag::GpClearRenderTargets);
    
    m_flags.set(
      DxvkContextFlag::GpDirtyPipeline,
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyResources,
      DxvkContextFlag::GpDirtyVertexBuffers,
      DxvkContextFlag::GpDirtyIndexBuffer,
      DxvkContextFlag::GpDirtyXfbBuffers,
      DxvkContextFlag::GpDirtyBlendConstants,
      DxvkContextFlag::GpDirtyStencilRef,
      DxvkContextFlag::GpDirtyViewport,
      DxvkContextFlag::GpDirtyDepthBias,
      DxvkContextFlag::GpDirtyDepthBounds,
      DxvkContextFlag::CpDirtyPipeline,
      DxvkContextFlag::CpDirtyPipelineState,
      DxvkContextFlag::CpDirtyResources,
      DxvkContextFlag::DirtyDrawBuffer);
  }
  
  
  Rc<DxvkCommandList> DxvkContext::endRecording() {
    this->spillRenderPass();
    
    m_sdmaBarriers.recordCommands(m_cmd);
    m_initBarriers.recordCommands(m_cmd);
    m_execBarriers.recordCommands(m_cmd);

    m_cmd->endRecording();
    return std::exchange(m_cmd, nullptr);
  }


  void DxvkContext::flushCommandList() {
    m_device->submitCommandList(
      this->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
    
    this->beginRecording(
      m_device->createCommandList());
  }
  
  
  void DxvkContext::beginQuery(const Rc<DxvkGpuQuery>& query) {
    m_queryManager.enableQuery(m_cmd, query);
  }


  void DxvkContext::endQuery(const Rc<DxvkGpuQuery>& query) {
    m_queryManager.disableQuery(m_cmd, query);
  }
  
  
  void DxvkContext::bindRenderTargets(
    const DxvkRenderTargets&    targets,
          bool                  spill) {
    // If necessary, perform clears on the active render targets
    if (m_flags.test(DxvkContextFlag::GpClearRenderTargets))
      this->clearRenderPass();
    
    // Set up default render pass ops
    m_state.om.renderTargets = targets;
    
    this->resetRenderPassOps(
      m_state.om.renderTargets,
      m_state.om.renderPassOps);

    if (m_state.om.framebuffer == nullptr || !m_state.om.framebuffer->hasTargets(targets)) {
      // Create a new framebuffer object next
      // time we start rendering something
      m_flags.set(DxvkContextFlag::GpDirtyFramebuffer);
    } else {
      // Don't redundantly spill the render pass if
      // the same render targets are bound again
      m_flags.clr(DxvkContextFlag::GpDirtyFramebuffer);
    }

    if (spill)
      this->spillRenderPass();
  }
  
  
  void DxvkContext::bindDrawBuffers(
    const DxvkBufferSlice&      argBuffer,
    const DxvkBufferSlice&      cntBuffer) {
    m_state.id.argBuffer = argBuffer;
    m_state.id.cntBuffer = cntBuffer;

    m_flags.set(DxvkContextFlag::DirtyDrawBuffer);
  }


  void DxvkContext::bindIndexBuffer(
    const DxvkBufferSlice&      buffer,
          VkIndexType           indexType) {
    if (!m_state.vi.indexBuffer.matchesBuffer(buffer))
      m_vbTracked.clr(MaxNumVertexBindings);

    m_state.vi.indexBuffer = buffer;
    m_state.vi.indexType   = indexType;

    m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer);
  }
  
  
  void DxvkContext::bindResourceBuffer(
          uint32_t              slot,
    const DxvkBufferSlice&      buffer) {
    bool needsUpdate = !m_rc[slot].bufferSlice.matchesBuffer(buffer);

    if (likely(needsUpdate))
      m_rcTracked.clr(slot);
    else
      needsUpdate = m_rc[slot].bufferSlice.length() != buffer.length();

    if (likely(needsUpdate)) {
      m_flags.set(
        DxvkContextFlag::CpDirtyResources,
        DxvkContextFlag::GpDirtyResources);
    } else {
      m_flags.set(
        DxvkContextFlag::CpDirtyDescriptorOffsets,
        DxvkContextFlag::GpDirtyDescriptorOffsets);
    }

    m_rc[slot].bufferSlice = buffer;
  }
  
  
  void DxvkContext::bindResourceView(
          uint32_t              slot,
    const Rc<DxvkImageView>&    imageView,
    const Rc<DxvkBufferView>&   bufferView) {
    m_rc[slot].imageView   = imageView;
    m_rc[slot].bufferView  = bufferView;
    m_rc[slot].bufferSlice = bufferView != nullptr
      ? bufferView->slice()
      : DxvkBufferSlice();
    m_rcTracked.clr(slot);

    m_flags.set(
      DxvkContextFlag::CpDirtyResources,
      DxvkContextFlag::GpDirtyResources);
  }
  
  
  void DxvkContext::bindResourceSampler(
          uint32_t              slot,
    const Rc<DxvkSampler>&      sampler) {
    m_rc[slot].sampler = sampler;
    m_rcTracked.clr(slot);

    m_flags.set(
      DxvkContextFlag::CpDirtyResources,
      DxvkContextFlag::GpDirtyResources);
  }
  
  
  void DxvkContext::bindShader(
          VkShaderStageFlagBits stage,
    const Rc<DxvkShader>&       shader) {
    Rc<DxvkShader>* shaderStage;
    
    switch (stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:                  shaderStage = &m_state.gp.shaders.vs;  break;
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    shaderStage = &m_state.gp.shaders.tcs; break;
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: shaderStage = &m_state.gp.shaders.tes; break;
      case VK_SHADER_STAGE_GEOMETRY_BIT:                shaderStage = &m_state.gp.shaders.gs;  break;
      case VK_SHADER_STAGE_FRAGMENT_BIT:                shaderStage = &m_state.gp.shaders.fs;  break;
      case VK_SHADER_STAGE_COMPUTE_BIT:                 shaderStage = &m_state.cp.shaders.cs;  break;
      default: return;
    }
    
    *shaderStage = shader;

    if (stage == VK_SHADER_STAGE_COMPUTE_BIT) {
      m_flags.set(
        DxvkContextFlag::CpDirtyPipeline,
        DxvkContextFlag::CpDirtyPipelineState,
        DxvkContextFlag::CpDirtyResources);
    } else {
      m_flags.set(
        DxvkContextFlag::GpDirtyPipeline,
        DxvkContextFlag::GpDirtyPipelineState,
        DxvkContextFlag::GpDirtyResources);
    }
  }
  
  
  void DxvkContext::bindVertexBuffer(
          uint32_t              binding,
    const DxvkBufferSlice&      buffer,
          uint32_t              stride) {
    if (!m_state.vi.vertexBuffers[binding].matchesBuffer(buffer))
      m_vbTracked.clr(binding);

    m_state.vi.vertexBuffers[binding] = buffer;
    m_flags.set(DxvkContextFlag::GpDirtyVertexBuffers);
    
    if (unlikely(!buffer.defined()))
      stride = 0;
    
    if (unlikely(m_state.vi.vertexStrides[binding] != stride)) {
      m_state.vi.vertexStrides[binding] = stride;
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
  }
  
  
  void DxvkContext::bindXfbBuffer(
          uint32_t              binding,
    const DxvkBufferSlice&      buffer,
    const DxvkBufferSlice&      counter) {
    this->spillRenderPass();

    m_state.xfb.buffers [binding] = buffer;
    m_state.xfb.counters[binding] = counter;
    
    m_flags.set(DxvkContextFlag::GpDirtyXfbBuffers);
  }


  void DxvkContext::blitImage(
    const Rc<DxvkImage>&        dstImage,
    const Rc<DxvkImage>&        srcImage,
    const VkImageBlit&          region,
          VkFilter              filter) {
    this->spillRenderPass();

    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);

    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // Prepare the two images for transfer ops if necessary
    auto dstLayout = dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto srcLayout = srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    if (dstImage->info().layout != dstLayout) {
      m_execAcquires.accessImage(
        dstImage, dstSubresourceRange,
        dstImage->info().layout, 0, 0,
        dstLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    if (srcImage->info().layout != srcLayout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout, 0, 0,
        srcLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    }

    m_execAcquires.recordCommands(m_cmd);

    // Perform the blit operation
    m_cmd->cmdBlitImage(
      srcImage->handle(), srcLayout,
      dstImage->handle(), dstLayout,
      1, &region, filter);
    
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange, dstLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);
    
    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange, srcLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);

    m_cmd->trackResource(dstImage);
    m_cmd->trackResource(srcImage);
  }


  void DxvkContext::changeImageLayout(
    const Rc<DxvkImage>&        image,
          VkImageLayout         layout) {
    if (image->info().layout != layout) {
      this->spillRenderPass();

      VkImageSubresourceRange subresources;
      subresources.aspectMask     = image->formatInfo()->aspectMask;
      subresources.baseArrayLayer = 0;
      subresources.baseMipLevel   = 0;
      subresources.layerCount     = image->info().numLayers;
      subresources.levelCount     = image->info().mipLevels;

      if (m_execBarriers.isImageDirty(image, subresources, DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);

      m_execBarriers.accessImage(image, subresources,
        image->info().layout,
        image->info().stages,
        image->info().access,
        layout,
        image->info().layout,
        image->info().stages);

      image->setLayout(layout);
    }
  }


  void DxvkContext::clearBuffer(
    const Rc<DxvkBuffer>&       buffer,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          uint32_t              value) {
    this->spillRenderPass();
    
    length = align(length, sizeof(uint32_t));
    auto slice = buffer->getSliceHandle(offset, length);

    if (m_execBarriers.isBufferDirty(slice, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    m_cmd->cmdFillBuffer(
      slice.handle,
      slice.offset,
      slice.length,
      value);
    
    m_execBarriers.accessBuffer(slice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      buffer->info().stages,
      buffer->info().access);
    
    m_cmd->trackResource(buffer);
  }
  
  
  void DxvkContext::clearBufferView(
    const Rc<DxvkBufferView>&   bufferView,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          VkClearColorValue     value) {
    this->spillRenderPass();
    this->unbindComputePipeline();

    // The view range might have been invalidated, so
    // we need to make sure the handle is up to date
    bufferView->updateView();

    auto bufferSlice = bufferView->getSliceHandle();

    if (m_execBarriers.isBufferDirty(bufferSlice, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // Query pipeline objects to use for this clear operation
    DxvkMetaClearPipeline pipeInfo = m_common->metaClear().getClearBufferPipeline(
      imageFormatInfo(bufferView->info().format)->flags);
    
    // Create a descriptor set pointing to the view
    VkBufferView viewObject = bufferView->handle();
    
    VkDescriptorSet descriptorSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    
    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstSet           = descriptorSet;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    descriptorWrite.pImageInfo       = nullptr;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = &viewObject;
    m_cmd->updateDescriptorSets(1, &descriptorWrite);
    
    // Prepare shader arguments
    DxvkMetaClearArgs pushArgs;
    pushArgs.clearValue = value;
    pushArgs.offset = VkOffset3D {  int32_t(offset), 0, 0 };
    pushArgs.extent = VkExtent3D { uint32_t(length), 1, 1 };
    
    VkExtent3D workgroups = util::computeBlockCount(
      pushArgs.extent, pipeInfo.workgroupSize);
    
    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeline);
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, descriptorSet,
      0, nullptr);
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(pushArgs), &pushArgs);
    m_cmd->cmdDispatch(
      workgroups.width,
      workgroups.height,
      workgroups.depth);
    
    m_execBarriers.accessBuffer(bufferSlice,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      bufferView->bufferInfo().stages,
      bufferView->bufferInfo().access);
    
    m_cmd->trackResource(bufferView);
    m_cmd->trackResource(bufferView->buffer());
  }
  
  
  void DxvkContext::clearColorImage(
    const Rc<DxvkImage>&            image,
    const VkClearColorValue&        value,
    const VkImageSubresourceRange&  subresources) {
    this->spillRenderPass();

    m_execBarriers.recordCommands(m_cmd);
    
    VkImageLayout imageLayoutClear = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    m_execBarriers.accessImage(image, subresources,
      VK_IMAGE_LAYOUT_UNDEFINED,
      image->info().stages,
      image->info().access,
      imageLayoutClear,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_execBarriers.recordCommands(m_cmd);
    
    m_cmd->cmdClearColorImage(image->handle(),
      imageLayoutClear, &value, 1, &subresources);
    
    m_execBarriers.accessImage(image, subresources,
      imageLayoutClear,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      image->info().layout,
      image->info().stages,
      image->info().access);
    
    m_cmd->trackResource(image);
  }
  
  
  void DxvkContext::clearDepthStencilImage(
    const Rc<DxvkImage>&            image,
    const VkClearDepthStencilValue& value,
    const VkImageSubresourceRange&  subresources) {
    this->spillRenderPass();
    
    m_execBarriers.recordCommands(m_cmd);

    VkImageLayout imageLayoutInitial = image->info().layout;
    VkImageLayout imageLayoutClear   = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (subresources.aspectMask == image->formatInfo()->aspectMask)
      imageLayoutInitial = VK_IMAGE_LAYOUT_UNDEFINED;

    m_execBarriers.accessImage(
      image, subresources,
      imageLayoutInitial,
      image->info().stages,
      image->info().access,
      imageLayoutClear,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_execBarriers.recordCommands(m_cmd);
    
    m_cmd->cmdClearDepthStencilImage(image->handle(),
      imageLayoutClear, &value, 1, &subresources);
    
    m_execBarriers.accessImage(
      image, subresources,
      imageLayoutClear,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      image->info().layout,
      image->info().stages,
      image->info().access);
    
    m_cmd->trackResource(image);
  }


  void DxvkContext::clearCompressedColorImage(
    const Rc<DxvkImage>&            image,
    const VkImageSubresourceRange&  subresources) {
    this->spillRenderPass();

    // Allocate enough staging buffer memory to fit one
    // single subresource, then dispatch multiple copies
    VkDeviceSize dataSize = util::computeImageDataSize(
      image->info().format,
      image->mipLevelExtent(subresources.baseMipLevel));
    
    auto stagingSlice = m_staging.alloc(CACHE_LINE_SIZE, dataSize);
    auto stagingHandle = stagingSlice.getSliceHandle();

    std::memset(stagingHandle.mapPtr, 0, dataSize);

    if (m_execBarriers.isImageDirty(image, subresources, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    m_execAcquires.accessImage(
      image, subresources,
      VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
      image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);
    
    m_execAcquires.recordCommands(m_cmd);

    for (uint32_t level = 0; level < subresources.levelCount; level++) {
      VkOffset3D offset = VkOffset3D { 0, 0, 0 };
      VkExtent3D extent = image->mipLevelExtent(subresources.baseMipLevel + level);

      for (uint32_t layer = 0; layer < subresources.layerCount; layer++) {
        VkBufferImageCopy region;
        region.bufferOffset       = stagingHandle.offset;
        region.bufferRowLength    = 0;
        region.bufferImageHeight  = 0;
        region.imageSubresource   = vk::makeSubresourceLayers(
          vk::pickSubresource(subresources, level, layer));
        region.imageOffset        = offset;
        region.imageExtent        = extent;

        m_cmd->cmdCopyBufferToImage(DxvkCmdBuffer::ExecBuffer,
          stagingHandle.handle, image->handle(),
          image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
          1, &region);
      }
    }

    m_execBarriers.accessImage(
      image, subresources,
      image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      image->info().layout,
      image->info().stages,
      image->info().access);
    
    m_cmd->trackResource(image);
    m_cmd->trackResource(stagingSlice.buffer());
  }
  
  
  void DxvkContext::clearRenderTarget(
    const Rc<DxvkImageView>&    imageView,
          VkImageAspectFlags    clearAspects,
          VkClearValue          clearValue) {
    this->updateFramebuffer();

    // Prepare attachment ops
    DxvkColorAttachmentOps colorOp;
    colorOp.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorOp.loadLayout    = imageView->imageInfo().layout;
    colorOp.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
    colorOp.storeLayout   = imageView->imageInfo().layout;
    
    DxvkDepthAttachmentOps depthOp;
    depthOp.loadOpD       = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthOp.loadOpS       = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthOp.loadLayout    = imageView->imageInfo().layout;
    depthOp.storeOpD      = VK_ATTACHMENT_STORE_OP_STORE;
    depthOp.storeOpS      = VK_ATTACHMENT_STORE_OP_STORE;
    depthOp.storeLayout   = imageView->imageInfo().layout;
    
    if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT)
      colorOp.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    
    if (clearAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      depthOp.loadOpD = VK_ATTACHMENT_LOAD_OP_CLEAR;
    
    if (clearAspects & VK_IMAGE_ASPECT_STENCIL_BIT)
      depthOp.loadOpS = VK_ATTACHMENT_LOAD_OP_CLEAR;
    
    if (clearAspects == imageView->info().aspect
     && imageView->imageInfo().type != VK_IMAGE_TYPE_3D) {
      colorOp.loadLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
      depthOp.loadLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    
    // Make sure the color components are ordered correctly
    if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      clearValue.color = util::swizzleClearColor(clearValue.color,
        util::invertComponentMapping(imageView->info().swizzle));
    }
    
    // Check whether the render target view is an attachment
    // of the current framebuffer and is included entirely.
    // If not, we need to create a temporary framebuffer.
    int32_t attachmentIndex = -1;
    
    if (m_state.om.framebuffer != nullptr
     && m_state.om.framebuffer->isFullSize(imageView))
      attachmentIndex = m_state.om.framebuffer->findAttachment(imageView);
    
    if (attachmentIndex < 0) {
      this->spillRenderPass();

      if (m_execBarriers.isImageDirty(
          imageView->image(),
          imageView->imageSubresources(),
          DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
      
      // Set up and bind a temporary framebuffer
      DxvkRenderTargets attachments;
      DxvkRenderPassOps ops;

      VkPipelineStageFlags clearStages = 0;
      VkAccessFlags        clearAccess = 0;
      
      if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
        clearStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        clearAccess |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        attachments.color[0].view   = imageView;
        attachments.color[0].layout = imageView->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        
        ops.colorOps[0] = colorOp;
      } else {
        clearStages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                    |  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        clearAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        attachments.depth.view   = imageView;
        attachments.depth.layout = imageView->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        
        ops.depthOps = depthOp;
      }
      
      this->renderPassBindFramebuffer(
        m_device->createFramebuffer(attachments),
        ops, 1, &clearValue);
      this->renderPassUnbindFramebuffer();

      m_execBarriers.accessImage(
        imageView->image(),
        imageView->imageSubresources(),
        imageView->imageInfo().layout,
        clearStages, clearAccess,
        imageView->imageInfo().layout,
        imageView->imageInfo().stages,
        imageView->imageInfo().access);
    } else if (m_flags.test(DxvkContextFlag::GpRenderPassBound)) {
      // Clear the attachment in quesion. For color images,
      // the attachment index for the current subpass is
      // equal to the render pass attachment index.
      VkClearAttachment clearInfo;
      clearInfo.aspectMask      = clearAspects;
      clearInfo.colorAttachment = attachmentIndex;
      clearInfo.clearValue      = clearValue;
      
      VkClearRect clearRect;
      clearRect.rect.offset.x       = 0;
      clearRect.rect.offset.y       = 0;
      clearRect.rect.extent.width   = imageView->mipLevelExtent(0).width;
      clearRect.rect.extent.height  = imageView->mipLevelExtent(0).height;
      clearRect.baseArrayLayer      = 0;
      clearRect.layerCount          = imageView->info().numLayers;

      m_cmd->cmdClearAttachments(1, &clearInfo, 1, &clearRect);
    } else {
      // Perform the clear when starting the render pass
      if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
        m_state.om.renderPassOps.colorOps[attachmentIndex] = colorOp;
        m_state.om.clearValues[attachmentIndex].color = clearValue.color;
      }
      
      if (clearAspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
        m_state.om.renderPassOps.depthOps.loadOpD  = depthOp.loadOpD;
        m_state.om.renderPassOps.depthOps.storeOpD = depthOp.storeOpD;
        m_state.om.clearValues[attachmentIndex].depthStencil.depth = clearValue.depthStencil.depth;
      }
      
      if (clearAspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
        m_state.om.renderPassOps.depthOps.loadOpS  = depthOp.loadOpS;
        m_state.om.renderPassOps.depthOps.storeOpS = depthOp.storeOpS;
        m_state.om.clearValues[attachmentIndex].depthStencil.stencil = clearValue.depthStencil.stencil;
      }

      if (clearAspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        m_state.om.renderPassOps.depthOps.loadLayout  = depthOp.loadLayout;
        m_state.om.renderPassOps.depthOps.storeLayout = depthOp.storeLayout;

        if (m_state.om.renderPassOps.depthOps.loadOpD == VK_ATTACHMENT_LOAD_OP_CLEAR
         && m_state.om.renderPassOps.depthOps.loadOpS == VK_ATTACHMENT_LOAD_OP_CLEAR)
          m_state.om.renderPassOps.depthOps.loadLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      }
      
      m_flags.set(DxvkContextFlag::GpClearRenderTargets);
    }
  }
  
  
  void DxvkContext::clearImageView(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkImageAspectFlags    aspect,
          VkClearValue          value) {
    const VkImageUsageFlags viewUsage = imageView->info().usage;

    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      value.color = util::swizzleClearColor(value.color,
        util::invertComponentMapping(imageView->info().swizzle));
    }
    
    if (viewUsage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
      this->clearImageViewFb(imageView, offset, extent, aspect, value);
    else if (viewUsage & VK_IMAGE_USAGE_STORAGE_BIT)
      this->clearImageViewCs(imageView, offset, extent, value);
  }
  
  
  void DxvkContext::copyBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkDeviceSize          numBytes) {
    if (numBytes == 0)
      return;
    
    this->spillRenderPass();
    
    auto dstSlice = dstBuffer->getSliceHandle(dstOffset, numBytes);
    auto srcSlice = srcBuffer->getSliceHandle(srcOffset, numBytes);

    if (m_execBarriers.isBufferDirty(srcSlice, DxvkAccess::Read)
     || m_execBarriers.isBufferDirty(dstSlice, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    VkBufferCopy bufferRegion;
    bufferRegion.srcOffset = srcSlice.offset;
    bufferRegion.dstOffset = dstSlice.offset;
    bufferRegion.size      = dstSlice.length;

    m_cmd->cmdCopyBuffer(DxvkCmdBuffer::ExecBuffer,
      srcSlice.handle, dstSlice.handle, 1, &bufferRegion);

    m_execBarriers.accessBuffer(srcSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcBuffer->info().stages,
      srcBuffer->info().access);

    m_execBarriers.accessBuffer(dstSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstBuffer->info().stages,
      dstBuffer->info().access);

    m_cmd->trackResource(dstBuffer);
    m_cmd->trackResource(srcBuffer);
  }
  
  
  void DxvkContext::copyBufferRegion(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
          VkDeviceSize          srcOffset,
          VkDeviceSize          numBytes) {
    VkDeviceSize loOvl = std::max(dstOffset, srcOffset);
    VkDeviceSize hiOvl = std::min(dstOffset, srcOffset) + numBytes;

    if (hiOvl > loOvl) {
      DxvkBufferCreateInfo bufInfo;
      bufInfo.size    = numBytes;
      bufInfo.usage   = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                      | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      bufInfo.stages  = VK_PIPELINE_STAGE_TRANSFER_BIT;
      bufInfo.access  = VK_ACCESS_TRANSFER_WRITE_BIT
                      | VK_ACCESS_TRANSFER_READ_BIT;

      auto tmpBuffer = m_device->createBuffer(
        bufInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      
      VkDeviceSize tmpOffset = 0;
      
      this->copyBuffer(tmpBuffer, tmpOffset, dstBuffer, srcOffset, numBytes);
      this->copyBuffer(dstBuffer, dstOffset, tmpBuffer, tmpOffset, numBytes);
    } else {
      this->copyBuffer(dstBuffer, dstOffset, dstBuffer, srcOffset, numBytes);
    }
  }


  void DxvkContext::copyBufferToImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkExtent3D            dstExtent,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkExtent2D            srcExtent) {
    this->spillRenderPass();

    auto srcSlice = srcBuffer->getSliceHandle(srcOffset, 0);

    // We may copy to only one aspect of a depth-stencil image,
    // but pipeline barriers need to have all aspect bits set
    auto dstFormatInfo = dstImage->formatInfo();

    auto dstSubresourceRange = vk::makeSubresourceRange(dstSubresource);
    dstSubresourceRange.aspectMask = dstFormatInfo->aspectMask;
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isBufferDirty(srcSlice, DxvkAccess::Read))
      m_execBarriers.recordCommands(m_cmd);

    // Initialize the image if the entire subresource is covered
    VkImageLayout dstImageLayoutInitial  = dstImage->info().layout;
    VkImageLayout dstImageLayoutTransfer = dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (dstImage->isFullSubresource(dstSubresource, dstExtent))
      dstImageLayoutInitial = VK_IMAGE_LAYOUT_UNDEFINED;

    m_execAcquires.accessImage(
      dstImage, dstSubresourceRange,
      dstImageLayoutInitial, 0, 0,
      dstImageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);
      
    m_execAcquires.recordCommands(m_cmd);
    
    VkBufferImageCopy copyRegion;
    copyRegion.bufferOffset       = srcSlice.offset;
    copyRegion.bufferRowLength    = srcExtent.width;
    copyRegion.bufferImageHeight  = srcExtent.height;
    copyRegion.imageSubresource   = dstSubresource;
    copyRegion.imageOffset        = dstOffset;
    copyRegion.imageExtent        = dstExtent;
    
    m_cmd->cmdCopyBufferToImage(DxvkCmdBuffer::ExecBuffer,
      srcSlice.handle, dstImage->handle(),
      dstImageLayoutTransfer, 1, &copyRegion);
    
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessBuffer(srcSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcBuffer->info().stages,
      srcBuffer->info().access);
    
    m_cmd->trackResource(dstImage);
    m_cmd->trackResource(srcBuffer);
  }
  
  
  void DxvkContext::copyImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    this->spillRenderPass();

    bool useFb = dstSubresource.aspectMask != srcSubresource.aspectMask;

    if (m_device->perfHints().preferFbDepthStencilCopy) {
      useFb |= (dstSubresource.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
            && (dstImage->info().usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
            && (srcImage->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    if (!useFb) {
      this->copyImageHw(
        dstImage, dstSubresource, dstOffset,
        srcImage, srcSubresource, srcOffset,
        extent);
    } else {
      this->copyImageFb(
        dstImage, dstSubresource, dstOffset,
        srcImage, srcSubresource, srcOffset,
        extent);
    }
  }
  
  
  void DxvkContext::copyImageRegion(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    VkOffset3D loOvl = {
      std::max(dstOffset.x, srcOffset.x),
      std::max(dstOffset.y, srcOffset.y),
      std::max(dstOffset.z, srcOffset.z) };
    
    VkOffset3D hiOvl = {
      std::min(dstOffset.x, srcOffset.x) + int32_t(extent.width),
      std::min(dstOffset.y, srcOffset.y) + int32_t(extent.height),
      std::min(dstOffset.z, srcOffset.z) + int32_t(extent.depth) };
    
    bool overlap = hiOvl.x > loOvl.x
                && hiOvl.y > loOvl.y
                && hiOvl.z > loOvl.z;
    
    if (overlap) {
      DxvkImageCreateInfo imgInfo;
      imgInfo.type          = dstImage->info().type;
      imgInfo.format        = dstImage->info().format;
      imgInfo.flags         = 0;
      imgInfo.sampleCount   = dstImage->info().sampleCount;
      imgInfo.extent        = extent;
      imgInfo.numLayers     = dstSubresource.layerCount;
      imgInfo.mipLevels     = 1;
      imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      imgInfo.stages        = VK_PIPELINE_STAGE_TRANSFER_BIT;
      imgInfo.access        = VK_ACCESS_TRANSFER_WRITE_BIT
                            | VK_ACCESS_TRANSFER_READ_BIT;
      imgInfo.tiling        = dstImage->info().tiling;
      imgInfo.layout        = VK_IMAGE_LAYOUT_GENERAL;

      auto tmpImage = m_device->createImage(
        imgInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      
      VkImageSubresourceLayers tmpSubresource;
      tmpSubresource.aspectMask     = dstSubresource.aspectMask;
      tmpSubresource.mipLevel       = 0;
      tmpSubresource.baseArrayLayer = 0;
      tmpSubresource.layerCount     = dstSubresource.layerCount;

      VkOffset3D tmpOffset = { 0, 0, 0 };

      this->copyImage(
        tmpImage, tmpSubresource, tmpOffset,
        dstImage, dstSubresource, srcOffset,
        extent);
      
      this->copyImage(
        dstImage, dstSubresource, dstOffset,
        tmpImage, tmpSubresource, tmpOffset,
        extent);
    } else {
      this->copyImage(
        dstImage, dstSubresource, dstOffset,
        dstImage, dstSubresource, srcOffset,
        extent);
    }
  }


  void DxvkContext::copyImageToBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
          VkExtent2D            dstExtent,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            srcExtent) {
    this->spillRenderPass();
    
    auto dstSlice = dstBuffer->getSliceHandle(dstOffset, 0);

    // We may copy to only one aspect of a depth-stencil image,
    // but pipeline barriers need to have all aspect bits set
    auto srcFormatInfo = srcImage->formatInfo();

    auto srcSubresourceRange = vk::makeSubresourceRange(srcSubresource);
    srcSubresourceRange.aspectMask = srcFormatInfo->aspectMask;
    
    if (m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isBufferDirty(dstSlice, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // Select a suitable image layout for the transfer op
    VkImageLayout srcImageLayoutTransfer = srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    
    m_execAcquires.accessImage(
      srcImage, srcSubresourceRange,
      srcImage->info().layout, 0, 0,
      srcImageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);

    m_execAcquires.recordCommands(m_cmd);
    
    VkBufferImageCopy copyRegion;
    copyRegion.bufferOffset       = dstSlice.offset;
    copyRegion.bufferRowLength    = dstExtent.width;
    copyRegion.bufferImageHeight  = dstExtent.height;
    copyRegion.imageSubresource   = srcSubresource;
    copyRegion.imageOffset        = srcOffset;
    copyRegion.imageExtent        = srcExtent;
    
    m_cmd->cmdCopyImageToBuffer(DxvkCmdBuffer::ExecBuffer,
      srcImage->handle(), srcImageLayoutTransfer,
      dstSlice.handle, 1, &copyRegion);
    
    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange,
      srcImageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);

    m_execBarriers.accessBuffer(dstSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstBuffer->info().stages,
      dstBuffer->info().access);
    
    m_cmd->trackResource(srcImage);
    m_cmd->trackResource(dstBuffer);
  }


  void DxvkContext::copyDepthStencilImageToPackedBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset2D            srcOffset,
          VkExtent2D            srcExtent,
          VkFormat              format) {
    this->spillRenderPass();
    this->unbindComputePipeline();

    // Retrieve compute pipeline for the given format
    auto pipeInfo = m_common->metaPack().getPackPipeline(format);

    if (!pipeInfo.pipeHandle)
      return;
    
    // Create one depth view and one stencil view
    DxvkImageViewCreateInfo dViewInfo;
    dViewInfo.type       = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dViewInfo.format     = srcImage->info().format;
    dViewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    dViewInfo.aspect     = VK_IMAGE_ASPECT_DEPTH_BIT;
    dViewInfo.minLevel   = srcSubresource.mipLevel;
    dViewInfo.numLevels  = 1;
    dViewInfo.minLayer   = srcSubresource.baseArrayLayer;
    dViewInfo.numLayers  = srcSubresource.layerCount;

    DxvkImageViewCreateInfo sViewInfo = dViewInfo;
    sViewInfo.aspect     = VK_IMAGE_ASPECT_STENCIL_BIT;
    
    Rc<DxvkImageView> dView = m_device->createImageView(srcImage, dViewInfo);
    Rc<DxvkImageView> sView = m_device->createImageView(srcImage, sViewInfo);

    // Create a descriptor set for the pack operation
    VkImageLayout layout = srcImage->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    DxvkMetaPackDescriptors descriptors;
    descriptors.dstBuffer  = dstBuffer->getDescriptor(dstOffset, VK_WHOLE_SIZE).buffer;
    descriptors.srcDepth   = dView->getDescriptor(VK_IMAGE_VIEW_TYPE_2D_ARRAY, layout).image;
    descriptors.srcStencil = sView->getDescriptor(VK_IMAGE_VIEW_TYPE_2D_ARRAY, layout).image;

    VkDescriptorSet dset = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSetWithTemplate(dset, pipeInfo.dsetTemplate, &descriptors);

    // Since this is a meta operation, the image may be
    // in a different layout and we have to transition it
    auto subresourceRange = vk::makeSubresourceRange(srcSubresource);

    if (m_execBarriers.isImageDirty(srcImage, subresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    if (srcImage->info().layout != layout) {
      m_execAcquires.accessImage(
        srcImage, subresourceRange,
        srcImage->info().layout, 0, 0,
        layout,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      
      m_execAcquires.recordCommands(m_cmd);
    }

    // Execute the actual pack operation
    DxvkMetaPackArgs args;
    args.srcOffset = srcOffset;
    args.srcExtent = srcExtent;

    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeHandle);
    
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, dset,
      0, nullptr);
    
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(args), &args);
    
    m_cmd->cmdDispatch(
      (srcExtent.width  + 7) / 8,
      (srcExtent.height + 7) / 8,
      srcSubresource.layerCount);
    
    m_execBarriers.accessImage(
      srcImage, subresourceRange, layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_execBarriers.accessBuffer(
      dstBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      dstBuffer->info().stages,
      dstBuffer->info().access);

    m_cmd->trackResource(dView);
    m_cmd->trackResource(sView);

    m_cmd->trackResource(srcImage);
    m_cmd->trackResource(dstBuffer);
  }
  
  
  void DxvkContext::copyPackedBufferToDepthStencilImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset2D            dstOffset,
          VkExtent2D            dstExtent,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkFormat              format) {
    this->spillRenderPass();
    this->unbindComputePipeline();

    if (m_execBarriers.isBufferDirty(srcBuffer->getSliceHandle(), DxvkAccess::Read))
      m_execBarriers.recordCommands(m_cmd);
    
    // Retrieve compute pipeline for the given format
    auto pipeInfo = m_common->metaPack().getUnpackPipeline(dstImage->info().format, format);

    if (!pipeInfo.pipeHandle) {
      Logger::err(str::format(
        "DxvkContext: copyPackedBufferToDepthStencilImage: Unhandled formats"
        "\n  dstFormat = ", dstImage->info().format,
        "\n  srcFormat = ", format));
      return;
    }
    
    // Pick depth and stencil data formats
    VkFormat dataFormatD = VK_FORMAT_UNDEFINED;
    VkFormat dataFormatS = VK_FORMAT_UNDEFINED;

    const std::array<std::tuple<VkFormat, VkFormat, VkFormat>, 2> formats = {{
      { VK_FORMAT_D24_UNORM_S8_UINT,  VK_FORMAT_R32_UINT,   VK_FORMAT_R8_UINT },
      { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8_UINT },
    }};

    for (const auto& e : formats) {
      if (std::get<0>(e) == dstImage->info().format) {
        dataFormatD = std::get<1>(e);
        dataFormatS = std::get<2>(e);
      }
    }

    // Create temporary buffer for depth/stencil data
    VkDeviceSize pixelCount = dstExtent.width * dstExtent.height * dstSubresource.layerCount;
    VkDeviceSize dataSizeD = align(pixelCount * imageFormatInfo(dataFormatD)->elementSize, 256);
    VkDeviceSize dataSizeS = align(pixelCount * imageFormatInfo(dataFormatS)->elementSize, 256);

    DxvkBufferCreateInfo tmpBufferInfo;
    tmpBufferInfo.size    = dataSizeD + dataSizeS;
    tmpBufferInfo.usage   = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
                          | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    tmpBufferInfo.stages  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                          | VK_PIPELINE_STAGE_TRANSFER_BIT;
    tmpBufferInfo.access  = VK_ACCESS_SHADER_WRITE_BIT
                          | VK_ACCESS_TRANSFER_READ_BIT;
    
    auto tmpBuffer = m_device->createBuffer(tmpBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Create formatted buffer views
    DxvkBufferViewCreateInfo tmpViewInfoD;
    tmpViewInfoD.format      = dataFormatD;
    tmpViewInfoD.rangeOffset = 0;
    tmpViewInfoD.rangeLength = dataSizeD;

    DxvkBufferViewCreateInfo tmpViewInfoS;
    tmpViewInfoS.format      = dataFormatS;
    tmpViewInfoS.rangeOffset = dataSizeD;
    tmpViewInfoS.rangeLength = dataSizeS;

    auto tmpBufferViewD = m_device->createBufferView(tmpBuffer, tmpViewInfoD);
    auto tmpBufferViewS = m_device->createBufferView(tmpBuffer, tmpViewInfoS);

    // Create descriptor set for the unpack operation
    DxvkMetaUnpackDescriptors descriptors;
    descriptors.dstDepth   = tmpBufferViewD->handle();
    descriptors.dstStencil = tmpBufferViewS->handle();
    descriptors.srcBuffer  = srcBuffer->getDescriptor(srcOffset, VK_WHOLE_SIZE).buffer;

    VkDescriptorSet dset = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSetWithTemplate(dset, pipeInfo.dsetTemplate, &descriptors);

    // Unpack the source buffer to temporary buffers
    DxvkMetaUnpackArgs args;
    args.dstExtent = dstExtent;
    args.srcExtent = dstExtent;

    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeHandle);
    
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, dset,
      0, nullptr);
    
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(args), &args);
    
    m_cmd->cmdDispatch(
      (dstExtent.width + 63) / 64,
      dstExtent.height,
      dstSubresource.layerCount);
    
    m_execBarriers.accessBuffer(
      tmpBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);

    m_execBarriers.accessBuffer(
      srcBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      srcBuffer->info().stages,
      srcBuffer->info().access);
    
    // Prepare image for the data transfer operation
    VkOffset3D dstOffset3D = { dstOffset.x,     dstOffset.y,      0 };
    VkExtent3D dstExtent3D = { dstExtent.width, dstExtent.height, 1 };

    VkImageLayout initialImageLayout = dstImage->info().layout;

    if (dstImage->isFullSubresource(dstSubresource, dstExtent3D))
      initialImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    m_execBarriers.accessImage(
      dstImage, vk::makeSubresourceRange(dstSubresource),
      initialImageLayout,
      dstImage->info().stages,
      dstImage->info().access,
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_execBarriers.recordCommands(m_cmd);

    // Copy temporary buffer data to depth-stencil image
    VkImageSubresourceLayers dstSubresourceD = dstSubresource;
    dstSubresourceD.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

    VkImageSubresourceLayers dstSubresourceS = dstSubresource;
    dstSubresourceS.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;

    std::array<VkBufferImageCopy, 2> copyRegions = {{
      { tmpBufferViewD->info().rangeOffset, 0, 0, dstSubresourceD, dstOffset3D, dstExtent3D },
      { tmpBufferViewS->info().rangeOffset, 0, 0, dstSubresourceS, dstOffset3D, dstExtent3D },
    }};

    m_cmd->cmdCopyBufferToImage(DxvkCmdBuffer::ExecBuffer,
      tmpBuffer->getSliceHandle().handle,
      dstImage->handle(),
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      copyRegions.size(),
      copyRegions.data());
    
    m_execBarriers.accessImage(
      dstImage, vk::makeSubresourceRange(dstSubresource),
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    // Track all involved resources
    m_cmd->trackResource(dstImage);
    m_cmd->trackResource(srcBuffer);

    m_cmd->trackResource(tmpBufferViewD);
    m_cmd->trackResource(tmpBufferViewS);
  }


  void DxvkContext::discardBuffer(
    const Rc<DxvkBuffer>&       buffer) {
    if (m_execBarriers.isBufferDirty(buffer->getSliceHandle(), DxvkAccess::Write))
      this->invalidateBuffer(buffer, buffer->allocSlice());
  }


  void DxvkContext::discardImage(
    const Rc<DxvkImage>&          image,
          VkImageSubresourceRange subresources) {
    this->spillRenderPass();

    if (m_execBarriers.isImageDirty(image, subresources, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    m_execBarriers.accessImage(image, subresources,
      VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
      image->info().layout,
      image->info().stages,
      image->info().access);
    
    m_cmd->trackResource(image);
  }


  void DxvkContext::dispatch(
          uint32_t x,
          uint32_t y,
          uint32_t z) {
    this->commitComputeState();
    
    if (m_cpActivePipeline) {
      this->commitComputeInitBarriers();

      m_queryManager.beginQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      m_cmd->cmdDispatch(x, y, z);
      
      m_queryManager.endQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      this->commitComputePostBarriers();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDispatchCalls, 1);
  }
  
  
  void DxvkContext::dispatchIndirect(
          VkDeviceSize      offset) {
    this->commitComputeState();
    
    auto bufferSlice = m_state.id.argBuffer.getSliceHandle(
      offset, sizeof(VkDispatchIndirectCommand));

    if (m_execBarriers.isBufferDirty(bufferSlice, DxvkAccess::Read))
      m_execBarriers.recordCommands(m_cmd);
    
    if (m_cpActivePipeline) {
      this->commitComputeInitBarriers();

      m_queryManager.beginQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      m_cmd->cmdDispatchIndirect(
        bufferSlice.handle,
        bufferSlice.offset);
      
      m_queryManager.endQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      this->commitComputePostBarriers();

      m_execBarriers.accessBuffer(bufferSlice,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        m_state.id.argBuffer.bufferInfo().stages,
        m_state.id.argBuffer.bufferInfo().access);
      
      this->trackDrawBuffer();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDispatchCalls, 1);
  }
  
  
  void DxvkContext::draw(
          uint32_t vertexCount,
          uint32_t instanceCount,
          uint32_t firstVertex,
          uint32_t firstInstance) {
    this->commitGraphicsState<false>();
    
    if (m_gpActivePipeline) {
      m_cmd->cmdDraw(
        vertexCount, instanceCount,
        firstVertex, firstInstance);
      
      this->commitGraphicsPostBarriers();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndirect(
          VkDeviceSize      offset,
          uint32_t          count,
          uint32_t          stride) {
    this->commitGraphicsState<false>();
    
    if (m_gpActivePipeline) {
      auto descriptor = m_state.id.argBuffer.getDescriptor();
      
      m_cmd->cmdDrawIndirect(
        descriptor.buffer.buffer,
        descriptor.buffer.offset + offset,
        count, stride);
      
      this->commitGraphicsPostBarriers();
      this->trackDrawBuffer();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndirectCount(
          VkDeviceSize      offset,
          VkDeviceSize      countOffset,
          uint32_t          maxCount,
          uint32_t          stride) {
    this->commitGraphicsState<false>();
    
    if (m_gpActivePipeline) {
      auto argDescriptor = m_state.id.argBuffer.getDescriptor();
      auto cntDescriptor = m_state.id.cntBuffer.getDescriptor();
      
      m_cmd->cmdDrawIndirectCount(
        argDescriptor.buffer.buffer,
        argDescriptor.buffer.offset + offset,
        cntDescriptor.buffer.buffer,
        cntDescriptor.buffer.offset + countOffset,
        maxCount, stride);
      
      this->commitGraphicsPostBarriers();
      this->trackDrawBuffer();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndexed(
          uint32_t indexCount,
          uint32_t instanceCount,
          uint32_t firstIndex,
          uint32_t vertexOffset,
          uint32_t firstInstance) {
    this->commitGraphicsState<true>();
    
    if (m_gpActivePipeline) {
      m_cmd->cmdDrawIndexed(
        indexCount, instanceCount,
        firstIndex, vertexOffset,
        firstInstance);
      
      this->commitGraphicsPostBarriers();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndexedIndirect(
          VkDeviceSize      offset,
          uint32_t          count,
          uint32_t          stride) {
    this->commitGraphicsState<true>();
    
    if (m_gpActivePipeline) {
      auto descriptor = m_state.id.argBuffer.getDescriptor();
      
      m_cmd->cmdDrawIndexedIndirect(
        descriptor.buffer.buffer,
        descriptor.buffer.offset + offset,
        count, stride);
      
      this->commitGraphicsPostBarriers();
      this->trackDrawBuffer();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndexedIndirectCount(
          VkDeviceSize      offset,
          VkDeviceSize      countOffset,
          uint32_t          maxCount,
          uint32_t          stride) {
    this->commitGraphicsState<true>();
    
    if (m_gpActivePipeline) {
      auto argDescriptor = m_state.id.argBuffer.getDescriptor();
      auto cntDescriptor = m_state.id.cntBuffer.getDescriptor();
      
      m_cmd->cmdDrawIndexedIndirectCount(
        argDescriptor.buffer.buffer,
        argDescriptor.buffer.offset + offset,
        cntDescriptor.buffer.buffer,
        cntDescriptor.buffer.offset + countOffset,
        maxCount, stride);
      
      this->commitGraphicsPostBarriers();
      this->trackDrawBuffer();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndirectXfb(
    const DxvkBufferSlice&  counterBuffer,
          uint32_t          counterDivisor,
          uint32_t          counterBias) {
    this->commitGraphicsState<false>();

    if (m_gpActivePipeline) {
      auto physSlice = counterBuffer.getSliceHandle();

      m_cmd->cmdDrawIndirectVertexCount(1, 0,
        physSlice.handle,
        physSlice.offset,
        counterBias,
        counterDivisor);
      
      this->commitGraphicsPostBarriers();
    }

    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }


  void DxvkContext::initImage(
    const Rc<DxvkImage>&           image,
    const VkImageSubresourceRange& subresources) {
    m_execBarriers.accessImage(image, subresources,
      VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
      image->info().layout,
      image->info().stages,
      image->info().access);
    
    m_cmd->trackResource(image);
  }
  
  
  void DxvkContext::generateMipmaps(
    const Rc<DxvkImageView>&        imageView) {
    if (imageView->info().numLevels <= 1)
      return;
    
    this->spillRenderPass();

    m_execBarriers.recordCommands(m_cmd);
    
    // Create the a set of framebuffers and image views
    const Rc<DxvkMetaMipGenRenderPass> mipGenerator
      = new DxvkMetaMipGenRenderPass(m_device->vkd(), imageView);
    
    // Common descriptor set properties that we use to
    // bind the source image view to the fragment shader
    VkDescriptorImageInfo descriptorImage;
    descriptorImage.sampler     = VK_NULL_HANDLE;
    descriptorImage.imageView   = VK_NULL_HANDLE;
    descriptorImage.imageLayout = imageView->imageInfo().layout;
    
    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstSet           = VK_NULL_HANDLE;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.pImageInfo       = &descriptorImage;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    
    // Common render pass info
    VkRenderPassBeginInfo passInfo;
    passInfo.sType            = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.pNext            = nullptr;
    passInfo.renderPass       = mipGenerator->renderPass();
    passInfo.framebuffer      = VK_NULL_HANDLE;
    passInfo.renderArea       = VkRect2D { };
    passInfo.clearValueCount  = 0;
    passInfo.pClearValues     = nullptr;
    
    // Retrieve a compatible pipeline to use for rendering
    DxvkMetaMipGenPipeline pipeInfo = m_common->metaMipGen().getPipeline(
      mipGenerator->viewType(), imageView->info().format);
    
    for (uint32_t i = 0; i < mipGenerator->passCount(); i++) {
      DxvkMetaMipGenPass pass = mipGenerator->pass(i);
      
      // Width, height and layer count for the current pass
      VkExtent3D passExtent = mipGenerator->passExtent(i);
      
      // Create descriptor set with the current source view
      descriptorImage.imageView = pass.srcView;
      descriptorWrite.dstSet = allocateDescriptorSet(pipeInfo.dsetLayout);
      m_cmd->updateDescriptorSets(1, &descriptorWrite);
      
      // Set up viewport and scissor rect
      VkViewport viewport;
      viewport.x        = 0.0f;
      viewport.y        = 0.0f;
      viewport.width    = float(passExtent.width);
      viewport.height   = float(passExtent.height);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      
      VkRect2D scissor;
      scissor.offset    = { 0, 0 };
      scissor.extent    = { passExtent.width, passExtent.height };
      
      // Set up render pass info
      passInfo.framebuffer = pass.framebuffer;
      passInfo.renderArea  = scissor;
      
      // Set up push constants
      DxvkMetaMipGenPushConstants pushConstants;
      pushConstants.layerCount = passExtent.depth;
      
      m_cmd->cmdBeginRenderPass(&passInfo, VK_SUBPASS_CONTENTS_INLINE);
      m_cmd->cmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeInfo.pipeHandle);
      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeInfo.pipeLayout, descriptorWrite.dstSet, 0, nullptr);
      
      m_cmd->cmdSetViewport(0, 1, &viewport);
      m_cmd->cmdSetScissor (0, 1, &scissor);
      
      m_cmd->cmdPushConstants(
        pipeInfo.pipeLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(pushConstants),
        &pushConstants);
      
      m_cmd->cmdDraw(3, passExtent.depth, 0, 0);
      m_cmd->cmdEndRenderPass();
    }
    
    m_cmd->trackResource(mipGenerator);
    m_cmd->trackResource(imageView->image());
  }
  
  
  void DxvkContext::invalidateBuffer(
    const Rc<DxvkBuffer>&           buffer,
    const DxvkBufferSliceHandle&    slice) {
    // Allocate new backing resource
    DxvkBufferSliceHandle prevSlice = buffer->rename(slice);
    m_cmd->freeBufferSlice(buffer, prevSlice);
    
    // We also need to update all bindings that the buffer
    // may be bound to either directly or through views.
    const VkBufferUsageFlags usage = buffer->info().usage;
    
    if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::DirtyDrawBuffer);
    
    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer);
    
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::GpDirtyVertexBuffers);
    
    if (usage & VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT)
      m_flags.set(DxvkContextFlag::GpDirtyXfbBuffers);
    
    if (usage & (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
               | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) {
      m_flags.set(DxvkContextFlag::GpDirtyResources,
                  DxvkContextFlag::CpDirtyResources);
    }

    if (usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
               | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
      if (prevSlice.handle != slice.handle) {
        m_flags.set(DxvkContextFlag::GpDirtyResources,
                    DxvkContextFlag::CpDirtyResources);
      } else {
        m_flags.set(DxvkContextFlag::GpDirtyDescriptorOffsets,
                    DxvkContextFlag::CpDirtyDescriptorOffsets);
      }
    }
  }


  void DxvkContext::pushConstants(
          uint32_t                  offset,
          uint32_t                  size,
    const void*                     data) {
    std::memcpy(&m_state.pc.data[offset], data, size);

    m_flags.set(DxvkContextFlag::DirtyPushConstants);
  }
  
  
  void DxvkContext::resolveImage(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkFormat                  format) {
    this->spillRenderPass();
    
    if (format == VK_FORMAT_UNDEFINED)
      format = srcImage->info().format;
    
    if (srcImage->info().format == format
     && dstImage->info().format == format) {
      this->resolveImageHw(
        dstImage, srcImage, region);
    } else {
      this->resolveImageFb(
        dstImage, srcImage, region, format,
        VK_RESOLVE_MODE_NONE_KHR,
        VK_RESOLVE_MODE_NONE_KHR);
    }
  }


  void DxvkContext::resolveDepthStencilImage(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkResolveModeFlagBitsKHR  depthMode,
          VkResolveModeFlagBitsKHR  stencilMode) {
    this->spillRenderPass();

    // Technically legal, but no-op
    if (!depthMode && !stencilMode)
      return;

    // Subsequent functions expect stencil mode to be None
    // if either of the images have no stencil aspect
    if (!(region.dstSubresource.aspectMask
        & region.srcSubresource.aspectMask
        & VK_IMAGE_ASPECT_STENCIL_BIT))
      stencilMode = VK_RESOLVE_MODE_NONE_KHR;

    // We can only use the depth-stencil resolve path if the
    // extension is supported, if we are resolving a full
    // subresource, and both images have the same format.
    bool useFb = !m_device->extensions().khrDepthStencilResolve
              || !dstImage->isFullSubresource(region.dstSubresource, region.extent)
              || !srcImage->isFullSubresource(region.srcSubresource, region.extent)
              || dstImage->info().format != srcImage->info().format;
    
    if (useFb) {
      // Additionally, the given mode combination must be supported.
      const auto& properties = m_device->properties().khrDepthStencilResolve;

      useFb |= (properties.supportedDepthResolveModes   & depthMode)   != depthMode
            || (properties.supportedStencilResolveModes & stencilMode) != stencilMode;
      
      if (depthMode != stencilMode) {
        useFb |= (!depthMode || !stencilMode)
          ? !properties.independentResolveNone
          : !properties.independentResolve;
      }
    }

    if (useFb) {
      this->resolveImageFb(
        dstImage, srcImage, region, VK_FORMAT_UNDEFINED,
        depthMode, stencilMode);
    } else {
      this->resolveImageDs(
        dstImage, srcImage, region,
        depthMode, stencilMode);
    }
  }


  void DxvkContext::transformImage(
    const Rc<DxvkImage>&            dstImage,
    const VkImageSubresourceRange&  dstSubresources,
          VkImageLayout             srcLayout,
          VkImageLayout             dstLayout) {
    this->spillRenderPass();
    
    if (srcLayout != dstLayout) {
      m_execBarriers.recordCommands(m_cmd);

      m_execBarriers.accessImage(
        dstImage, dstSubresources,
        srcLayout,
        dstImage->info().stages,
        dstImage->info().access,
        dstLayout,
        dstImage->info().stages,
        dstImage->info().access);
      
      m_cmd->trackResource(dstImage);
    }
  }
  
  
  void DxvkContext::updateBuffer(
    const Rc<DxvkBuffer>&           buffer,
          VkDeviceSize              offset,
          VkDeviceSize              size,
    const void*                     data) {
    bool replaceBuffer = (size == buffer->info().size)
                      && (size <= (1 << 20)) /* 1 MB */
                      && (m_flags.test(DxvkContextFlag::GpRenderPassBound));
    
    DxvkBufferSliceHandle bufferSlice;
    DxvkCmdBuffer         cmdBuffer;

    if (replaceBuffer) {
      // As an optimization, allocate a free slice and perform
      // the copy in the initialization command buffer instead
      // interrupting the render pass and stalling the pipeline.
      bufferSlice = buffer->allocSlice();
      cmdBuffer   = DxvkCmdBuffer::InitBuffer;

      this->invalidateBuffer(buffer, bufferSlice);
    } else {
      this->spillRenderPass();
    
      bufferSlice = buffer->getSliceHandle(offset, size);
      cmdBuffer   = DxvkCmdBuffer::ExecBuffer;

      if (m_execBarriers.isBufferDirty(bufferSlice, DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
    }

    // Vulkan specifies that small amounts of data (up to 64kB) can
    // be copied to a buffer directly if the size is a multiple of
    // four. Anything else must be copied through a staging buffer.
    // We'll limit the size to 4kB in order to keep command buffers
    // reasonably small, we do not know how much data apps may upload.
    if ((size <= 4096) && ((size & 0x3) == 0) && ((offset & 0x3) == 0)) {
      m_cmd->cmdUpdateBuffer(
        cmdBuffer,
        bufferSlice.handle,
        bufferSlice.offset,
        bufferSlice.length,
        data);
    } else {
      auto stagingSlice  = m_staging.alloc(CACHE_LINE_SIZE, size);
      auto stagingHandle = stagingSlice.getSliceHandle();

      std::memcpy(stagingHandle.mapPtr, data, size);

      VkBufferCopy region;
      region.srcOffset = stagingHandle.offset;
      region.dstOffset = bufferSlice.offset;
      region.size      = size;

      m_cmd->cmdCopyBuffer(cmdBuffer,
        stagingHandle.handle, bufferSlice.handle, 1, &region);
      
      m_cmd->trackResource(stagingSlice.buffer());
    }

    auto& barriers = replaceBuffer
      ? m_initBarriers
      : m_execBarriers;

    barriers.accessBuffer(
      bufferSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      buffer->info().stages,
      buffer->info().access);

    m_cmd->trackResource(buffer);
  }
  
  
  void DxvkContext::updateImage(
    const Rc<DxvkImage>&            image,
    const VkImageSubresourceLayers& subresources,
          VkOffset3D                imageOffset,
          VkExtent3D                imageExtent,
    const void*                     data,
          VkDeviceSize              pitchPerRow,
          VkDeviceSize              pitchPerLayer) {
    this->spillRenderPass();
    
    // Upload data through a staging buffer. Special care needs to
    // be taken when dealing with compressed image formats: Rather
    // than copying pixels, we'll be copying blocks of pixels.
    const DxvkFormatInfo* formatInfo = image->formatInfo();
    
    // Align image extent to a full block. This is necessary in
    // case the image size is not a multiple of the block size.
    VkExtent3D elementCount = util::computeBlockCount(
      imageExtent, formatInfo->blockSize);
    elementCount.depth *= subresources.layerCount;
    
    // Allocate staging buffer memory for the image data. The
    // pixels or blocks will be tightly packed within the buffer.
    auto stagingSlice = m_staging.alloc(CACHE_LINE_SIZE,
      formatInfo->elementSize * util::flattenImageExtent(elementCount));
    auto stagingHandle = stagingSlice.getSliceHandle();
    
    util::packImageData(stagingHandle.mapPtr, data,
      elementCount, formatInfo->elementSize,
      pitchPerRow, pitchPerLayer);
    
    // Prepare the image layout. If the given extent covers
    // the entire image, we may discard its previous contents.
    auto subresourceRange = vk::makeSubresourceRange(subresources);
    subresourceRange.aspectMask = formatInfo->aspectMask;

    if (m_execBarriers.isImageDirty(image, subresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // Initialize the image if the entire subresource is covered
    VkImageLayout imageLayoutInitial  = image->info().layout;
    VkImageLayout imageLayoutTransfer = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (image->isFullSubresource(subresources, imageExtent))
      imageLayoutInitial = VK_IMAGE_LAYOUT_UNDEFINED;

    m_execAcquires.accessImage(
      image, subresourceRange,
      imageLayoutInitial, 0, 0,
      imageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_execAcquires.recordCommands(m_cmd);
    
    // Copy contents of the staging buffer into the image.
    // Since our source data is tightly packed, we do not
    // need to specify any strides.
    VkBufferImageCopy region;
    region.bufferOffset       = stagingHandle.offset;
    region.bufferRowLength    = 0;
    region.bufferImageHeight  = 0;
    region.imageSubresource   = subresources;
    region.imageOffset        = imageOffset;
    region.imageExtent        = imageExtent;
    
    m_cmd->cmdCopyBufferToImage(DxvkCmdBuffer::ExecBuffer,
      stagingHandle.handle, image->handle(),
      imageLayoutTransfer, 1, &region);
    
    // Transition image back into its optimal layout
    m_execBarriers.accessImage(
      image, subresourceRange,
      imageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      image->info().layout,
      image->info().stages,
      image->info().access);
    
    m_cmd->trackResource(image);
    m_cmd->trackResource(stagingSlice.buffer());
  }
  
  
  void DxvkContext::updateDepthStencilImage(
    const Rc<DxvkImage>&            image,
    const VkImageSubresourceLayers& subresources,
          VkOffset2D                imageOffset,
          VkExtent2D                imageExtent,
    const void*                     data,
          VkDeviceSize              pitchPerRow,
          VkDeviceSize              pitchPerLayer,
          VkFormat                  format) {
    auto formatInfo = imageFormatInfo(format);
    
    VkExtent3D extent3D;
    extent3D.width  = imageExtent.width;
    extent3D.height = imageExtent.height;
    extent3D.depth  = subresources.layerCount;

    VkDeviceSize pixelCount = extent3D.width * extent3D.height * extent3D.depth;

    DxvkBufferCreateInfo tmpBufferInfo;
    tmpBufferInfo.size      = pixelCount * formatInfo->elementSize;
    tmpBufferInfo.usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    tmpBufferInfo.stages    = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    tmpBufferInfo.access    = VK_ACCESS_SHADER_READ_BIT;

    auto tmpBuffer = m_device->createBuffer(tmpBufferInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    util::packImageData(tmpBuffer->mapPtr(0), data,
      extent3D, formatInfo->elementSize,
      pitchPerRow, pitchPerLayer);
    
    copyPackedBufferToDepthStencilImage(
      image, subresources, imageOffset, imageExtent,
      tmpBuffer, 0, format);
  }


  void DxvkContext::uploadBuffer(
    const Rc<DxvkBuffer>&           buffer,
    const void*                     data) {
    auto bufferSlice = buffer->getSliceHandle();

    auto stagingSlice = m_staging.alloc(CACHE_LINE_SIZE, bufferSlice.length);
    auto stagingHandle = stagingSlice.getSliceHandle();
    std::memcpy(stagingHandle.mapPtr, data, bufferSlice.length);

    VkBufferCopy region;
    region.srcOffset = stagingHandle.offset;
    region.dstOffset = bufferSlice.offset;
    region.size      = bufferSlice.length;

    m_cmd->cmdCopyBuffer(DxvkCmdBuffer::SdmaBuffer,
      stagingHandle.handle, bufferSlice.handle, 1, &region);

    m_sdmaBarriers.releaseBuffer(
      m_initBarriers, bufferSlice,
      m_device->queues().transfer.queueFamily,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      m_device->queues().graphics.queueFamily,
      buffer->info().stages,
      buffer->info().access);
    
    m_cmd->trackResource(stagingSlice.buffer());
    m_cmd->trackResource(buffer);
  }


  void DxvkContext::uploadImage(
    const Rc<DxvkImage>&            image,
    const VkImageSubresourceLayers& subresources,
    const void*                     data,
          VkDeviceSize              pitchPerRow,
          VkDeviceSize              pitchPerLayer) {
    const DxvkFormatInfo* formatInfo = image->formatInfo();

    VkOffset3D imageOffset = { 0, 0, 0 };
    VkExtent3D imageExtent = image->mipLevelExtent(subresources.mipLevel);
    
    // Allocate staging buffer slice and copy data to it
    VkExtent3D elementCount = util::computeBlockCount(
      imageExtent, formatInfo->blockSize);
    elementCount.depth *= subresources.layerCount;
    
    auto stagingSlice = m_staging.alloc(CACHE_LINE_SIZE,
      formatInfo->elementSize * util::flattenImageExtent(elementCount));
    auto stagingHandle = stagingSlice.getSliceHandle();
    
    util::packImageData(stagingHandle.mapPtr, data,
      elementCount, formatInfo->elementSize,
      pitchPerRow, pitchPerLayer);

    // Discard previous subresource contents
    m_sdmaAcquires.accessImage(image,
      vk::makeSubresourceRange(subresources),
      VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
      image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_sdmaAcquires.recordCommands(m_cmd);
    
    // Perform copy on the transfer queue
    VkBufferImageCopy region;
    region.bufferOffset       = stagingHandle.offset;
    region.bufferRowLength    = 0;
    region.bufferImageHeight  = 0;
    region.imageSubresource   = subresources;
    region.imageOffset        = imageOffset;
    region.imageExtent        = imageExtent;
    
    m_cmd->cmdCopyBufferToImage(DxvkCmdBuffer::SdmaBuffer,
      stagingHandle.handle, image->handle(),
      image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      1, &region);
    
    // Transfer ownership to graphics queue
    m_sdmaBarriers.releaseImage(m_initBarriers,
      image, vk::makeSubresourceRange(subresources),
      m_device->queues().transfer.queueFamily,
      image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      m_device->queues().graphics.queueFamily,
      image->info().layout,
      image->info().stages,
      image->info().access);
    
    m_cmd->trackResource(image);
    m_cmd->trackResource(stagingSlice.buffer());
  }


  void DxvkContext::setViewports(
          uint32_t            viewportCount,
    const VkViewport*         viewports,
    const VkRect2D*           scissorRects) {
    if (m_state.gp.state.rsViewportCount != viewportCount) {
      m_state.gp.state.rsViewportCount = viewportCount;
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
    
    for (uint32_t i = 0; i < viewportCount; i++) {
      m_state.vp.viewports[i]    = viewports[i];
      m_state.vp.scissorRects[i] = scissorRects[i];
      
      // Vulkan viewports are not allowed to have a width or
      // height of zero, so we fall back to a dummy viewport
      // and instead set an empty scissor rect, which is legal.
      if (viewports[i].width == 0.0f || viewports[i].height == 0.0f) {
        m_state.vp.viewports[i] = VkViewport {
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
        m_state.vp.scissorRects[i] = VkRect2D {
          VkOffset2D { 0, 0 },
          VkExtent2D { 0, 0 } };
      }
    }
    
    m_flags.set(DxvkContextFlag::GpDirtyViewport);
  }
  
  
  void DxvkContext::setBlendConstants(
          DxvkBlendConstants  blendConstants) {
    if (m_state.dyn.blendConstants != blendConstants) {
      m_state.dyn.blendConstants = blendConstants;
      m_flags.set(DxvkContextFlag::GpDirtyBlendConstants);
    }
  }
  
  
  void DxvkContext::setDepthBias(
          DxvkDepthBias       depthBias) {
    if (m_state.dyn.depthBias != depthBias) {
      m_state.dyn.depthBias = depthBias;
      m_flags.set(DxvkContextFlag::GpDirtyDepthBias);
    }
  }


  void DxvkContext::setDepthBounds(
          DxvkDepthBounds     depthBounds) {
    if (m_state.dyn.depthBounds != depthBounds) {
      m_state.dyn.depthBounds = depthBounds;
      m_flags.set(DxvkContextFlag::GpDirtyDepthBounds);
    }

    if (m_state.gp.state.dsEnableDepthBoundsTest != depthBounds.enableDepthBounds) {
      m_state.gp.state.dsEnableDepthBoundsTest = depthBounds.enableDepthBounds;
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
  }
  
  
  void DxvkContext::setStencilReference(
          uint32_t            reference) {
    if (m_state.dyn.stencilReference != reference) {
      m_state.dyn.stencilReference = reference;
      m_flags.set(DxvkContextFlag::GpDirtyStencilRef);
    }
  }
  
  
  void DxvkContext::setInputAssemblyState(const DxvkInputAssemblyState& ia) {
    m_state.gp.state.iaPrimitiveTopology = ia.primitiveTopology;
    m_state.gp.state.iaPrimitiveRestart  = ia.primitiveRestart;
    m_state.gp.state.iaPatchVertexCount  = ia.patchVertexCount;
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setInputLayout(
          uint32_t             attributeCount,
    const DxvkVertexAttribute* attributes,
          uint32_t             bindingCount,
    const DxvkVertexBinding*   bindings) {
    m_flags.set(
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyVertexBuffers);
    
    for (uint32_t i = 0; i < attributeCount; i++) {
      m_state.gp.state.ilAttributes[i].location = attributes[i].location;
      m_state.gp.state.ilAttributes[i].binding  = attributes[i].binding;
      m_state.gp.state.ilAttributes[i].format   = attributes[i].format;
      m_state.gp.state.ilAttributes[i].offset   = attributes[i].offset;
    }
    
    for (uint32_t i = attributeCount; i < m_state.gp.state.ilAttributeCount; i++)
      m_state.gp.state.ilAttributes[i] = VkVertexInputAttributeDescription();
    
    for (uint32_t i = 0; i < bindingCount; i++) {
      m_state.gp.state.ilBindings[i].binding    = bindings[i].binding;
      m_state.gp.state.ilBindings[i].inputRate  = bindings[i].inputRate;
      m_state.gp.state.ilDivisors[i]            = bindings[i].fetchRate;
    }
    
    for (uint32_t i = bindingCount; i < m_state.gp.state.ilBindingCount; i++) {
      m_state.gp.state.ilBindings[i] = VkVertexInputBindingDescription();
      m_state.gp.state.ilDivisors[i] = 0;
    }
    
    m_state.gp.state.ilAttributeCount = attributeCount;
    m_state.gp.state.ilBindingCount   = bindingCount;
  }
  
  
  void DxvkContext::setRasterizerState(const DxvkRasterizerState& rs) {
    m_state.gp.state.rsDepthClipEnable   = rs.depthClipEnable;
    m_state.gp.state.rsDepthBiasEnable   = rs.depthBiasEnable;
    m_state.gp.state.rsPolygonMode       = rs.polygonMode;
    m_state.gp.state.rsCullMode          = rs.cullMode;
    m_state.gp.state.rsFrontFace         = rs.frontFace;
    m_state.gp.state.rsSampleCount       = rs.sampleCount;

    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setMultisampleState(const DxvkMultisampleState& ms) {
    m_state.gp.state.msSampleMask            = ms.sampleMask;
    m_state.gp.state.msEnableAlphaToCoverage = ms.enableAlphaToCoverage;
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setDepthStencilState(const DxvkDepthStencilState& ds) {
    m_state.gp.state.dsEnableDepthTest   = ds.enableDepthTest;
    m_state.gp.state.dsEnableDepthWrite  = ds.enableDepthWrite;
    m_state.gp.state.dsEnableStencilTest = ds.enableStencilTest;
    m_state.gp.state.dsDepthCompareOp    = ds.depthCompareOp;
    m_state.gp.state.dsStencilOpFront    = ds.stencilOpFront;
    m_state.gp.state.dsStencilOpBack     = ds.stencilOpBack;
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setLogicOpState(const DxvkLogicOpState& lo) {
    m_state.gp.state.omEnableLogicOp = lo.enableLogicOp;
    m_state.gp.state.omLogicOp       = lo.logicOp;
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setBlendMode(
          uint32_t            attachment,
    const DxvkBlendMode&      blendMode) {
    m_state.gp.state.omBlendAttachments[attachment].blendEnable         = blendMode.enableBlending;
    m_state.gp.state.omBlendAttachments[attachment].srcColorBlendFactor = blendMode.colorSrcFactor;
    m_state.gp.state.omBlendAttachments[attachment].dstColorBlendFactor = blendMode.colorDstFactor;
    m_state.gp.state.omBlendAttachments[attachment].colorBlendOp        = blendMode.colorBlendOp;
    m_state.gp.state.omBlendAttachments[attachment].srcAlphaBlendFactor = blendMode.alphaSrcFactor;
    m_state.gp.state.omBlendAttachments[attachment].dstAlphaBlendFactor = blendMode.alphaDstFactor;
    m_state.gp.state.omBlendAttachments[attachment].alphaBlendOp        = blendMode.alphaBlendOp;
    m_state.gp.state.omBlendAttachments[attachment].colorWriteMask      = blendMode.writeMask;
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }


  void DxvkContext::setSpecConstant(
          uint32_t            index,
          uint32_t            value) {
    if (m_state.gp.state.scSpecConstants[index] != value) {
      m_state.gp.state.scSpecConstants[index] = value;
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
  }
  
  
  void DxvkContext::setPredicate(
    const DxvkBufferSlice&    predicate,
          VkConditionalRenderingFlagsEXT flags) {
    if (!m_state.cond.predicate.matches(predicate)) {
      m_state.cond.predicate = predicate;

      if (m_predicateWrites.find(predicate.getSliceHandle())
       != m_predicateWrites.end()) {
        spillRenderPass();
        commitPredicateUpdates();
      }

      m_flags.set(DxvkContextFlag::GpDirtyPredicate);
    }

    if (m_state.cond.flags != flags) {
      m_state.cond.flags = flags;
      m_flags.set(DxvkContextFlag::GpDirtyPredicate);
    }
  }


  void DxvkContext::setBarrierControl(DxvkBarrierControlFlags control) {
    m_barrierControl = control;
  }
  
  
  void DxvkContext::signalGpuEvent(const Rc<DxvkGpuEvent>& event) {
    this->spillRenderPass();
    
    DxvkGpuEventHandle handle = m_common->eventPool().allocEvent();

    m_cmd->cmdSetEvent(handle.event,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    m_cmd->trackGpuEvent(event->reset(handle));
    m_cmd->trackResource(event);
  }
  
  
  void DxvkContext::writePredicate(
    const DxvkBufferSlice&    predicate,
    const Rc<DxvkGpuQuery>&   query) {
    DxvkBufferSliceHandle predicateHandle = predicate.getSliceHandle();
    DxvkGpuQueryHandle    queryHandle     = query->handle();

    if (m_flags.test(DxvkContextFlag::GpRenderPassBound))
      m_predicateWrites.insert({ predicateHandle, queryHandle });
    else
      updatePredicate(predicateHandle, queryHandle);

    m_cmd->trackResource(predicate.buffer());
  }


  void DxvkContext::writeTimestamp(const Rc<DxvkGpuQuery>& query) {
    m_queryManager.writeTimestamp(m_cmd, query);
  }


  void DxvkContext::queueSignal(const Rc<sync::Signal>& signal) {
    m_cmd->queueSignal(signal);
  }


  void DxvkContext::trimStagingBuffers() {
    m_staging.trim();
  }
  
  
  void DxvkContext::clearImageViewFb(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkImageAspectFlags    aspect,
          VkClearValue          value) {
    this->updateFramebuffer();

    // Find out if the render target view is currently bound,
    // so that we can avoid spilling the render pass if it is.
    int32_t attachmentIndex = -1;
    
    if (m_state.om.framebuffer != nullptr
     && m_state.om.framebuffer->isFullSize(imageView))
      attachmentIndex = m_state.om.framebuffer->findAttachment(imageView);

    if (attachmentIndex < 0) {
      this->spillRenderPass();

      if (m_execBarriers.isImageDirty(
          imageView->image(),
          imageView->imageSubresources(),
          DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
      
      // Set up a temporary framebuffer
      DxvkRenderTargets attachments;
      DxvkRenderPassOps ops;

      VkPipelineStageFlags clearStages = 0;
      VkAccessFlags        clearAccess = 0;
      
      if (imageView->info().aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        clearStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        clearAccess |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        attachments.color[0].view   = imageView;
        attachments.color[0].layout = imageView->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        ops.colorOps[0].loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        ops.colorOps[0].loadLayout  = imageView->imageInfo().layout;
        ops.colorOps[0].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        ops.colorOps[0].storeLayout = imageView->imageInfo().layout;
      } else {
        clearStages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                    |  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        clearAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        attachments.depth.view   = imageView;
        attachments.depth.layout = imageView->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        ops.depthOps.loadOpD     = VK_ATTACHMENT_LOAD_OP_LOAD;
        ops.depthOps.loadOpS     = VK_ATTACHMENT_LOAD_OP_LOAD;
        ops.depthOps.loadLayout  = imageView->imageInfo().layout;
        ops.depthOps.storeOpD    = VK_ATTACHMENT_STORE_OP_STORE;
        ops.depthOps.storeOpS    = VK_ATTACHMENT_STORE_OP_STORE;
        ops.depthOps.storeLayout = imageView->imageInfo().layout;
      }

      // We cannot leverage render pass clears
      // because we clear only part of the view
      this->renderPassBindFramebuffer(
        m_device->createFramebuffer(attachments),
        ops, 0, nullptr);
      
      m_execBarriers.accessImage(
        imageView->image(),
        imageView->imageSubresources(),
        imageView->imageInfo().layout,
        clearStages, clearAccess,
        imageView->imageInfo().layout,
        imageView->imageInfo().stages,
        imageView->imageInfo().access);
    } else {
      // Make sure the render pass is active so
      // that we can actually perform the clear
      this->startRenderPass();
    }

    // Perform the actual clear operation
    VkClearAttachment clearInfo;
    clearInfo.aspectMask          = aspect;
    clearInfo.colorAttachment     = attachmentIndex;
    clearInfo.clearValue          = value;

    if (attachmentIndex < 0)
      clearInfo.colorAttachment   = 0;

    VkClearRect clearRect;
    clearRect.rect.offset.x       = offset.x;
    clearRect.rect.offset.y       = offset.y;
    clearRect.rect.extent.width   = extent.width;
    clearRect.rect.extent.height  = extent.height;
    clearRect.baseArrayLayer      = 0;
    clearRect.layerCount          = imageView->info().numLayers;

    m_cmd->cmdClearAttachments(1, &clearInfo, 1, &clearRect);

    // Unbind temporary framebuffer
    if (attachmentIndex < 0)
      this->renderPassUnbindFramebuffer();
  }

  
  void DxvkContext::clearImageViewCs(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkClearValue          value) {
    this->spillRenderPass();
    this->unbindComputePipeline();
    
    if (m_execBarriers.isImageDirty(
          imageView->image(),
          imageView->imageSubresources(),
          DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // Query pipeline objects to use for this clear operation
    DxvkMetaClearPipeline pipeInfo = m_common->metaClear().getClearImagePipeline(
      imageView->type(), imageFormatInfo(imageView->info().format)->flags);
    
    // Create a descriptor set pointing to the view
    VkDescriptorSet descriptorSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    
    VkDescriptorImageInfo viewInfo;
    viewInfo.sampler      = VK_NULL_HANDLE;
    viewInfo.imageView    = imageView->handle();
    viewInfo.imageLayout  = imageView->imageInfo().layout;
    
    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstSet           = descriptorSet;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrite.pImageInfo       = &viewInfo;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    m_cmd->updateDescriptorSets(1, &descriptorWrite);
    
    // Prepare shader arguments
    DxvkMetaClearArgs pushArgs;
    pushArgs.clearValue = value.color;
    pushArgs.offset = offset;
    pushArgs.extent = extent;
    
    VkExtent3D workgroups = util::computeBlockCount(
      pushArgs.extent, pipeInfo.workgroupSize);
    
    if (imageView->type() == VK_IMAGE_VIEW_TYPE_1D_ARRAY)
      workgroups.height = imageView->subresources().layerCount;
    else if (imageView->type() == VK_IMAGE_VIEW_TYPE_2D_ARRAY)
      workgroups.depth = imageView->subresources().layerCount;
    
    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeline);
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, descriptorSet,
      0, nullptr);
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(pushArgs), &pushArgs);
    m_cmd->cmdDispatch(
      workgroups.width,
      workgroups.height,
      workgroups.depth);
    
    m_execBarriers.accessImage(
      imageView->image(),
      imageView->imageSubresources(),
      imageView->imageInfo().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      imageView->imageInfo().layout,
      imageView->imageInfo().stages,
      imageView->imageInfo().access);
    
    m_cmd->trackResource(imageView);
    m_cmd->trackResource(imageView->image());
  }

  
  void DxvkContext::copyImageHw(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    auto dstSubresourceRange = vk::makeSubresourceRange(dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(srcSubresource);
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    VkImageLayout dstImageLayout = dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageLayout srcImageLayout = srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImageLayout dstInitImageLayout = dstImage->info().layout;

    if (dstImage->isFullSubresource(dstSubresource, extent))
      dstInitImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    m_execAcquires.accessImage(
      dstImage, dstSubresourceRange,
      dstInitImageLayout, 0, 0,
      dstImageLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_execAcquires.accessImage(
      srcImage, srcSubresourceRange,
      srcImage->info().layout, 0, 0,
      srcImageLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);

    m_execAcquires.recordCommands(m_cmd);
    
    VkImageCopy imageRegion;
    imageRegion.srcSubresource = srcSubresource;
    imageRegion.srcOffset      = srcOffset;
    imageRegion.dstSubresource = dstSubresource;
    imageRegion.dstOffset      = dstOffset;
    imageRegion.extent         = extent;
    
    m_cmd->cmdCopyImage(DxvkCmdBuffer::ExecBuffer,
      srcImage->handle(), srcImageLayout,
      dstImage->handle(), dstImageLayout,
      1, &imageRegion);
    
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImageLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange,
      srcImageLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_cmd->trackResource(dstImage);
    m_cmd->trackResource(srcImage);
  }

  
  void DxvkContext::copyImageFb(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    auto dstSubresourceRange = vk::makeSubresourceRange(dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(srcSubresource);
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // Source image needs to be readable
    if (!(srcImage->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
      Logger::err("DxvkContext: copyImageFb: Source image not readable");
      return;
    }

    // Render target format to use for this copy
    VkFormat viewFormat = m_common->metaCopy().getCopyDestinationFormat(
      dstSubresource.aspectMask,
      srcSubresource.aspectMask,
      srcImage->info().format);
    
    if (viewFormat == VK_FORMAT_UNDEFINED) {
      Logger::err("DxvkContext: copyImageFb: Unsupported format");
      return;
    }
    
    // We might have to transition the source image layout
    VkImageLayout srcLayout = (srcSubresource.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)
      ? srcImage->pickLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      : srcImage->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    
    if (srcImage->info().layout != srcLayout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout, 0, 0,
        srcLayout,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      
      m_execAcquires.recordCommands(m_cmd);
    }

    // In some cases, we may be able to render to the destination
    // image directly, which is faster than using a temporary image
    VkImageUsageFlagBits tgtUsage = (dstSubresource.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)
      ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    bool useDirectRender = (dstImage->isViewCompatible(viewFormat))
                        && (dstImage->info().usage & tgtUsage);
    
    // If needed, create a temporary render target for the copy
    Rc<DxvkImage>            tgtImage       = dstImage;
    VkImageSubresourceLayers tgtSubresource = dstSubresource;
    VkOffset3D               tgtOffset      = dstOffset;

    if (!useDirectRender) {
      DxvkImageCreateInfo info;
      info.type           = dstImage->info().type;
      info.format         = viewFormat;
      info.flags          = 0;
      info.sampleCount    = dstImage->info().sampleCount;
      info.extent         = extent;
      info.numLayers      = dstSubresource.layerCount;
      info.mipLevels      = 1;
      info.usage          = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | tgtUsage;
      info.stages         = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access         = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      info.tiling         = VK_IMAGE_TILING_OPTIMAL;
      info.layout         = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

      tgtImage = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      tgtSubresource.mipLevel       = 0;
      tgtSubresource.baseArrayLayer = 0;

      tgtOffset = { 0, 0, 0 };
    }
    
    // Create source and destination image views
    VkImageViewType viewType = dstImage->info().type == VK_IMAGE_TYPE_1D
      ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
      : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    
    DxvkImageViewCreateInfo tgtViewInfo;
    tgtViewInfo.type      = viewType;
    tgtViewInfo.format    = viewFormat;
    tgtViewInfo.usage     = tgtUsage;
    tgtViewInfo.aspect    = tgtSubresource.aspectMask;
    tgtViewInfo.minLevel  = tgtSubresource.mipLevel;
    tgtViewInfo.numLevels = 1;
    tgtViewInfo.minLayer  = tgtSubresource.baseArrayLayer;
    tgtViewInfo.numLayers = tgtSubresource.layerCount;

    DxvkImageViewCreateInfo srcViewInfo;
    srcViewInfo.type      = viewType;
    srcViewInfo.format    = srcImage->info().format;
    srcViewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
    srcViewInfo.aspect    = srcSubresource.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_COLOR_BIT);
    srcViewInfo.minLevel  = srcSubresource.mipLevel;
    srcViewInfo.numLevels = 1;
    srcViewInfo.minLayer  = srcSubresource.baseArrayLayer;
    srcViewInfo.numLayers = srcSubresource.layerCount;

    Rc<DxvkImageView> tgtImageView = m_device->createImageView(tgtImage, tgtViewInfo);
    Rc<DxvkImageView> srcImageView = m_device->createImageView(srcImage, srcViewInfo);
    Rc<DxvkImageView> srcStencilView;

    if (srcSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
      srcViewInfo.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
      srcStencilView = m_device->createImageView(srcImage, srcViewInfo);
    }

    // Create framebuffer and pipeline for the copy
    Rc<DxvkMetaCopyRenderPass> fb = new DxvkMetaCopyRenderPass(
      m_device->vkd(), tgtImageView, srcImageView, srcStencilView,
      tgtImage->isFullSubresource(tgtSubresource, extent));
    
    auto pipeInfo = m_common->metaCopy().getPipeline(
      viewType, viewFormat, tgtImage->info().sampleCount);
    
    VkDescriptorImageInfo descriptorImage;
    descriptorImage.sampler          = VK_NULL_HANDLE;
    descriptorImage.imageView        = srcImageView->handle();
    descriptorImage.imageLayout      = srcLayout;

    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.pImageInfo       = &descriptorImage;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    
    descriptorWrite.dstSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSets(1, &descriptorWrite);

    if (srcSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
      descriptorImage.imageView  = srcStencilView->handle();
      descriptorWrite.dstBinding = 1;
      m_cmd->updateDescriptorSets(1, &descriptorWrite);
    }
    
    VkViewport viewport;
    viewport.x        = float(tgtOffset.x);
    viewport.y        = float(tgtOffset.y);
    viewport.width    = float(extent.width);
    viewport.height   = float(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor;
    scissor.offset    = { tgtOffset.x, tgtOffset.y };
    scissor.extent    = { extent.width, extent.height };

    VkRenderPassBeginInfo info;
    info.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext              = nullptr;
    info.renderPass         = fb->renderPass();
    info.framebuffer        = fb->framebuffer();
    info.renderArea.offset  = { 0, 0 };
    info.renderArea.extent  = {
      tgtImage->mipLevelExtent(tgtSubresource.mipLevel).width,
      tgtImage->mipLevelExtent(tgtSubresource.mipLevel).height };
    info.clearValueCount    = 0;
    info.pClearValues       = nullptr;

    // Perform the actual copy operation
    m_cmd->cmdBeginRenderPass(&info, VK_SUBPASS_CONTENTS_INLINE);
    m_cmd->cmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeInfo.pipeHandle);
    m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipeInfo.pipeLayout, descriptorWrite.dstSet, 0, nullptr);

    m_cmd->cmdSetViewport(0, 1, &viewport);
    m_cmd->cmdSetScissor (0, 1, &scissor);

    VkOffset2D srcCoordOffset = {
      srcOffset.x - tgtOffset.x,
      srcOffset.y - tgtOffset.y };
    
    m_cmd->cmdPushConstants(pipeInfo.pipeLayout,
      VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(srcCoordOffset),
      &srcCoordOffset);
    
    m_cmd->cmdDraw(3, tgtSubresource.layerCount, 0, 0);
    m_cmd->cmdEndRenderPass();

    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange,
      srcLayout,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImage->info().layout,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT    |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT          |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_cmd->trackResource(tgtImage);
    m_cmd->trackResource(srcImage);
    m_cmd->trackResource(fb);
    
    // If necessary, copy the temporary image
    // to the original destination image
    if (!useDirectRender) {
      this->copyImageHw(
        dstImage, dstSubresource, dstOffset,
        tgtImage, tgtSubresource, tgtOffset,
        extent);
    }
  }


  void DxvkContext::resolveImageHw(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // We only support resolving to the entire image
    // area, so we might as well discard its contents
    VkImageLayout initialLayout = dstImage->info().layout;

    if (dstImage->isFullSubresource(region.dstSubresource, region.extent))
      initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    m_execAcquires.accessImage(
      dstImage, dstSubresourceRange,
      initialLayout, 0, 0,
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_execAcquires.accessImage(
      srcImage, srcSubresourceRange,
      srcImage->info().layout, 0, 0,
      srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);

    m_execAcquires.recordCommands(m_cmd);
    
    m_cmd->cmdResolveImage(
      srcImage->handle(), srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
      dstImage->handle(), dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      1, &region);
  
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange,
      srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_cmd->trackResource(dstImage);
    m_cmd->trackResource(srcImage);
  }


  void DxvkContext::resolveImageDs(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkResolveModeFlagBitsKHR  depthMode,
          VkResolveModeFlagBitsKHR  stencilMode) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);

    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // Create image views covering the requested subresourcs
    DxvkImageViewCreateInfo dstViewInfo;
    dstViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dstViewInfo.format    = dstImage->info().format;
    dstViewInfo.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dstViewInfo.aspect    = region.dstSubresource.aspectMask;
    dstViewInfo.minLevel  = region.dstSubresource.mipLevel;
    dstViewInfo.numLevels = 1;
    dstViewInfo.minLayer  = region.dstSubresource.baseArrayLayer;
    dstViewInfo.numLayers = region.dstSubresource.layerCount;

    DxvkImageViewCreateInfo srcViewInfo;
    srcViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    srcViewInfo.format    = srcImage->info().format;
    srcViewInfo.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    srcViewInfo.aspect    = region.srcSubresource.aspectMask;
    srcViewInfo.minLevel  = region.srcSubresource.mipLevel;
    srcViewInfo.numLevels = 1;
    srcViewInfo.minLayer  = region.srcSubresource.baseArrayLayer;
    srcViewInfo.numLayers = region.srcSubresource.layerCount;

    Rc<DxvkImageView> dstImageView = m_device->createImageView(dstImage, dstViewInfo);
    Rc<DxvkImageView> srcImageView = m_device->createImageView(srcImage, srcViewInfo);

    // Create a framebuffer for the resolve op
    VkExtent3D passExtent = dstImageView->mipLevelExtent(0);

    Rc<DxvkMetaResolveRenderPass> fb = new DxvkMetaResolveRenderPass(
      m_device->vkd(), dstImageView, srcImageView, depthMode, stencilMode);

    VkRenderPassBeginInfo info;
    info.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext              = nullptr;
    info.renderPass         = fb->renderPass();
    info.framebuffer        = fb->framebuffer();
    info.renderArea.offset  = { 0, 0 };
    info.renderArea.extent  = { passExtent.width, passExtent.height };
    info.clearValueCount    = 0;
    info.pClearValues       = nullptr;

    m_cmd->cmdBeginRenderPass(&info, VK_SUBPASS_CONTENTS_INLINE);
    m_cmd->cmdEndRenderPass();

    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImage->info().layout,
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange,
      srcImage->info().layout,
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);

    m_cmd->trackResource(fb);
    m_cmd->trackResource(dstImage);
    m_cmd->trackResource(srcImage);
  }


  void DxvkContext::resolveImageFb(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkFormat                  format,
          VkResolveModeFlagBitsKHR  depthMode,
          VkResolveModeFlagBitsKHR  stencilMode) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // We might have to transition the source image layout
    VkImageLayout srcLayout = srcImage->pickLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    if (srcImage->info().layout != srcLayout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout, 0, 0,
        srcLayout,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      
      m_execAcquires.recordCommands(m_cmd);
    }

    // Create image views covering the requested subresourcs
    DxvkImageViewCreateInfo dstViewInfo;
    dstViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dstViewInfo.format    = format ? format : dstImage->info().format;
    dstViewInfo.usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    dstViewInfo.aspect    = region.dstSubresource.aspectMask;
    dstViewInfo.minLevel  = region.dstSubresource.mipLevel;
    dstViewInfo.numLevels = 1;
    dstViewInfo.minLayer  = region.dstSubresource.baseArrayLayer;
    dstViewInfo.numLayers = region.dstSubresource.layerCount;

    if (region.dstSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
      dstViewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    DxvkImageViewCreateInfo srcViewInfo;
    srcViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    srcViewInfo.format    = format ? format : srcImage->info().format;
    srcViewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
    srcViewInfo.aspect    = region.srcSubresource.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_COLOR_BIT);
    srcViewInfo.minLevel  = region.srcSubresource.mipLevel;
    srcViewInfo.numLevels = 1;
    srcViewInfo.minLayer  = region.srcSubresource.baseArrayLayer;
    srcViewInfo.numLayers = region.srcSubresource.layerCount;

    Rc<DxvkImageView> dstImageView = m_device->createImageView(dstImage, dstViewInfo);
    Rc<DxvkImageView> srcImageView = m_device->createImageView(srcImage, srcViewInfo);
    Rc<DxvkImageView> srcStencilView = nullptr;

    if ((region.dstSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && stencilMode != VK_RESOLVE_MODE_NONE_KHR) {
      srcViewInfo.aspect  = VK_IMAGE_ASPECT_STENCIL_BIT;
      srcStencilView = m_device->createImageView(srcImage, srcViewInfo);
    }

    // Create a framebuffer and pipeline for the resolve op
    VkExtent3D passExtent = dstImageView->mipLevelExtent(0);

    Rc<DxvkMetaResolveRenderPass> fb = new DxvkMetaResolveRenderPass(
      m_device->vkd(), dstImageView, srcImageView, srcStencilView,
      dstImage->isFullSubresource(region.dstSubresource, region.extent));

    auto pipeInfo = m_common->metaResolve().getPipeline(
      dstViewInfo.format, srcImage->info().sampleCount, depthMode, stencilMode);
    
    VkDescriptorImageInfo descriptorImage;
    descriptorImage.sampler          = VK_NULL_HANDLE;
    descriptorImage.imageView        = srcImageView->handle();
    descriptorImage.imageLayout      = srcLayout;

    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.pImageInfo       = &descriptorImage;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    
    descriptorWrite.dstSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSets(1, &descriptorWrite);

    if (srcStencilView != nullptr) {
      descriptorWrite.dstBinding     = 1;
      descriptorImage.imageView      = srcStencilView->handle();
      m_cmd->updateDescriptorSets(1, &descriptorWrite);
    }

    VkViewport viewport;
    viewport.x        = float(region.dstOffset.x);
    viewport.y        = float(region.dstOffset.y);
    viewport.width    = float(region.extent.width);
    viewport.height   = float(region.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor;
    scissor.offset    = { region.dstOffset.x,  region.dstOffset.y   };
    scissor.extent    = { region.extent.width, region.extent.height };

    VkRenderPassBeginInfo info;
    info.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext              = nullptr;
    info.renderPass         = fb->renderPass();
    info.framebuffer        = fb->framebuffer();
    info.renderArea.offset  = { 0, 0 };
    info.renderArea.extent  = { passExtent.width, passExtent.height };
    info.clearValueCount    = 0;
    info.pClearValues       = nullptr;
    
    // Perform the actual resolve operation
    VkOffset2D srcOffset = {
      region.srcOffset.x,
      region.srcOffset.y };
    
    m_cmd->cmdBeginRenderPass(&info, VK_SUBPASS_CONTENTS_INLINE);
    m_cmd->cmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeInfo.pipeHandle);
    m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipeInfo.pipeLayout, descriptorWrite.dstSet, 0, nullptr);
    m_cmd->cmdSetViewport(0, 1, &viewport);
    m_cmd->cmdSetScissor (0, 1, &scissor);
    m_cmd->cmdPushConstants(pipeInfo.pipeLayout,
      VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(srcOffset), &srcOffset);
    m_cmd->cmdDraw(3, region.dstSubresource.layerCount, 0, 0);
    m_cmd->cmdEndRenderPass();

    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImage->info().layout,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange, srcLayout,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_cmd->trackResource(fb);
    m_cmd->trackResource(dstImage);
    m_cmd->trackResource(srcImage);
  }


  void DxvkContext::updatePredicate(
    const DxvkBufferSliceHandle&    predicate,
    const DxvkGpuQueryHandle&       query) {
    m_cmd->cmdCopyQueryPoolResults(
      query.queryPool, query.queryId, 1,
      predicate.handle, predicate.offset, sizeof(uint32_t),
      VK_QUERY_RESULT_WAIT_BIT);
    
    m_execBarriers.accessBuffer(predicate,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT,
      VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT);
  }


  void DxvkContext::commitPredicateUpdates() {
    for (const auto& update : m_predicateWrites)
      updatePredicate(update.first, update.second);
    
    m_predicateWrites.clear();
  }


  void DxvkContext::startRenderPass() {
    if (!m_flags.test(DxvkContextFlag::GpRenderPassBound)
     && (m_state.om.framebuffer != nullptr)) {
      m_flags.set(DxvkContextFlag::GpRenderPassBound);
      m_flags.clr(DxvkContextFlag::GpClearRenderTargets);

      m_execBarriers.recordCommands(m_cmd);

      this->renderPassBindFramebuffer(
        m_state.om.framebuffer,
        m_state.om.renderPassOps,
        m_state.om.clearValues.size(),
        m_state.om.clearValues.data());
      
      // Don't discard image contents if we have
      // to spill the current render pass
      this->resetRenderPassOps(
        m_state.om.renderTargets,
        m_state.om.renderPassOps);
      
      // Begin occlusion queries
      m_queryManager.beginQueries(m_cmd, VK_QUERY_TYPE_OCCLUSION);
      m_queryManager.beginQueries(m_cmd, VK_QUERY_TYPE_PIPELINE_STATISTICS);
    }
  }
  
  
  void DxvkContext::spillRenderPass() {
    if (m_flags.test(DxvkContextFlag::GpClearRenderTargets))
      this->clearRenderPass();
    
    if (m_flags.test(DxvkContextFlag::GpRenderPassBound)) {
      m_flags.clr(DxvkContextFlag::GpRenderPassBound);

      this->pauseTransformFeedback();
      
      m_queryManager.endQueries(m_cmd, VK_QUERY_TYPE_OCCLUSION);
      m_queryManager.endQueries(m_cmd, VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      this->renderPassUnbindFramebuffer();
      this->unbindGraphicsPipeline();
      this->commitPredicateUpdates();

      m_flags.clr(DxvkContextFlag::GpDirtyXfbCounters);
    }
  }


  void DxvkContext::clearRenderPass() {
    if (m_flags.test(DxvkContextFlag::GpClearRenderTargets)) {
      m_flags.clr(DxvkContextFlag::GpClearRenderTargets);

      bool flushBarriers = false;

      for (uint32_t i = 0; i < m_state.om.framebuffer->numAttachments(); i++) {
        const DxvkAttachment& attachment = m_state.om.framebuffer->getAttachment(i);

        flushBarriers |= m_execBarriers.isImageDirty(
          attachment.view->image(),
          attachment.view->imageSubresources(),
          DxvkAccess::Write);
      }

      if (flushBarriers)
        m_execBarriers.recordCommands(m_cmd);

      this->renderPassBindFramebuffer(
        m_state.om.framebuffer,
        m_state.om.renderPassOps,
        m_state.om.clearValues.size(),
        m_state.om.clearValues.data());
      
      this->resetRenderPassOps(
        m_state.om.renderTargets,
        m_state.om.renderPassOps);
      
      this->renderPassUnbindFramebuffer();

      for (uint32_t i = 0; i < m_state.om.framebuffer->numAttachments(); i++) {
        const DxvkAttachment& attachment = m_state.om.framebuffer->getAttachment(i);

        m_execBarriers.accessImage(
          attachment.view->image(),
          attachment.view->imageSubresources(),
          attachment.view->imageInfo().layout,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT    |
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT          |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          attachment.view->imageInfo().layout,
          attachment.view->imageInfo().stages,
          attachment.view->imageInfo().access);
      }
    }
  }
  
  
  void DxvkContext::renderPassBindFramebuffer(
    const Rc<DxvkFramebuffer>&  framebuffer,
    const DxvkRenderPassOps&    ops,
          uint32_t              clearValueCount,
    const VkClearValue*         clearValues) {
    const DxvkFramebufferSize fbSize = framebuffer->size();
    
    VkRect2D renderArea;
    renderArea.offset = VkOffset2D { 0, 0 };
    renderArea.extent = VkExtent2D { fbSize.width, fbSize.height };
    
    VkRenderPassBeginInfo info;
    info.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext                = nullptr;
    info.renderPass           = framebuffer->getRenderPassHandle(ops);
    info.framebuffer          = framebuffer->handle();
    info.renderArea           = renderArea;
    info.clearValueCount      = clearValueCount;
    info.pClearValues         = clearValues;
    
    m_cmd->cmdBeginRenderPass(&info,
      VK_SUBPASS_CONTENTS_INLINE);
    
    m_cmd->trackResource(framebuffer);

    for (uint32_t i = 0; i < framebuffer->numAttachments(); i++) {
      m_cmd->trackResource(framebuffer->getAttachment(i).view);
      m_cmd->trackResource(framebuffer->getAttachment(i).view->image());
    }

    m_cmd->addStatCtr(DxvkStatCounter::CmdRenderPassCount, 1);
  }
  
  
  void DxvkContext::renderPassUnbindFramebuffer() {
    m_cmd->cmdEndRenderPass();
  }
  
  
  void DxvkContext::resetRenderPassOps(
    const DxvkRenderTargets&    renderTargets,
          DxvkRenderPassOps&    renderPassOps) {
    VkPipelineStageFlags shaderStages = m_device->getShaderPipelineStages()
                                      & ~VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    
    renderPassOps.barrier.srcStages = shaderStages
                                    | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT
                                    | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
                                    | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                    | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                    | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                    | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    renderPassOps.barrier.srcAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                    | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                    | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                    | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                    | VK_ACCESS_INDIRECT_COMMAND_READ_BIT
                                    | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                                    | VK_ACCESS_INDEX_READ_BIT
                                    | VK_ACCESS_UNIFORM_READ_BIT
                                    | VK_ACCESS_SHADER_READ_BIT
                                    | VK_ACCESS_SHADER_WRITE_BIT;
    
    if (m_device->features().extTransformFeedback.transformFeedback) {
      renderPassOps.barrier.srcStages |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
      renderPassOps.barrier.srcAccess |= VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT
                                      |  VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT
                                      |  VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
    }

    renderPassOps.barrier.dstStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    renderPassOps.barrier.dstAccess = renderPassOps.barrier.srcAccess
                                    | VK_ACCESS_TRANSFER_READ_BIT
                                    | VK_ACCESS_TRANSFER_WRITE_BIT;

    renderPassOps.depthOps = renderTargets.depth.view != nullptr
      ? DxvkDepthAttachmentOps {
          VK_ATTACHMENT_LOAD_OP_LOAD,
          VK_ATTACHMENT_LOAD_OP_LOAD,
          renderTargets.depth.view->imageInfo().layout,
          VK_ATTACHMENT_STORE_OP_STORE,
          VK_ATTACHMENT_STORE_OP_STORE,
          renderTargets.depth.view->imageInfo().layout }
      : DxvkDepthAttachmentOps { };
    
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      renderPassOps.colorOps[i] = renderTargets.color[i].view != nullptr
        ? DxvkColorAttachmentOps {
            VK_ATTACHMENT_LOAD_OP_LOAD,
            renderTargets.color[i].view->imageInfo().layout,
            VK_ATTACHMENT_STORE_OP_STORE,
            renderTargets.color[i].view->imageInfo().layout }
        : DxvkColorAttachmentOps { };
    }
    
    // TODO provide a sane alternative for this
    if (renderPassOps.colorOps[0].loadLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
      renderPassOps.colorOps[0].loadOp     = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      renderPassOps.colorOps[0].loadLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }
  
  
  void DxvkContext::startConditionalRendering() {
    if (!m_flags.test(DxvkContextFlag::GpCondActive)) {
      m_flags.set(DxvkContextFlag::GpCondActive);

      auto predicateSlice = m_state.cond.predicate.getSliceHandle();

      VkConditionalRenderingBeginInfoEXT info;
      info.sType  = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
      info.pNext  = nullptr;
      info.buffer = predicateSlice.handle;
      info.offset = predicateSlice.offset;
      info.flags  = m_state.cond.flags;

      m_cmd->cmdBeginConditionalRendering(&info);
    }
  }


  void DxvkContext::pauseConditionalRendering() {
    if (m_flags.test(DxvkContextFlag::GpCondActive)) {
      m_flags.clr(DxvkContextFlag::GpCondActive);

      m_cmd->cmdEndConditionalRendering();
    }
  }


  void DxvkContext::startTransformFeedback() {
    if (!m_flags.test(DxvkContextFlag::GpXfbActive)) {
      m_flags.set(DxvkContextFlag::GpXfbActive);

      if (m_flags.test(DxvkContextFlag::GpDirtyXfbCounters)) {
        m_flags.clr(DxvkContextFlag::GpDirtyXfbCounters);

        this->emitMemoryBarrier(
          VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
          VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
          VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, /* XXX */
          VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT);
      }

      VkBuffer     ctrBuffers[MaxNumXfbBuffers];
      VkDeviceSize ctrOffsets[MaxNumXfbBuffers];

      for (uint32_t i = 0; i < MaxNumXfbBuffers; i++) {
        auto physSlice = m_state.xfb.counters[i].getSliceHandle();

        ctrBuffers[i] = physSlice.handle;
        ctrOffsets[i] = physSlice.offset;

        if (physSlice.handle != VK_NULL_HANDLE)
          m_cmd->trackResource(m_state.xfb.counters[i].buffer());
      }
      
      m_cmd->cmdBeginTransformFeedback(
        0, MaxNumXfbBuffers, ctrBuffers, ctrOffsets);
      
      m_queryManager.beginQueries(m_cmd,
        VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT);
    }
  }


  void DxvkContext::pauseTransformFeedback() {
    if (m_flags.test(DxvkContextFlag::GpXfbActive)) {
      m_flags.clr(DxvkContextFlag::GpXfbActive);
      
      VkBuffer     ctrBuffers[MaxNumXfbBuffers];
      VkDeviceSize ctrOffsets[MaxNumXfbBuffers];

      for (uint32_t i = 0; i < MaxNumXfbBuffers; i++) {
        auto physSlice = m_state.xfb.counters[i].getSliceHandle();

        ctrBuffers[i] = physSlice.handle;
        ctrOffsets[i] = physSlice.offset;

        if (physSlice.handle != VK_NULL_HANDLE)
          m_cmd->trackResource(m_state.xfb.counters[i].buffer());
      }

      m_queryManager.endQueries(m_cmd, 
        VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT);
      
      m_cmd->cmdEndTransformFeedback(
        0, MaxNumXfbBuffers, ctrBuffers, ctrOffsets);
      
      m_flags.set(DxvkContextFlag::GpDirtyXfbCounters);
    }
  }


  void DxvkContext::unbindComputePipeline() {
    m_flags.set(
      DxvkContextFlag::CpDirtyPipeline,
      DxvkContextFlag::CpDirtyPipelineState,
      DxvkContextFlag::CpDirtyResources);
    
    m_cpActivePipeline = VK_NULL_HANDLE;
  }
  
  
  void DxvkContext::updateComputePipeline() {
    if (m_flags.test(DxvkContextFlag::CpDirtyPipeline)) {
      m_flags.clr(DxvkContextFlag::CpDirtyPipeline);
      
      m_state.cp.state.bsBindingMask.clear();
      m_state.cp.pipeline = m_common->pipelineManager().createComputePipeline(m_state.cp.shaders);
      
      if (m_state.cp.pipeline != nullptr
       && m_state.cp.pipeline->layout()->pushConstRange().size)
        m_flags.set(DxvkContextFlag::DirtyPushConstants);
    }
  }
  
  
  void DxvkContext::updateComputePipelineState() {
    if (m_flags.test(DxvkContextFlag::CpDirtyPipelineState)) {
      m_flags.clr(DxvkContextFlag::CpDirtyPipelineState);
      
      m_cpActivePipeline = m_state.cp.pipeline != nullptr
        ? m_state.cp.pipeline->getPipelineHandle(m_state.cp.state)
        : VK_NULL_HANDLE;
      
      if (m_cpActivePipeline != VK_NULL_HANDLE) {
        m_cmd->cmdBindPipeline(
          VK_PIPELINE_BIND_POINT_COMPUTE,
          m_cpActivePipeline);
      }
    }
  }
  
  
  void DxvkContext::unbindGraphicsPipeline() {
    m_flags.set(
      DxvkContextFlag::GpDirtyPipeline,
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyResources,
      DxvkContextFlag::GpDirtyVertexBuffers,
      DxvkContextFlag::GpDirtyIndexBuffer,
      DxvkContextFlag::GpDirtyXfbBuffers,
      DxvkContextFlag::GpDirtyBlendConstants,
      DxvkContextFlag::GpDirtyStencilRef,
      DxvkContextFlag::GpDirtyViewport,
      DxvkContextFlag::GpDirtyDepthBias,
      DxvkContextFlag::GpDirtyDepthBounds,
      DxvkContextFlag::GpDirtyPredicate);
    
    m_gpActivePipeline = VK_NULL_HANDLE;
  }
  
  
  void DxvkContext::updateGraphicsPipeline() {
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline)) {
      m_flags.clr(DxvkContextFlag::GpDirtyPipeline);
      
      m_state.gp.state.bsBindingMask.clear();
      m_state.gp.pipeline = m_common->pipelineManager().createGraphicsPipeline(m_state.gp.shaders);
      m_state.gp.flags = DxvkGraphicsPipelineFlags();
      
      if (m_state.gp.pipeline != nullptr) {
        m_state.gp.flags = m_state.gp.pipeline->flags();

        if (m_state.gp.pipeline->layout()->pushConstRange().size)
          m_flags.set(DxvkContextFlag::DirtyPushConstants);
      }
    }
  }
  
  
  void DxvkContext::updateGraphicsPipelineState() {
    if (m_flags.test(DxvkContextFlag::GpDirtyPipelineState)) {
      m_flags.clr(DxvkContextFlag::GpDirtyPipelineState);
      
      this->pauseTransformFeedback();

      // Set up vertex buffer strides for active bindings
      for (uint32_t i = 0; i < m_state.gp.state.ilBindingCount; i++) {
        const uint32_t binding = m_state.gp.state.ilBindings[i].binding;
        m_state.gp.state.ilBindings[i].stride = m_state.vi.vertexStrides[binding];
      }
      
      for (uint32_t i = m_state.gp.state.ilBindingCount; i < MaxNumVertexBindings; i++)
        m_state.gp.state.ilBindings[i].stride = 0;
      
      // Check which dynamic states need to be active. States that
      // are not dynamic will be invalidated in the command buffer.
      m_flags.clr(DxvkContextFlag::GpDynamicBlendConstants,
                  DxvkContextFlag::GpDynamicDepthBias,
                  DxvkContextFlag::GpDynamicDepthBounds,
                  DxvkContextFlag::GpDynamicStencilRef);
      
      m_flags.set(m_state.gp.state.useDynamicBlendConstants()
        ? DxvkContextFlag::GpDynamicBlendConstants
        : DxvkContextFlag::GpDirtyBlendConstants);
      
      m_flags.set(m_state.gp.state.useDynamicDepthBias()
        ? DxvkContextFlag::GpDynamicDepthBias
        : DxvkContextFlag::GpDirtyDepthBias);
      
      m_flags.set(m_state.gp.state.useDynamicDepthBounds()
        ? DxvkContextFlag::GpDynamicDepthBounds
        : DxvkContextFlag::GpDirtyDepthBounds);
      
      m_flags.set(m_state.gp.state.useDynamicStencilRef()
        ? DxvkContextFlag::GpDynamicStencilRef
        : DxvkContextFlag::GpDirtyStencilRef);
      
      // Retrieve and bind actual Vulkan pipeline handle
      m_gpActivePipeline = m_state.gp.pipeline != nullptr && m_state.om.framebuffer != nullptr
        ? m_state.gp.pipeline->getPipelineHandle(m_state.gp.state,
            m_state.om.framebuffer->getRenderPass())
        : VK_NULL_HANDLE;
      
      if (m_gpActivePipeline != VK_NULL_HANDLE) {
        m_cmd->cmdBindPipeline(
          VK_PIPELINE_BIND_POINT_GRAPHICS,
          m_gpActivePipeline);
      }
    }
  }
  
  
  void DxvkContext::updateComputeShaderResources() {
    if (m_state.cp.pipeline == nullptr)
      return;

    if ((m_flags.test(DxvkContextFlag::CpDirtyResources))
     || (m_flags.test(DxvkContextFlag::CpDirtyDescriptorOffsets)
      && m_state.cp.pipeline->layout()->hasStaticBufferBindings())) {
      m_flags.clr(DxvkContextFlag::CpDirtyResources);

      if (this->updateShaderResources<VK_PIPELINE_BIND_POINT_COMPUTE>(m_state.cp.pipeline->layout()))
        m_flags.set(DxvkContextFlag::CpDirtyPipelineState);

      m_flags.set(
        DxvkContextFlag::CpDirtyDescriptorSet,
        DxvkContextFlag::CpDirtyDescriptorOffsets);
    }
  }
  
  
  void DxvkContext::updateComputeShaderDescriptors() {
    if (m_state.cp.pipeline == nullptr)
      return;

    if (m_flags.test(DxvkContextFlag::CpDirtyDescriptorSet)) {
      m_cpSet = this->updateShaderDescriptors(
        m_state.cp.pipeline->layout());
    }

    if (m_flags.test(DxvkContextFlag::CpDirtyDescriptorOffsets)) {
      this->updateShaderDescriptorSetBinding<VK_PIPELINE_BIND_POINT_COMPUTE>(
        m_cpSet, m_state.cp.pipeline->layout());
    }

    m_flags.clr(
      DxvkContextFlag::CpDirtyDescriptorOffsets,
      DxvkContextFlag::CpDirtyDescriptorSet);
  }
  
  
  void DxvkContext::updateGraphicsShaderResources() {
    if (m_state.gp.pipeline == nullptr)
      return;
    
    if ((m_flags.test(DxvkContextFlag::GpDirtyResources))
     || (m_flags.test(DxvkContextFlag::GpDirtyDescriptorOffsets)
      && m_state.gp.pipeline->layout()->hasStaticBufferBindings())) {
      m_flags.clr(DxvkContextFlag::GpDirtyResources);

      if (this->updateShaderResources<VK_PIPELINE_BIND_POINT_GRAPHICS>(m_state.gp.pipeline->layout()))
        m_flags.set(DxvkContextFlag::GpDirtyPipelineState);

      m_flags.set(
        DxvkContextFlag::GpDirtyDescriptorSet,
        DxvkContextFlag::GpDirtyDescriptorOffsets);
    }
  }
  
  
  void DxvkContext::updateGraphicsShaderDescriptors() {
    if (m_state.gp.pipeline == nullptr)
      return;

    if (m_flags.test(DxvkContextFlag::GpDirtyDescriptorSet)) {
      m_gpSet = this->updateShaderDescriptors(
        m_state.gp.pipeline->layout());
    }

    if (m_flags.test(DxvkContextFlag::GpDirtyDescriptorOffsets)) {
      this->updateShaderDescriptorSetBinding<VK_PIPELINE_BIND_POINT_GRAPHICS>(
        m_gpSet, m_state.gp.pipeline->layout());
    }

    m_flags.clr(
      DxvkContextFlag::GpDirtyDescriptorOffsets,
      DxvkContextFlag::GpDirtyDescriptorSet);
  }
  
  
  template<VkPipelineBindPoint BindPoint>
  bool DxvkContext::updateShaderResources(
    const DxvkPipelineLayout* layout) {
    DxvkBindingMask bindMask;
    bindMask.setFirst(layout->bindingCount());

    // If the depth attachment is also bound as a shader
    // resource, we have to use the appropriate layout
    VkImage       depthImage  = VK_NULL_HANDLE;
    VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    if (BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS && m_state.om.framebuffer != nullptr) {
      const auto& depthAttachment = m_state.om.framebuffer->getDepthTarget();

      if (depthAttachment.view != nullptr) {
        depthImage  = depthAttachment.view->imageHandle();
        depthLayout = depthAttachment.layout;
      }
    }

    for (uint32_t i = 0; i < layout->bindingCount(); i++) {
      const auto& binding = layout->binding(i);
      const auto& res     = m_rc[binding.slot];
      
      switch (binding.type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
          if (res.sampler != nullptr) {
            m_descInfos[i].image.sampler     = res.sampler->handle();
            m_descInfos[i].image.imageView   = VK_NULL_HANDLE;
            m_descInfos[i].image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            
            if (m_rcTracked.set(binding.slot))
              m_cmd->trackResource(res.sampler);
          } else {
            bindMask.clr(i);
            m_descInfos[i].image = m_common->dummyResources().samplerDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          if (res.imageView != nullptr && res.imageView->handle(binding.view) != VK_NULL_HANDLE) {
            m_descInfos[i].image.sampler     = VK_NULL_HANDLE;
            m_descInfos[i].image.imageView   = res.imageView->handle(binding.view);
            m_descInfos[i].image.imageLayout = res.imageView->imageInfo().layout;
            
            if (unlikely(res.imageView->imageHandle() == depthImage))
              m_descInfos[i].image.imageLayout = depthLayout;
            
            if (m_rcTracked.set(binding.slot)) {
              m_cmd->trackResource(res.imageView);
              m_cmd->trackResource(res.imageView->image());
            }
          } else {
            bindMask.clr(i);
            m_descInfos[i].image = m_common->dummyResources().imageViewDescriptor(binding.view);
          } break;
        
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          if (res.sampler != nullptr && res.imageView != nullptr
           && res.imageView->handle(binding.view) != VK_NULL_HANDLE) {
            m_descInfos[i].image.sampler     = res.sampler->handle();
            m_descInfos[i].image.imageView   = res.imageView->handle(binding.view);
            m_descInfos[i].image.imageLayout = res.imageView->imageInfo().layout;
            
            if (unlikely(res.imageView->imageHandle() == depthImage))
              m_descInfos[i].image.imageLayout = depthLayout;
            
            if (m_rcTracked.set(binding.slot)) {
              m_cmd->trackResource(res.sampler);
              m_cmd->trackResource(res.imageView);
              m_cmd->trackResource(res.imageView->image());
            }
          } else {
            bindMask.clr(i);
            m_descInfos[i].image = m_common->dummyResources().imageSamplerDescriptor(binding.view);
          } break;
        
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          if (res.bufferView != nullptr) {
            res.bufferView->updateView();
            m_descInfos[i].texelBuffer = res.bufferView->handle();
            
            if (m_rcTracked.set(binding.slot)) {
              m_cmd->trackResource(res.bufferView);
              m_cmd->trackResource(res.bufferView->buffer());
            }
          } else {
            bindMask.clr(i);
            m_descInfos[i].texelBuffer = m_common->dummyResources().bufferViewDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          if (res.bufferSlice.defined()) {
            m_descInfos[i] = res.bufferSlice.getDescriptor();
            
            if (m_rcTracked.set(binding.slot))
              m_cmd->trackResource(res.bufferSlice.buffer());
          } else {
            bindMask.clr(i);
            m_descInfos[i].buffer = m_common->dummyResources().bufferDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
          if (res.bufferSlice.defined()) {
            m_descInfos[i] = res.bufferSlice.getDescriptor();
            m_descInfos[i].buffer.offset = 0;
            
            if (m_rcTracked.set(binding.slot))
              m_cmd->trackResource(res.bufferSlice.buffer());
          } else {
            bindMask.clr(i);
            m_descInfos[i].buffer = m_common->dummyResources().bufferDescriptor();
          } break;
        
        default:
          Logger::err(str::format("DxvkContext: Unhandled descriptor type: ", binding.type));
      }
    }

    // Select the active binding mask to update
    auto& refMask = BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.state.bsBindingMask
      : m_state.cp.state.bsBindingMask;

    // If some resources are not bound, we may need to
    // update spec constants and rebind the pipeline
    bool updatePipelineState = refMask != bindMask;

    if (updatePipelineState)
      refMask = bindMask;

    return updatePipelineState;
  }
  
  
  VkDescriptorSet DxvkContext::updateShaderDescriptors(
    const DxvkPipelineLayout* layout) {
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    if (layout->bindingCount() != 0) {
      descriptorSet = allocateDescriptorSet(
        layout->descriptorSetLayout());
      
      m_cmd->updateDescriptorSetWithTemplate(
        descriptorSet, layout->descriptorTemplate(),
        m_descInfos.data());
    }

    return descriptorSet;
  }


  template<VkPipelineBindPoint BindPoint>
  void DxvkContext::updateShaderDescriptorSetBinding(
          VkDescriptorSet         set,
    const DxvkPipelineLayout*     layout) {
    if (set != VK_NULL_HANDLE) {
      for (uint32_t i = 0; i < layout->dynamicBindingCount(); i++) {
        const auto& binding = layout->dynamicBinding(i);
        const auto& res     = m_rc[binding.slot];

        m_descOffsets[i] = res.bufferSlice.defined()
          ? res.bufferSlice.getDynamicOffset()
          : 0;
      }
      
      m_cmd->cmdBindDescriptorSet(BindPoint,
        layout->pipelineLayout(), set,
        layout->dynamicBindingCount(),
        m_descOffsets.data());
    }
  }
  
  
  void DxvkContext::updateFramebuffer() {
    if (m_flags.test(DxvkContextFlag::GpDirtyFramebuffer)) {
      m_flags.clr(DxvkContextFlag::GpDirtyFramebuffer);
      
      this->spillRenderPass();
      
      auto fb = m_device->createFramebuffer(m_state.om.renderTargets);
      
      m_state.gp.state.msSampleCount = fb->getSampleCount();
      m_state.om.framebuffer = fb;

      for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
        Rc<DxvkImageView> attachment = fb->getColorTarget(i).view;

        m_state.gp.state.omComponentMapping[i] = attachment != nullptr
          ? util::invertComponentMapping(attachment->info().swizzle)
          : VkComponentMapping();
      }

      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
  }
  
  
  void DxvkContext::updateIndexBufferBinding() {
    if (m_flags.test(DxvkContextFlag::GpDirtyIndexBuffer)) {
      m_flags.clr(DxvkContextFlag::GpDirtyIndexBuffer);
      
      if (m_state.vi.indexBuffer.defined()) {
        auto bufferInfo = m_state.vi.indexBuffer.getDescriptor();
        
        m_cmd->cmdBindIndexBuffer(
          bufferInfo.buffer.buffer,
          bufferInfo.buffer.offset,
          m_state.vi.indexType);

        if (m_vbTracked.set(MaxNumVertexBindings))
          m_cmd->trackResource(m_state.vi.indexBuffer.buffer());
      } else {
        m_cmd->cmdBindIndexBuffer(
          m_common->dummyResources().bufferHandle(),
          0, VK_INDEX_TYPE_UINT32);
      }
    }
  }
  
  
  void DxvkContext::updateVertexBufferBindings() {
    if (m_flags.test(DxvkContextFlag::GpDirtyVertexBuffers)) {
      m_flags.clr(DxvkContextFlag::GpDirtyVertexBuffers);

      if (unlikely(!m_state.gp.state.ilBindingCount))
        return;
      
      std::array<VkBuffer,     MaxNumVertexBindings> buffers;
      std::array<VkDeviceSize, MaxNumVertexBindings> offsets;
      
      // Set buffer handles and offsets for active bindings
      for (uint32_t i = 0; i < m_state.gp.state.ilBindingCount; i++) {
        uint32_t binding = m_state.gp.state.ilBindings[i].binding;
        
        if (likely(m_state.vi.vertexBuffers[binding].defined())) {
          auto vbo = m_state.vi.vertexBuffers[binding].getDescriptor();
          
          buffers[i] = vbo.buffer.buffer;
          offsets[i] = vbo.buffer.offset;
          
          if (m_vbTracked.set(binding))
            m_cmd->trackResource(m_state.vi.vertexBuffers[binding].buffer());
        } else {
          buffers[i] = m_common->dummyResources().bufferHandle();
          offsets[i] = 0;
        }
      }
      
      // Vertex bindigs get remapped when compiling the
      // pipeline, so this actually does the right thing
      m_cmd->cmdBindVertexBuffers(
        0, m_state.gp.state.ilBindingCount,
        buffers.data(), offsets.data());
    }
  }
  
  
  void DxvkContext::updateTransformFeedbackBuffers() {
    auto gsOptions = m_state.gp.shaders.gs->shaderOptions();

    VkBuffer     xfbBuffers[MaxNumXfbBuffers];
    VkDeviceSize xfbOffsets[MaxNumXfbBuffers];
    VkDeviceSize xfbLengths[MaxNumXfbBuffers];

    for (size_t i = 0; i < MaxNumXfbBuffers; i++) {
      auto physSlice = m_state.xfb.buffers[i].getSliceHandle();
      
      xfbBuffers[i] = physSlice.handle;
      xfbOffsets[i] = physSlice.offset;
      xfbLengths[i] = physSlice.length;

      if (physSlice.handle == VK_NULL_HANDLE)
        xfbBuffers[i] = m_common->dummyResources().bufferHandle();
      
      if (physSlice.handle != VK_NULL_HANDLE) {
        auto buffer = m_state.xfb.buffers[i].buffer();
        buffer->setXfbVertexStride(gsOptions.xfbStrides[i]);
        
        m_cmd->trackResource(buffer);
      }
    }

    m_cmd->cmdBindTransformFeedbackBuffers(
      0, MaxNumXfbBuffers,
      xfbBuffers, xfbOffsets, xfbLengths);
  }


  void DxvkContext::updateTransformFeedbackState() {
    if (m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasTransformFeedback)) {
      if (m_flags.test(DxvkContextFlag::GpDirtyXfbBuffers)) {
        m_flags.clr(DxvkContextFlag::GpDirtyXfbBuffers);

        this->pauseTransformFeedback();
        this->updateTransformFeedbackBuffers();
      }

      this->startTransformFeedback();
    }
  }

  
  void DxvkContext::updateConditionalRendering() {
    if (m_flags.test(DxvkContextFlag::GpDirtyPredicate)) {
      m_flags.clr(DxvkContextFlag::GpDirtyPredicate);

      pauseConditionalRendering();

      if (m_state.cond.predicate.defined())
        startConditionalRendering();
    }
  }


  void DxvkContext::updateDynamicState() {
    if (m_gpActivePipeline == VK_NULL_HANDLE)
      return;
    
    if (m_flags.test(DxvkContextFlag::GpDirtyViewport)) {
      m_flags.clr(DxvkContextFlag::GpDirtyViewport);

      uint32_t viewportCount = m_state.gp.state.rsViewportCount;
      m_cmd->cmdSetViewport(0, viewportCount, m_state.vp.viewports.data());
      m_cmd->cmdSetScissor (0, viewportCount, m_state.vp.scissorRects.data());
    }

    if (m_flags.all(DxvkContextFlag::GpDirtyBlendConstants,
                    DxvkContextFlag::GpDynamicBlendConstants)) {
      m_flags.clr(DxvkContextFlag::GpDirtyBlendConstants);
      m_cmd->cmdSetBlendConstants(&m_state.dyn.blendConstants.r);
    }

    if (m_flags.all(DxvkContextFlag::GpDirtyStencilRef,
                    DxvkContextFlag::GpDynamicStencilRef)) {
      m_flags.clr(DxvkContextFlag::GpDirtyStencilRef);

      m_cmd->cmdSetStencilReference(
        VK_STENCIL_FRONT_AND_BACK,
        m_state.dyn.stencilReference);
    }
    
    if (m_flags.all(DxvkContextFlag::GpDirtyDepthBias,
                    DxvkContextFlag::GpDynamicDepthBias)) {
      m_flags.clr(DxvkContextFlag::GpDirtyDepthBias);

      m_cmd->cmdSetDepthBias(
        m_state.dyn.depthBias.depthBiasConstant,
        m_state.dyn.depthBias.depthBiasClamp,
        m_state.dyn.depthBias.depthBiasSlope);
    }
    
    if (m_flags.all(DxvkContextFlag::GpDirtyDepthBounds,
                    DxvkContextFlag::GpDynamicDepthBounds)) {
      m_flags.clr(DxvkContextFlag::GpDirtyDepthBounds);

      m_cmd->cmdSetDepthBounds(
        m_state.dyn.depthBounds.minDepthBounds,
        m_state.dyn.depthBounds.maxDepthBounds);
    }
  }


  template<VkPipelineBindPoint BindPoint>
  void DxvkContext::updatePushConstants() {
    if (m_flags.test(DxvkContextFlag::DirtyPushConstants)) {
      m_flags.clr(DxvkContextFlag::DirtyPushConstants);

      auto layout = BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
        ? (m_state.gp.pipeline != nullptr ? m_state.gp.pipeline->layout() : nullptr)
        : (m_state.cp.pipeline != nullptr ? m_state.cp.pipeline->layout() : nullptr);
      
      if (!layout)
        return;
      
      VkPushConstantRange pushConstRange = layout->pushConstRange();
      if (!pushConstRange.size)
        return;
      
      m_cmd->cmdPushConstants(
        layout->pipelineLayout(),
        pushConstRange.stageFlags,
        pushConstRange.offset,
        pushConstRange.size,
        &m_state.pc.data[pushConstRange.offset]);
    }
  }
  
  
  void DxvkContext::commitComputeState() {
    if (m_flags.test(DxvkContextFlag::GpRenderPassBound))
      this->spillRenderPass();

    if (m_flags.test(DxvkContextFlag::GpClearRenderTargets))
      this->clearRenderPass();
    
    if (m_flags.test(DxvkContextFlag::CpDirtyPipeline))
      this->updateComputePipeline();
    
    if (m_flags.any(
          DxvkContextFlag::CpDirtyResources,
          DxvkContextFlag::CpDirtyDescriptorOffsets))
    this->updateComputeShaderResources();

    if (m_flags.test(DxvkContextFlag::CpDirtyPipelineState))
      this->updateComputePipelineState();
    
    if (m_flags.any(
          DxvkContextFlag::CpDirtyDescriptorSet,
          DxvkContextFlag::CpDirtyDescriptorOffsets))
      this->updateComputeShaderDescriptors();
    
    if (m_flags.test(DxvkContextFlag::DirtyPushConstants))
      this->updatePushConstants<VK_PIPELINE_BIND_POINT_COMPUTE>();
  }
  
  
  template<bool Indexed>
  void DxvkContext::commitGraphicsState() {
    if (m_flags.test(DxvkContextFlag::GpDirtyFramebuffer))
      this->updateFramebuffer();

    if (!m_flags.test(DxvkContextFlag::GpRenderPassBound))
      this->startRenderPass();
    
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline))
      this->updateGraphicsPipeline();
    
    if (m_flags.test(DxvkContextFlag::GpDirtyIndexBuffer) && Indexed)
      this->updateIndexBufferBinding();
    
    if (m_flags.test(DxvkContextFlag::GpDirtyVertexBuffers))
      this->updateVertexBufferBindings();
    
    if (m_flags.any(
          DxvkContextFlag::GpDirtyResources,
          DxvkContextFlag::GpDirtyDescriptorOffsets))
      this->updateGraphicsShaderResources();
    
    if (m_flags.test(DxvkContextFlag::GpDirtyPipelineState))
      this->updateGraphicsPipelineState();
    
    if (m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasTransformFeedback))
      this->updateTransformFeedbackState();
    
    if (m_flags.test(DxvkContextFlag::GpDirtyPredicate))
      this->updateConditionalRendering();

    if (m_flags.any(
          DxvkContextFlag::GpDirtyDescriptorSet,
          DxvkContextFlag::GpDirtyDescriptorOffsets))
      this->updateGraphicsShaderDescriptors();
    
    if (m_flags.any(
          DxvkContextFlag::GpDirtyViewport,
          DxvkContextFlag::GpDirtyBlendConstants,
          DxvkContextFlag::GpDirtyStencilRef,
          DxvkContextFlag::GpDirtyDepthBias,
          DxvkContextFlag::GpDirtyDepthBounds))
      this->updateDynamicState();
    
    if (m_flags.test(DxvkContextFlag::DirtyPushConstants))
      this->updatePushConstants<VK_PIPELINE_BIND_POINT_GRAPHICS>();
  }
  
  
  void DxvkContext::commitComputeInitBarriers() {
    auto layout = m_state.cp.pipeline->layout();

    bool requiresBarrier = false;

    for (uint32_t i = 0; i < layout->bindingCount() && !requiresBarrier; i++) {
      if (m_state.cp.state.bsBindingMask.test(i)) {
        const DxvkDescriptorSlot binding = layout->binding(i);
        const DxvkShaderResourceSlot& slot = m_rc[binding.slot];

        DxvkAccessFlags dstAccess = DxvkAccess::Read;
        DxvkAccessFlags srcAccess = 0;
        
        switch (binding.type) {
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            if (binding.access & VK_ACCESS_SHADER_WRITE_BIT)
              dstAccess.set(DxvkAccess::Write);
            /* fall through */
          
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            srcAccess = m_execBarriers.getBufferAccess(
              slot.bufferSlice.getSliceHandle());
            break;
        
          case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (binding.access & VK_ACCESS_SHADER_WRITE_BIT)
              dstAccess.set(DxvkAccess::Write);
            /* fall through */

          case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            srcAccess = m_execBarriers.getBufferAccess(
              slot.bufferView->getSliceHandle());
            break;
          
          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            if (binding.access & VK_ACCESS_SHADER_WRITE_BIT)
              dstAccess.set(DxvkAccess::Write);
            /* fall through */

          case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            srcAccess = m_execBarriers.getImageAccess(
              slot.imageView->image(),
              slot.imageView->imageSubresources());
            break;

          default:
            /* nothing to do */;
        }

        if (srcAccess == 0)
          continue;

        // Skip write-after-write barriers if explicitly requested
        if ((m_barrierControl.test(DxvkBarrierControl::IgnoreWriteAfterWrite))
         && (m_execBarriers.getSrcStages() == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
         && (srcAccess.test(DxvkAccess::Write))
         && (dstAccess.test(DxvkAccess::Write)))
          continue;

        requiresBarrier = (srcAccess | dstAccess).test(DxvkAccess::Write);
      }
    }

    if (requiresBarrier)
      m_execBarriers.recordCommands(m_cmd);
  }
  

  void DxvkContext::commitComputePostBarriers() {
    auto layout = m_state.cp.pipeline->layout();
    
    for (uint32_t i = 0; i < layout->bindingCount(); i++) {
      if (m_state.cp.state.bsBindingMask.test(i)) {
        const DxvkDescriptorSlot binding = layout->binding(i);
        const DxvkShaderResourceSlot& slot = m_rc[binding.slot];

        VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkAccessFlags        access = VK_ACCESS_SHADER_READ_BIT;
        
        switch (binding.type) {
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            if (binding.access & VK_ACCESS_SHADER_WRITE_BIT)
              access |= VK_ACCESS_SHADER_WRITE_BIT;
            /* fall through */
          
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            m_execBarriers.accessBuffer(
              slot.bufferSlice.getSliceHandle(),
              stages, access,
              slot.bufferSlice.bufferInfo().stages,
              slot.bufferSlice.bufferInfo().access);
            break;
        
          case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (binding.access & VK_ACCESS_SHADER_WRITE_BIT)
              access |= VK_ACCESS_SHADER_WRITE_BIT;
            /* fall through */

          case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            m_execBarriers.accessBuffer(
              slot.bufferView->getSliceHandle(),
              stages, access,
              slot.bufferView->bufferInfo().stages,
              slot.bufferView->bufferInfo().access);
            break;
          
          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            if (binding.access & VK_ACCESS_SHADER_WRITE_BIT)
              access |= VK_ACCESS_SHADER_WRITE_BIT;
            /* fall through */

          case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            m_execBarriers.accessImage(
              slot.imageView->image(),
              slot.imageView->imageSubresources(),
              slot.imageView->imageInfo().layout,
              stages, access,
              slot.imageView->imageInfo().layout,
              slot.imageView->imageInfo().stages,
              slot.imageView->imageInfo().access);
            break;

          default:
            /* nothing to do */;
        }
      }
    }
  }
  
  
  void DxvkContext::commitGraphicsPostBarriers() {
    bool fs = m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasFsStorageDescriptors);
    bool vs = m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasVsStorageDescriptors);

    if (vs) {
      // External subpass dependencies serve as full memory
      // and execution barriers, so we can use this to allow
      // inter-stage synchronization.
      this->spillRenderPass();
    } else if (fs) {
      this->emitMemoryBarrier(
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    }
  }


  void DxvkContext::emitMemoryBarrier(
          VkPipelineStageFlags      srcStages,
          VkAccessFlags             srcAccess,
          VkPipelineStageFlags      dstStages,
          VkAccessFlags             dstAccess) {
    VkMemoryBarrier barrier;
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.pNext         = nullptr;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    m_cmd->cmdPipelineBarrier(
      DxvkCmdBuffer::ExecBuffer, srcStages, dstStages,
      0, 1, &barrier, 0, nullptr, 0, nullptr);
  }


  VkDescriptorSet DxvkContext::allocateDescriptorSet(
          VkDescriptorSetLayout     layout) {
    if (m_descPool == nullptr)
      m_descPool = m_device->createDescriptorPool();
    
    VkDescriptorSet set = m_descPool->alloc(layout);

    if (set == VK_NULL_HANDLE) {
      m_cmd->trackDescriptorPool(std::move(m_descPool));

      m_descPool = m_device->createDescriptorPool();
      set = m_descPool->alloc(layout);
    }

    return set;
  }

  
  void DxvkContext::trackDrawBuffer() {
    if (m_flags.test(DxvkContextFlag::DirtyDrawBuffer)) {
      m_flags.clr(DxvkContextFlag::DirtyDrawBuffer);

      if (m_state.id.argBuffer.defined())
        m_cmd->trackResource(m_state.id.argBuffer.buffer());

      if (m_state.id.cntBuffer.defined())
        m_cmd->trackResource(m_state.id.cntBuffer.buffer());
    }
  }
  
}