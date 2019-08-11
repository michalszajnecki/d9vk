#include "dxvk_device.h"
#include "dxvk_meta_resolve.h"

#include <dxvk_fullscreen_geom.h>
#include <dxvk_fullscreen_vert.h>
#include <dxvk_fullscreen_layer_vert.h>

#include <dxvk_resolve_frag_f.h>
#include <dxvk_resolve_frag_f_amd.h>
#include <dxvk_resolve_frag_u.h>
#include <dxvk_resolve_frag_i.h>

namespace dxvk {
  
  DxvkMetaResolveRenderPass::DxvkMetaResolveRenderPass(
    const Rc<vk::DeviceFn>&   vkd,
    const Rc<DxvkImageView>&  dstImageView,
    const Rc<DxvkImageView>&  srcImageView,
          bool                discardDst)
  : m_vkd(vkd),
    m_dstImageView(dstImageView),
    m_srcImageView(srcImageView),
    m_renderPass  (createRenderPass(discardDst)),
    m_framebuffer (createFramebuffer()) { }
  

  DxvkMetaResolveRenderPass::~DxvkMetaResolveRenderPass() {
    m_vkd->vkDestroyFramebuffer(m_vkd->device(), m_framebuffer, nullptr);
    m_vkd->vkDestroyRenderPass (m_vkd->device(), m_renderPass,  nullptr);
  }


  VkRenderPass DxvkMetaResolveRenderPass::createRenderPass(bool discard) const {
    VkAttachmentDescription attachment;
    attachment.flags            = 0;
    attachment.format           = m_dstImageView->info().format;
    attachment.samples          = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp           = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp    = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp   = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout    = m_dstImageView->imageInfo().layout;
    attachment.finalLayout      = m_dstImageView->imageInfo().layout;

    if (discard) {
      attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    
    VkAttachmentReference dstRef;
    dstRef.attachment    = 0;
    dstRef.layout        = m_dstImageView->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    
    VkSubpassDescription subpass;
    subpass.flags               = 0;
    subpass.pipelineBindPoint   = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.inputAttachmentCount = 0;
    subpass.pInputAttachments   = nullptr;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments   = &dstRef;
    subpass.pResolveAttachments = nullptr;
    subpass.pDepthStencilAttachment = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = nullptr;

    VkRenderPassCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.attachmentCount        = 1;
    info.pAttachments           = &attachment;
    info.subpassCount           = 1;
    info.pSubpasses             = &subpass;
    info.dependencyCount        = 0;
    info.pDependencies          = nullptr;

    VkRenderPass result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateRenderPass(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaResolveRenderPass: Failed to create render pass");
    return result;
  }


  VkFramebuffer DxvkMetaResolveRenderPass::createFramebuffer() const {
    VkImageSubresourceRange dstSubresources = m_dstImageView->subresources();
    VkExtent3D              dstExtent       = m_dstImageView->mipLevelExtent(0);
    VkImageView             dstHandle       = m_dstImageView->handle();

    VkFramebufferCreateInfo fboInfo;
    fboInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fboInfo.pNext           = nullptr;
    fboInfo.flags           = 0;
    fboInfo.renderPass      = m_renderPass;
    fboInfo.attachmentCount = 1;
    fboInfo.pAttachments    = &dstHandle;
    fboInfo.width           = dstExtent.width;
    fboInfo.height          = dstExtent.height;
    fboInfo.layers          = dstSubresources.layerCount;
    
    VkFramebuffer result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateFramebuffer(m_vkd->device(), &fboInfo, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaMipGenRenderPass: Failed to create target framebuffer");
    return result;
  }



  DxvkMetaResolveObjects::DxvkMetaResolveObjects(const DxvkDevice* device)
  : m_vkd         (device->vkd()),
    m_sampler     (createSampler()),
    m_shaderFragF (device->extensions().amdShaderFragmentMask
      ? createShaderModule(dxvk_resolve_frag_f_amd)
      : createShaderModule(dxvk_resolve_frag_f)),
    m_shaderFragU (createShaderModule(dxvk_resolve_frag_u)),
    m_shaderFragI (createShaderModule(dxvk_resolve_frag_i)) {
    if (device->extensions().extShaderViewportIndexLayer) {
      m_shaderVert = createShaderModule(dxvk_fullscreen_layer_vert);
    } else {
      m_shaderVert = createShaderModule(dxvk_fullscreen_vert);
      m_shaderGeom = createShaderModule(dxvk_fullscreen_geom);
    }
  }


  DxvkMetaResolveObjects::~DxvkMetaResolveObjects() {
    for (const auto& pair : m_pipelines) {
      m_vkd->vkDestroyPipeline(m_vkd->device(), pair.second.pipeHandle, nullptr);
      m_vkd->vkDestroyPipelineLayout(m_vkd->device(), pair.second.pipeLayout, nullptr);
      m_vkd->vkDestroyDescriptorSetLayout (m_vkd->device(), pair.second.dsetLayout, nullptr);
      m_vkd->vkDestroyRenderPass(m_vkd->device(), pair.second.renderPass, nullptr);
    }

    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_shaderFragF, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_shaderFragI, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_shaderFragU, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_shaderGeom, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_shaderVert, nullptr);

    m_vkd->vkDestroySampler(m_vkd->device(), m_sampler, nullptr);
  }


  DxvkMetaResolvePipeline DxvkMetaResolveObjects::getPipeline(
          VkFormat              format,
          VkSampleCountFlagBits samples) {
    std::lock_guard<std::mutex> lock(m_mutex);

    DxvkMetaResolvePipelineKey key;
    key.format  = format;
    key.samples = samples;
    
    auto entry = m_pipelines.find(key);
    if (entry != m_pipelines.end())
      return entry->second;

    DxvkMetaResolvePipeline pipeline = createPipeline(key);
    m_pipelines.insert({ key, pipeline });
    return pipeline;
  }
  
  
  VkSampler DxvkMetaResolveObjects::createSampler() const {
    VkSamplerCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.magFilter              = VK_FILTER_NEAREST;
    info.minFilter              = VK_FILTER_NEAREST;
    info.mipmapMode             = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU           = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV           = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW           = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.mipLodBias             = 0.0f;
    info.anisotropyEnable       = VK_FALSE;
    info.maxAnisotropy          = 1.0f;
    info.compareEnable          = VK_FALSE;
    info.compareOp              = VK_COMPARE_OP_ALWAYS;
    info.minLod                 = 0.0f;
    info.maxLod                 = 0.0f;
    info.borderColor            = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;
    
    VkSampler result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateSampler(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaResolveObjects: Failed to create sampler");
    return result;
  }

  
  VkShaderModule DxvkMetaResolveObjects::createShaderModule(
    const SpirvCodeBuffer&       code) const {
    VkShaderModuleCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.codeSize               = code.size();
    info.pCode                  = code.data();
    
    VkShaderModule result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateShaderModule(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create shader module");
    return result;
  }

  
  DxvkMetaResolvePipeline DxvkMetaResolveObjects::createPipeline(
    const DxvkMetaResolvePipelineKey& key) {
    DxvkMetaResolvePipeline pipeline;
    pipeline.renderPass = this->createRenderPass(key);
    pipeline.dsetLayout = this->createDescriptorSetLayout();
    pipeline.pipeLayout = this->createPipelineLayout(pipeline.dsetLayout);
    pipeline.pipeHandle = this->createPipelineObject(key, pipeline.pipeLayout, pipeline.renderPass);
    return pipeline;
  }


  VkRenderPass DxvkMetaResolveObjects::createRenderPass(
    const DxvkMetaResolvePipelineKey& key) {
    VkAttachmentDescription attachment;
    attachment.flags            = 0;
    attachment.format           = key.format;
    attachment.samples          = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp           = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp    = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp   = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout    = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout      = VK_IMAGE_LAYOUT_GENERAL;
    
    VkAttachmentReference attachmentRef;
    attachmentRef.attachment    = 0;
    attachmentRef.layout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass;
    subpass.flags                   = 0;
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.inputAttachmentCount    = 0;
    subpass.pInputAttachments       = nullptr;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &attachmentRef;
    subpass.pResolveAttachments     = nullptr;
    subpass.pDepthStencilAttachment = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments    = nullptr;

    VkRenderPassCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.attachmentCount        = 1;
    info.pAttachments           = &attachment;
    info.subpassCount           = 1;
    info.pSubpasses             = &subpass;
    info.dependencyCount        = 0;
    info.pDependencies          = nullptr;

    VkRenderPass result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateRenderPass(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaResolveObjects: Failed to create render pass");
    return result;
  }

  
  VkDescriptorSetLayout DxvkMetaResolveObjects::createDescriptorSetLayout() const {
    VkDescriptorSetLayoutBinding binding;
    binding.binding             = 0;
    binding.descriptorType      = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount     = 1;
    binding.stageFlags          = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers  = &m_sampler;
    
    VkDescriptorSetLayoutCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.bindingCount           = 1;
    info.pBindings              = &binding;
    
    VkDescriptorSetLayout result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateDescriptorSetLayout(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaResolveObjects: Failed to create descriptor set layout");
    return result;
  }
  

  VkPipelineLayout DxvkMetaResolveObjects::createPipelineLayout(
          VkDescriptorSetLayout  descriptorSetLayout) const {
    VkPushConstantRange push;
    push.stageFlags             = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset                 = 0;
    push.size                   = sizeof(VkOffset2D);

    VkPipelineLayoutCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.setLayoutCount         = 1;
    info.pSetLayouts            = &descriptorSetLayout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges    = &push;
    
    VkPipelineLayout result = VK_NULL_HANDLE;
    if (m_vkd->vkCreatePipelineLayout(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create pipeline layout");
    return result;
  }
  

  VkPipeline DxvkMetaResolveObjects::createPipelineObject(
    const DxvkMetaResolvePipelineKey& key,
          VkPipelineLayout       pipelineLayout,
          VkRenderPass           renderPass) {
    auto formatInfo = imageFormatInfo(key.format);

    std::array<VkPipelineShaderStageCreateInfo, 3> stages;
    uint32_t stageCount = 0;

    VkSpecializationMapEntry specEntry;
    specEntry.constantID        = 0;
    specEntry.offset            = 0;
    specEntry.size              = sizeof(VkSampleCountFlagBits);

    VkSpecializationInfo specInfo;
    specInfo.mapEntryCount      = 1;
    specInfo.pMapEntries        = &specEntry;
    specInfo.dataSize           = sizeof(VkSampleCountFlagBits);
    specInfo.pData              = &key.samples;
    
    VkPipelineShaderStageCreateInfo& vsStage = stages[stageCount++];
    vsStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vsStage.pNext               = nullptr;
    vsStage.flags               = 0;
    vsStage.stage               = VK_SHADER_STAGE_VERTEX_BIT;
    vsStage.module              = m_shaderVert;
    vsStage.pName               = "main";
    vsStage.pSpecializationInfo = nullptr;
    
    if (m_shaderGeom) {
      VkPipelineShaderStageCreateInfo& gsStage = stages[stageCount++];
      gsStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      gsStage.pNext               = nullptr;
      gsStage.flags               = 0;
      gsStage.stage               = VK_SHADER_STAGE_GEOMETRY_BIT;
      gsStage.module              = m_shaderGeom;
      gsStage.pName               = "main";
      gsStage.pSpecializationInfo = nullptr;
    }
    
    VkPipelineShaderStageCreateInfo& psStage = stages[stageCount++];
    psStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    psStage.pNext               = nullptr;
    psStage.flags               = 0;
    psStage.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
    psStage.module              = m_shaderFragF;
    psStage.pName               = "main";
    psStage.pSpecializationInfo = &specInfo;

    if (formatInfo->flags.test(DxvkFormatFlag::SampledUInt))
      psStage.module            = m_shaderFragU;
    else if (formatInfo->flags.test(DxvkFormatFlag::SampledSInt))
      psStage.module            = m_shaderFragI;
    
    std::array<VkDynamicState, 2> dynStates = {{
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
    }};
    
    VkPipelineDynamicStateCreateInfo dynState;
    dynState.sType              = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.pNext              = nullptr;
    dynState.flags              = 0;
    dynState.dynamicStateCount  = dynStates.size();
    dynState.pDynamicStates     = dynStates.data();
    
    VkPipelineVertexInputStateCreateInfo viState;
    viState.sType               = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viState.pNext               = nullptr;
    viState.flags               = 0;
    viState.vertexBindingDescriptionCount   = 0;
    viState.pVertexBindingDescriptions      = nullptr;
    viState.vertexAttributeDescriptionCount = 0;
    viState.pVertexAttributeDescriptions    = nullptr;
    
    VkPipelineInputAssemblyStateCreateInfo iaState;
    iaState.sType               = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaState.pNext               = nullptr;
    iaState.flags               = 0;
    iaState.topology            = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    iaState.primitiveRestartEnable = VK_FALSE;
    
    VkPipelineViewportStateCreateInfo vpState;
    vpState.sType               = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.pNext               = nullptr;
    vpState.flags               = 0;
    vpState.viewportCount       = 1;
    vpState.pViewports          = nullptr;
    vpState.scissorCount        = 1;
    vpState.pScissors           = nullptr;
    
    VkPipelineRasterizationStateCreateInfo rsState;
    rsState.sType               = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rsState.pNext               = nullptr;
    rsState.flags               = 0;
    rsState.depthClampEnable    = VK_TRUE;
    rsState.rasterizerDiscardEnable = VK_FALSE;
    rsState.polygonMode         = VK_POLYGON_MODE_FILL;
    rsState.cullMode            = VK_CULL_MODE_NONE;
    rsState.frontFace           = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.depthBiasEnable     = VK_FALSE;
    rsState.depthBiasConstantFactor = 0.0f;
    rsState.depthBiasClamp          = 0.0f;
    rsState.depthBiasSlopeFactor    = 0.0f;
    rsState.lineWidth           = 1.0f;
    
    uint32_t msMask = 0xFFFFFFFF;
    VkPipelineMultisampleStateCreateInfo msState;
    msState.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msState.pNext                 = nullptr;
    msState.flags                 = 0;
    msState.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
    msState.sampleShadingEnable   = VK_FALSE;
    msState.minSampleShading      = 1.0f;
    msState.pSampleMask           = &msMask;
    msState.alphaToCoverageEnable = VK_FALSE;
    msState.alphaToOneEnable      = VK_FALSE;

    VkPipelineColorBlendAttachmentState cbAttachment;
    cbAttachment.blendEnable         = VK_FALSE;
    cbAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
    cbAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    cbAttachment.colorWriteMask      =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    
    VkPipelineColorBlendStateCreateInfo cbState;
    cbState.sType               = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbState.pNext               = nullptr;
    cbState.flags               = 0;
    cbState.logicOpEnable       = VK_FALSE;
    cbState.logicOp             = VK_LOGIC_OP_NO_OP;
    cbState.attachmentCount     = 1;
    cbState.pAttachments        = &cbAttachment;
    
    for (uint32_t i = 0; i < 4; i++)
      cbState.blendConstants[i] = 0.0f;
    
    VkGraphicsPipelineCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.stageCount             = stageCount;
    info.pStages                = stages.data();
    info.pVertexInputState      = &viState;
    info.pInputAssemblyState    = &iaState;
    info.pTessellationState     = nullptr;
    info.pViewportState         = &vpState;
    info.pRasterizationState    = &rsState;
    info.pMultisampleState      = &msState;
    info.pColorBlendState       = &cbState;
    info.pDepthStencilState     = nullptr;
    info.pDynamicState          = &dynState;
    info.layout                 = pipelineLayout;
    info.renderPass             = renderPass;
    info.subpass                = 0;
    info.basePipelineHandle     = VK_NULL_HANDLE;
    info.basePipelineIndex      = -1;
    
    VkPipeline result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateGraphicsPipelines(m_vkd->device(), VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create graphics pipeline");
    return result;
  }

}
