// Copyright (c) 2018 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

#include <rvg/context.hpp>
#include <rvg/font.hpp>
#include <rvg/text.hpp>
#include <rvg/polygon.hpp>
#include <rvg/shapes.hpp>
#include <rvg/state.hpp>
#include <rvg/deviceObject.hpp>
#include <rvg/util.hpp>

#include <vpp/vk.hpp>
#include <vpp/queue.hpp>
#include <dlg/dlg.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vecOps.hpp>
#include <cstring>

#include <shaders/fill.vert.frag_scissor.h>
#include <shaders/fill.frag.frag_scissor.h>
#include <shaders/fill.vert.plane_scissor.h>
#include <shaders/fill.frag.plane_scissor.h>
#include <shaders/fill.frag.frag_scissor.edge_aa.h>
#include <shaders/fill.frag.plane_scissor.edge_aa.h>

// TODO(performance): cache points vec in {Circle, Rect}Shape::update
// TODO: something like default font(atlas) in context instead of dummy
//   texture?

namespace rvg {

// Context
Context::Context(vpp::Device& dev, const ContextSettings& settings) :
	device_(dev), settings_(settings)
{
	constexpr auto sampleCount = vk::SampleCountBits::e1;

	// sampler
	vk::SamplerCreateInfo samplerInfo {};
	samplerInfo.magFilter = vk::Filter::linear;
	samplerInfo.minFilter = vk::Filter::linear;
	samplerInfo.minLod = 0.f;
	samplerInfo.maxLod = 0.25f;
	samplerInfo.maxAnisotropy = 1.f;
	samplerInfo.mipmapMode = vk::SamplerMipmapMode::nearest;
	samplerInfo.addressModeU = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeV = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeW = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.borderColor = vk::BorderColor::floatOpaqueWhite;
	texSampler_ = {dev, samplerInfo};

	samplerInfo.magFilter = vk::Filter::nearest;
	samplerInfo.minFilter = vk::Filter::nearest;
	fontSampler_ = {dev, samplerInfo};

	// layouts
	auto transformDSB = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex),
	};

	auto paintDSB = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex),
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &texSampler_.vkHandle()),
	};

	auto fontAtlasDSB = {
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &fontSampler_.vkHandle()),
	};

	auto clipDistance = settings.clipDistanceEnable;
	auto scissorStage = clipDistance ?
		vk::ShaderStageBits::vertex :
		vk::ShaderStageBits::fragment;

	auto scissorDSB = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer, scissorStage),
	};

	dsLayoutTransform_ = {dev, transformDSB};
	dsLayoutPaint_ = {dev, paintDSB};
	dsLayoutFontAtlas_ = {dev, fontAtlasDSB};
	dsLayoutScissor_ = {dev, scissorDSB};
	std::vector<vk::DescriptorSetLayout> layouts = {
		dsLayoutTransform_,
		dsLayoutPaint_,
		dsLayoutFontAtlas_,
		dsLayoutScissor_
	};

	if(settings.antiAliasing) {
		auto aaStrokeDSB = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		dsLayoutStrokeAA_ = {dev, aaStrokeDSB};
		layouts.push_back(dsLayoutStrokeAA_);
	}

	pipeLayout_ = {dev, layouts, {
		{vk::ShaderStageBits::fragment, 0, 4}
	}};

	// pipeline
	using ShaderData = nytl::Span<const std::uint32_t>;
	auto vertData = ShaderData(fill_vert_frag_scissor_data);
	auto fragData = ShaderData(fill_frag_frag_scissor_data);

	if(clipDistance) {
		vertData = fill_vert_plane_scissor_data;
		if(settings.antiAliasing) {
			fragData = fill_frag_plane_scissor_edge_aa_data;
		} else {
			fragData = fill_frag_plane_scissor_data;
		}
	} else if(settings.antiAliasing) {
		fragData = fill_frag_frag_scissor_edge_aa_data;
	}

	auto fillVertex = vpp::ShaderModule(dev, vertData);
	auto fillFragment = vpp::ShaderModule(dev, fragData);

	vpp::ShaderProgram fillStages({
		{fillVertex, vk::ShaderStageBits::vertex},
		{fillFragment, vk::ShaderStageBits::fragment}
	});

	vk::GraphicsPipelineCreateInfo fanPipeInfo;

	fanPipeInfo.renderPass = settings.renderPass;
	fanPipeInfo.subpass = settings.subpass;
	fanPipeInfo.layout = pipeLayout_;
	fanPipeInfo.stageCount = fillStages.vkStageInfos().size();
	fanPipeInfo.pStages = fillStages.vkStageInfos().data();
	fanPipeInfo.flags = vk::PipelineCreateBits::allowDerivatives;

	// vertex attribs: vec2 pos, vec2 uv, vec4u8 color
	std::array<vk::VertexInputAttributeDescription, 3> vertexAttribs = {};
	vertexAttribs[0].format = vk::Format::r32g32Sfloat;

	vertexAttribs[1].format = vk::Format::r32g32Sfloat;
	vertexAttribs[1].location = 1;
	vertexAttribs[1].binding = 1;

	vertexAttribs[2].format = vk::Format::r8g8b8a8Unorm;
	vertexAttribs[2].location = 2;
	vertexAttribs[2].binding = 2;

	// position and uv are in different buffers
	// this allows polygons that don't use any uv-coords to simply
	// reuse the position buffer which will result in better performance
	// (due to caching) and waste less memory
	std::array<vk::VertexInputBindingDescription, 3> vertexBindings = {};
	vertexBindings[0].inputRate = vk::VertexInputRate::vertex;
	vertexBindings[0].stride = sizeof(float) * 2; // position
	vertexBindings[0].binding = 0;

	vertexBindings[1].inputRate = vk::VertexInputRate::vertex;
	vertexBindings[1].stride = sizeof(float) * 2; // uv
	vertexBindings[1].binding = 1;

	vertexBindings[2].inputRate = vk::VertexInputRate::vertex;
	vertexBindings[2].stride = sizeof(u8) * 4; // color
	vertexBindings[2].binding = 2;

	vk::PipelineVertexInputStateCreateInfo vertexInfo {};
	vertexInfo.pVertexAttributeDescriptions = vertexAttribs.data();
	vertexInfo.vertexAttributeDescriptionCount = vertexAttribs.size();
	vertexInfo.pVertexBindingDescriptions = vertexBindings.data();
	vertexInfo.vertexBindingDescriptionCount = vertexBindings.size();
	fanPipeInfo.pVertexInputState = &vertexInfo;

	vk::PipelineInputAssemblyStateCreateInfo assemblyInfo;
	assemblyInfo.topology = vk::PrimitiveTopology::triangleFan;
	fanPipeInfo.pInputAssemblyState = &assemblyInfo;

	vk::PipelineRasterizationStateCreateInfo rasterizationInfo;
	rasterizationInfo.polygonMode = vk::PolygonMode::fill;
	rasterizationInfo.cullMode = vk::CullModeBits::none;
	rasterizationInfo.frontFace = vk::FrontFace::counterClockwise;
	rasterizationInfo.depthClampEnable = false;
	rasterizationInfo.rasterizerDiscardEnable = false;
	rasterizationInfo.depthBiasEnable = false;
	rasterizationInfo.lineWidth = 1.f;
	fanPipeInfo.pRasterizationState = &rasterizationInfo;

	vk::PipelineMultisampleStateCreateInfo multisampleInfo;
	multisampleInfo.rasterizationSamples = sampleCount;
	multisampleInfo.sampleShadingEnable = false;
	multisampleInfo.alphaToCoverageEnable = false;
	fanPipeInfo.pMultisampleState = &multisampleInfo;

	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.blendEnable = true;
	blendAttachment.alphaBlendOp = vk::BlendOp::add;
	blendAttachment.srcColorBlendFactor = vk::BlendFactor::srcAlpha;
	blendAttachment.dstColorBlendFactor = vk::BlendFactor::oneMinusSrcAlpha;
	blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::one;
	blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::zero;
	blendAttachment.colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;

	vk::PipelineColorBlendStateCreateInfo blendInfo;
	blendInfo.attachmentCount = 1;
	blendInfo.pAttachments = &blendAttachment;
	fanPipeInfo.pColorBlendState = &blendInfo;

	vk::PipelineViewportStateCreateInfo viewportInfo;
	viewportInfo.scissorCount = 1;
	viewportInfo.viewportCount = 1;
	fanPipeInfo.pViewportState = &viewportInfo;

	const auto dynStates = {
		vk::DynamicState::viewport,
		vk::DynamicState::scissor};

	vk::PipelineDynamicStateCreateInfo dynamicInfo;
	dynamicInfo.dynamicStateCount = dynStates.size();
	dynamicInfo.pDynamicStates = dynStates.begin();
	fanPipeInfo.pDynamicState = &dynamicInfo;

	// stripPipe
	auto stripPipeInfo = fanPipeInfo;
	stripPipeInfo.flags = vk::PipelineCreateBits::derivative;
	stripPipeInfo.basePipelineIndex = 0;

	auto stripAssemblyInfo = assemblyInfo;
	stripAssemblyInfo.topology = vk::PrimitiveTopology::triangleStrip;
	stripPipeInfo.pInputAssemblyState = &stripAssemblyInfo;

	auto pipes = vk::createGraphicsPipelines(dev, {},
		{fanPipeInfo, stripPipeInfo});
	fanPipe_ = {dev, pipes[0]};
	stripPipe_ = {dev, pipes[1]};

	// sync stuff
	auto family = device().queueSubmitter().queue().family();
	uploadSemaphore_ = {device()};
	uploadCmdBuf_ = device().commandAllocator().get(family,
		vk::CommandPoolCreateBits::resetCommandBuffer);

	// dummies
	constexpr std::uint8_t bytes[] = {0xFF, 0xFF, 0xFF, 0xFF};
	emptyImage_ = createTexture(dev, 1, 1,
		reinterpret_cast<const std::byte*>(bytes),
		TextureType::rgba32);

	dummyTex_ = {dsAllocator(), dsLayoutFontAtlas_};
	vpp::DescriptorSetUpdate update(dummyTex_);
	auto layout = vk::ImageLayout::general;
	update.imageSampler({{{}, emptyImage_.vkImageView(), layout}});

	identityTransform_ = {*this};
	pointColorPaint_ = {*this, ::rvg::pointColorPaint()};
	defaultScissor_ = {*this, Scissor::reset};

	if(settings.antiAliasing) {
		defaultStrokeAABuf_ = {bufferAllocator(), 12 * sizeof(float),
			vk::BufferUsageBits::uniformBuffer, 0u, device().hostMemoryTypes()};
		auto map = defaultStrokeAABuf_.memoryMap();
		auto ptr = map.ptr();
		write(ptr, 1.f);

		defaultStrokeAA_ = {dsAllocator(), dsLayoutStrokeAA_};
		auto& b = defaultStrokeAABuf_;
		vpp::DescriptorSetUpdate update(defaultStrokeAA_);
		update.uniform({{b.buffer(), b.offset(), sizeof(float)}});
	}
}

vpp::DescriptorAllocator& Context::dsAllocator() const {
	return device().descriptorAllocator();
}

vpp::BufferAllocator& Context::bufferAllocator() const {
	return device().bufferAllocator();
}

DrawInstance Context::record(vk::CommandBuffer cmdb) {
	DrawInstance ret { *this, cmdb };
	identityTransform_.bind(ret);
	defaultScissor_.bind(ret);
	vk::cmdBindDescriptorSets(cmdb, vk::PipelineBindPoint::graphics,
		pipeLayout(), fontBindSet, {dummyTex_}, {});

	if(settings().antiAliasing) {
		vk::cmdBindDescriptorSets(cmdb, vk::PipelineBindPoint::graphics,
			pipeLayout(), aaStrokeBindSet, {defaultStrokeAA_}, {});
	}

	return ret;
}

bool Context::updateDevice() {
	auto visitor = [&](auto* obj) {
		dlg_assert(obj);
		return obj->updateDevice();
	};

	for(auto& ud : updateDevice_) {
		rerecord_ |= std::visit(visitor, ud);
	}

	updateDevice_.clear();
	auto ret = rerecord_;
	rerecord_ = false;
	return ret;
}

vk::Semaphore Context::stageUpload() {
	if(!recordedUpload_) {
		return {};
	}

	vk::endCommandBuffer(uploadCmdBuf_);
	recordedUpload_ = false;

	vk::SubmitInfo info;
	info.commandBufferCount = 1;
	info.pCommandBuffers = &uploadCmdBuf_.vkHandle();
	info.pSignalSemaphores = &uploadSemaphore_.vkHandle();
	info.signalSemaphoreCount = 1u;
	device().queueSubmitter().add(info);
	return uploadSemaphore_;
}

vk::CommandBuffer Context::uploadCmdBuf() {
	if(!recordedUpload_) {
		vk::beginCommandBuffer(uploadCmdBuf_, {});
		recordedUpload_ = true;
		stages_.clear();
	}

	return uploadCmdBuf_;
}

void Context::addStage(vpp::SubBuffer&& buf) {
	if(buf.size()) {
		stages_.emplace_back(std::move(buf));
	}
}

void Context::registerUpdateDevice(DeviceObject obj) {
	updateDevice_.insert(obj);
}

bool Context::deviceObjectDestroyed(::rvg::DeviceObject& obj) noexcept {
	auto it = updateDevice_.begin();
	auto visitor = [&](auto* ud) {
		if(&obj == ud) {
			updateDevice_.erase(it);
			return true;
		}

		return false;
	};

	for(; it != updateDevice_.end(); ++it) {
		if(std::visit(visitor, *it)) {
			return true;
		}
	}

	return false;
}

void Context::deviceObjectMoved(::rvg::DeviceObject& o,
		::rvg::DeviceObject& n) noexcept {

	auto it = updateDevice_.begin();
	auto visitor = [&](auto* ud) {
		if(&o == ud) {
			updateDevice_.erase(it);
			auto i = static_cast<decltype(ud)>(&n);
			updateDevice_.insert(i);
			return true;
		}

		return false;
	};

	for(; it != updateDevice_.end(); ++it) {
		if(std::visit(visitor, *it)) {
			return;
		}
	}
}

// DeviceObject
DeviceObject::DeviceObject(DeviceObject&& rhs) noexcept {
	using std::swap;
	swap(context_, rhs.context_);

	if(context_) {
		context_->deviceObjectMoved(rhs, *this);
	}
}

DeviceObject& DeviceObject::operator=(DeviceObject&& rhs) noexcept {
	if(context_) {
		context_->deviceObjectDestroyed(*this);
	}

	if(rhs.context_) {
		rhs.context_->deviceObjectMoved(rhs, *this);
	}

	context_ = rhs.context_;
	rhs.context_ = {};
	return *this;
}

DeviceObject::~DeviceObject() {
	if(valid()) {
		context().deviceObjectDestroyed(*this);
	}
}


} // namespace rvg
