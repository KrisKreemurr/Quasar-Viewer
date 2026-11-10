#include "vk_display_encode.h"
#include "vk_pipeline.h"
#include <utility>

void VkDisplayEncode::destroy()
{
    if (!mContext) return;
    const VkDevice device=mContext->device();
    if (mPipeline) { vkForgetPipeline(mPipeline); vkDestroyPipeline(device,mPipeline,nullptr); }
    if (mLayout) vkDestroyPipelineLayout(device,mLayout,nullptr);
    if (mPool) vkDestroyDescriptorPool(device,mPool,nullptr);
    if (mSetLayout) vkDestroyDescriptorSetLayout(device,mSetLayout,nullptr);
    if (mSampler) vkDestroySampler(device,mSampler,nullptr);
    mPipeline=VK_NULL_HANDLE; mLayout=VK_NULL_HANDLE; mPool=VK_NULL_HANDLE;
    mSetLayout=VK_NULL_HANDLE; mSampler=VK_NULL_HANDLE;
    mSets={};mInputs={};mContext=nullptr;
    mTarget={VK_FORMAT_UNDEFINED,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    mEncoding=vkdisplay::Encoding::Sdr;
}

bool VkDisplayEncode::validForTarget(VkSurfaceFormatKHR target) const
{
    return mContext && mPipeline && mTarget.format==target.format &&
        mTarget.colorSpace==target.colorSpace;
}

void VkDisplayEncode::swap(VkDisplayEncode& other) noexcept
{
    using std::swap;
    swap(mContext,other.mContext);swap(mPipeline,other.mPipeline);swap(mLayout,other.mLayout);
    swap(mSetLayout,other.mSetLayout);swap(mPool,other.mPool);swap(mSampler,other.mSampler);
    swap(mSets,other.mSets);swap(mInputs,other.mInputs);swap(mTarget,other.mTarget);
    swap(mEncoding,other.mEncoding);
}

bool VkDisplayEncode::prepare(VkDeviceContext& ctx,bool enabled,VkSurfaceFormatKHR target,
                             std::span<const uint32_t> vertex,std::span<const uint32_t> fragment)
{
    destroy(); // Caller drained; toggling off releases once, never allocates.
    if (!enabled) return true;
    if (vertex.empty() || fragment.empty()) return false;
    mEncoding=vkdisplay::encodingOf(target);
    mTarget=target;
    const bool sdrCapture=target.format==VK_FORMAT_B8G8R8A8_UNORM &&
                          target.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (mEncoding==vkdisplay::Encoding::Sdr && !sdrCapture) return false;
    mContext=&ctx;
    const VkDevice device=ctx.device();
    auto failed=[this] { destroy();return false; };
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter=VK_FILTER_NEAREST;sampler.minFilter=VK_FILTER_NEAREST;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device,&sampler,nullptr,&mSampler)!=VK_SUCCESS) return failed();
    VkDescriptorSetLayoutBinding binding{};
    binding.binding=0;binding.descriptorCount=1;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;binding.pImmutableSamplers=&mSampler;
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount=1;set.pBindings=&binding;
    if (vkCreateDescriptorSetLayout(device,&set,nullptr,&mSetLayout)!=VK_SUCCESS) return failed();
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=3;pool.poolSizeCount=1;pool.pPoolSizes=&poolSize;
    if (vkCreateDescriptorPool(device,&pool,nullptr,&mPool)!=VK_SUCCESS) return failed();
    const VkDescriptorSetLayout layouts[]{mSetLayout,mSetLayout,mSetLayout};
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool=mPool;allocate.descriptorSetCount=3;allocate.pSetLayouts=layouts;
    if (vkAllocateDescriptorSets(device,&allocate,mSets.data())!=VK_SUCCESS) return failed();
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount=1;layout.pSetLayouts=&mSetLayout;
    layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    if (vkCreatePipelineLayout(device,&layout,nullptr,&mLayout)!=VK_SUCCESS) return failed();
    VkShaderModule modules[2]{};
    const std::span<const uint32_t> binaries[]{vertex,fragment};
    for (unsigned i=0;i<2;++i) {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize=binaries[i].size_bytes();shader.pCode=binaries[i].data();
        if (vkCreateShaderModule(device,&shader,nullptr,&modules[i])!=VK_SUCCESS) {
            if (modules[0]) vkDestroyShaderModule(device,modules[0],nullptr);
            return failed();
        }
    }
    GraphicsPipelineDesc description;
    description.vert=modules[0];description.frag=modules[1];description.layout=mLayout;
    description.colorFormat=target.format;description.depthFormat=VK_FORMAT_UNDEFINED;
    description.depthTest=false;description.depthWrite=false;description.blend=false;
    // Existing pipeline factory returns null on bounded allocation/OOM recovery.
    mPipeline=createGraphicsPipeline(device,description);
    for (auto module:modules) vkDestroyShaderModule(device,module,nullptr);
    if (!mPipeline) return failed();
    return true;
}

bool VkDisplayEncode::record(VkCommandBuffer cmd,uint32_t slot,VkImageView input,VkImageView target,
                            VkExtent2D extent,vkdisplay::Request request,Content content,
                            uint64_t allHdrImageBytes,uint64_t declaredBudget,bool dither)
{
    if (!mContext || !mPipeline || !cmd || slot>=mSets.size() || !input || !target || input==target ||
        !vkdisplay::imageBudget(extent.width,extent.height,1,allHdrImageBytes,declaredBudget)) return false;
    if ((mEncoding==vkdisplay::Encoding::Sdr && content!=Content::WorldBeforeUi) ||
        (mEncoding!=vkdisplay::Encoding::Sdr && content!=Content::CompleteDisplay)) return false;
    if (mInputs[slot]!=input) {
        VkDescriptorImageInfo image{mSampler,input,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=mSets[slot];write.dstBinding=0;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
        vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
        mInputs[slot]=input;
    }
    request=vkdisplay::sanitized(request);
    struct Push { uint32_t encoding;float paperWhite,peakNits,dither; };
    const Push push{uint32_t(mEncoding),request.paperWhiteNits,request.peakNits,dither ? 1.f : 0.f};
    static_assert(sizeof(Push)==16);
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView=target;colour.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;colour.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea={{0,0},extent};rendering.layerCount=1;
    rendering.colorAttachmentCount=1;rendering.pColorAttachments=&colour;
    vkCmdBeginRendering(cmd,&rendering);
    VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};
    VkRect2D scissor{{0,0},extent};
    vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
    bindGraphicsPipeline(cmd,mPipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mLayout,0,1,&mSets[slot],0,nullptr);
    vkCmdPushConstants(cmd,mLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    vkCmdDraw(cmd,3,1,0,0);
    vkCmdEndRendering(cmd);
    return true;
}
