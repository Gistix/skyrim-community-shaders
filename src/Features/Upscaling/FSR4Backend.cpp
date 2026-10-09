#include "FSR4Backend.h"

#include "DXVKInterop.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Game.h"

#include <algorithm>
#include <cmath>

static void RecordImageBarrier(
	VkCommandBuffer cb,
	VkImage image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	VkAccessFlags srcAccess,
	VkAccessFlags dstAccess,
	VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT)
{
	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspectMask;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(
		cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier);
}

FSR4Backend* FSR4Backend::GetSingleton()
{
	static FSR4Backend singleton;
	return &singleton;
}

FSR4Backend::~FSR4Backend()
{
	Shutdown();
}

bool FSR4Backend::Initialize(
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	if (initialized &&
		renderWidth == a_renderWidth && renderHeight == a_renderHeight &&
		displayWidth == a_displayWidth && displayHeight == a_displayHeight)
		return true;

	Shutdown();

	if (a_renderWidth == 0 || a_renderHeight == 0 || a_displayWidth == 0 || a_displayHeight == 0)
		return false;

	auto* interop = D3D12Interop::GetSingleton();
	if (!interop || !interop->IsAvailable()) {
		logger::warn("[FSR 4] D3D12Interop not available");
		return false;
	}

	auto* ffxBackend = FFXD3D12Backend::GetSingleton();
	if (!ffxBackend || !ffxBackend->IsAvailable()) {
		if (!ffxBackend->Initialize(interop->GetDevice())) {
			logger::warn("[FSR 4] FidelityFX D3D12 backend failed to initialize");
			return false;
		}
	}

	// 1. Create cross-API shared textures
	constexpr D3D12_RESOURCE_FLAGS d3dFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
	                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
	                                          D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

	constexpr VkImageUsageFlags vkFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                                      VK_IMAGE_USAGE_SAMPLED_BIT |
	                                      VK_IMAGE_USAGE_STORAGE_BIT;

	bool ok = true;
	ok &= interop->CreateSharedTexture2D(a_renderWidth, a_renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedColorIn);
	ok &= interop->CreateSharedTexture2D(a_displayWidth, a_displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedColorOut);
	ok &= interop->CreateSharedTexture2D(a_renderWidth, a_renderHeight, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_D32_SFLOAT, d3dFlags, vkFlags | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, sharedDepth);
	ok &= interop->CreateSharedTexture2D(a_renderWidth, a_renderHeight, DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, d3dFlags, vkFlags, sharedMotion);

	if (!ok) {
		logger::error("[FSR 4] Failed to create shared NT textures for FSR 4 upscaler");
		Shutdown();
		return false;
	}

	renderWidth = a_renderWidth;
	renderHeight = a_renderHeight;
	displayWidth = a_displayWidth;
	displayHeight = a_displayHeight;

	// 2. Create FidelityFX Upscale context with DX12 backend
	auto* ffx = FFXD3D12Backend::GetSingleton();
	if (ffx && ffx->IsAvailable()) {
		ffxCreateBackendDX12Desc backendDesc = ffx->GetBackendDesc();

		ffxCreateContextDescUpscale upscaleDesc{};
		upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		upscaleDesc.header.pNext = &backendDesc.header;
		upscaleDesc.maxRenderSize = { a_renderWidth, a_renderHeight };
		upscaleDesc.maxUpscaleSize = { a_displayWidth, a_displayHeight };
		// Skyrim's depth is standard [0=near .. 1=far], NOT reversed-Z, so no FFX_UPSCALE_ENABLE_DEPTH_INVERTED.
		upscaleDesc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

		ffxReturnCode_t ret = ffx->GetApi().CreateContext(&upscaleContext, &upscaleDesc.header, nullptr);
		if (ret == FFX_API_RETURN_OK && upscaleContext) {
			logger::info("[FSR 4] Created FidelityFX Upscale context successfully");

			if (ffx->GetApi().Query) {
				ffxQueryGetProviderVersion verQuery{};
				verQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
				if (ffx->GetApi().Query(&upscaleContext, &verQuery.header) == FFX_API_RETURN_OK) {
					logger::info("[FSR 4] Active provider: '{}' (versionId {:#x})",
						verQuery.versionName ? verQuery.versionName : "Unknown",
						verQuery.versionId);
				}
			}
		} else {
			logger::warn("[FSR 4] ffxCreateContext for Upscale returned code {}; using blit fallback", ret);
			upscaleContext = nullptr;
		}
	}

	initialized = true;

	logger::info("[FSR 4] Successfully initialized FSR 4 upscaler (render: {}x{}, display: {}x{})",
		a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight);
	return true;
}

void FSR4Backend::Shutdown()
{
	auto* interop = D3D12Interop::GetSingleton();
	if (interop) {
		interop->DestroySharedTexture(sharedColorIn);
		interop->DestroySharedTexture(sharedColorOut);
		interop->DestroySharedTexture(sharedDepth);
		interop->DestroySharedTexture(sharedMotion);
	}

	auto* ffxBackend = FFXD3D12Backend::GetSingleton();
	if (ffxBackend && upscaleContext) {
		ffxBackend->GetApi().DestroyContext(&upscaleContext, nullptr);
		upscaleContext = nullptr;
	}

	renderWidth = 0;
	renderHeight = 0;
	displayWidth = 0;
	displayHeight = 0;
	initialized = false;
}

bool FSR4Backend::Evaluate(
	ID3D11Resource* a_colorIn,
	ID3D11Resource* a_colorOut,
	ID3D11Resource* a_depth,
	ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight,
	float a_jitterX, float a_jitterY,
	float a_sharpness)
{
	if (!a_colorIn || !a_colorOut)
		return false;

	if (!initialized ||
		a_renderWidth != renderWidth || a_renderHeight != renderHeight ||
		a_displayWidth != displayWidth || a_displayHeight != displayHeight) {
		if (!Initialize(a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight)) {
			logger::error("[FSR 4] Failed to initialize backend for {}x{} -> {}x{}",
				a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight);
			return false;
		}
	}

	auto* interop = D3D12Interop::GetSingleton();
	auto* dxvk = DXVKInterop::GetSingleton();
	if (!interop || !dxvk)
		return false;

	// 1. Copy source render-resolution inputs to shared Vulkan surfaces on the GPU with full layout barriers
	auto tx = dxvk->BeginFrameCommandBuffer();
	if (tx) {
		VkCommandBuffer cb = tx.GetCommandBuffer();

		VkImage srcColor = VK_NULL_HANDLE;
		VkImageLayout srcColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		dxvk->GetVkImage(a_colorIn, &srcColor, &srcColorLayout);

		VkImage srcDepth = VK_NULL_HANDLE;
		VkImageLayout srcDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (a_depth) dxvk->GetVkImage(a_depth, &srcDepth, &srcDepthLayout);

		VkImage srcMotion = VK_NULL_HANDLE;
		VkImageLayout srcMotionLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (a_motionVectors) dxvk->GetVkImage(a_motionVectors, &srcMotion, &srcMotionLayout);

		VkImageCopy copyRegion{};
		copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.dstSubresource.layerCount = 1;
		copyRegion.extent = { renderWidth, renderHeight, 1 };

		if (srcColor && sharedColorIn.vkImage) {
			VkImageLayout curSrc = (srcColorLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? srcColorLayout : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			RecordImageBarrier(cb, srcColor, curSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			RecordImageBarrier(cb, sharedColorIn.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT);

			vkCmdCopyImage(cb, srcColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				sharedColorIn.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			RecordImageBarrier(cb, srcColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, curSrc,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
			RecordImageBarrier(cb, sharedColorIn.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		}

		if (srcDepth && sharedDepth.vkImage) {
			VkImageCopy depthCopyRegion = copyRegion;
			depthCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			depthCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			VkImageLayout curDepth = (srcDepthLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? srcDepthLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			RecordImageBarrier(cb, srcDepth, curDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
			RecordImageBarrier(cb, sharedDepth.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

			vkCmdCopyImage(cb, srcDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				sharedDepth.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopyRegion);

			RecordImageBarrier(cb, srcDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, curDepth,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
			RecordImageBarrier(cb, sharedDepth.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
		}

		if (srcMotion && sharedMotion.vkImage) {
			VkImageLayout curMotion = (srcMotionLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? srcMotionLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			RecordImageBarrier(cb, srcMotion, curMotion, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			RecordImageBarrier(cb, sharedMotion.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT);

			vkCmdCopyImage(cb, srcMotion, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				sharedMotion.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			RecordImageBarrier(cb, srcMotion, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, curMotion,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
			RecordImageBarrier(cb, sharedMotion.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		}
	}

	// 2. Advance GPU timeline fence:
	//    val + 3: Vulkan signals -> D3D12 waits
	//    val + 4: D3D12 signals -> Vulkan waits
	const uint64_t baseFence = interop->AdvanceTimelineFrame();
	const uint64_t vkSignalVal = baseFence + 3;
	const uint64_t d3dSignalVal = baseFence + 4;

	if (tx) {
		dxvk->SubmitFrameCommandBuffer(tx, interop->GetMainSharedFence().vkTimelineSemaphore, vkSignalVal);
	}
	interop->D3D12QueueWait(interop->GetMainSharedFence(), vkSignalVal);

	// 3. Record & dispatch D3D12 FSR 4 upscaling work
	const uint32_t slot = 1;
	auto* cmdList = interop->BeginCommandList(slot);
	if (cmdList) {
		// Transition D3D12 resources from COMMON to active execution states
		D3D12_RESOURCE_BARRIER preBarriers[4]{};
		preBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		preBarriers[0].Transition.pResource = sharedColorIn.d3dResource.get();
		preBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		preBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		preBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		preBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		preBarriers[1].Transition.pResource = sharedDepth.d3dResource.get();
		preBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		preBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		preBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		preBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		preBarriers[2].Transition.pResource = sharedMotion.d3dResource.get();
		preBarriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		preBarriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		preBarriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		preBarriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		preBarriers[3].Transition.pResource = sharedColorOut.d3dResource.get();
		preBarriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		preBarriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		preBarriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		cmdList->ResourceBarrier(4, preBarriers);

		if (upscaleContext) {
			auto* ffx = FFXD3D12Backend::GetSingleton();

			LARGE_INTEGER qpcNow;
			QueryPerformanceCounter(&qpcNow);
			float deltaMs = 16.666f;
			static int64_t s_lastQpc = 0;
			if (s_lastQpc > 0) {
				LARGE_INTEGER qpcFreq;
				QueryPerformanceFrequency(&qpcFreq);
				const double ms = double(qpcNow.QuadPart - s_lastQpc) * 1000.0 / double(qpcFreq.QuadPart);
				deltaMs = static_cast<float>(std::clamp(ms, 0.1, 100.0));
			}
			s_lastQpc = qpcNow.QuadPart;

			float camNear = 0.1f;
			float camFar = 10000.0f;
			if (globals::game::cameraNear && *globals::game::cameraNear > 0.0f) {
				camNear = *globals::game::cameraNear;
			}
			if (globals::game::cameraFar && *globals::game::cameraFar > 0.0f) {
				camFar = *globals::game::cameraFar;
			}
			float camFov = Util::GetVerticalFOVRad();
			if (camFov <= 0.0f || !std::isfinite(camFov)) {
				camFov = 0.785398f;
			}

			ffxDispatchDescUpscale dispatchDesc{};
			dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
			dispatchDesc.commandList = cmdList;
			dispatchDesc.color = FFXD3D12Backend::CreateResource(
				sharedColorIn, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.output = FFXD3D12Backend::CreateResource(
				sharedColorOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			dispatchDesc.depth = FFXD3D12Backend::CreateResource(
				sharedDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_DEPTHTARGET | FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.motionVectors = FFXD3D12Backend::CreateResource(
				sharedMotion, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.jitterOffset = { -a_jitterX, -a_jitterY };
			dispatchDesc.motionVectorScale = { static_cast<float>(renderWidth), static_cast<float>(renderHeight) };
			dispatchDesc.renderSize = { renderWidth, renderHeight };
			dispatchDesc.upscaleSize = { displayWidth, displayHeight };
			dispatchDesc.enableSharpening = (a_sharpness > 0.0f);
			dispatchDesc.sharpness = a_sharpness;
			dispatchDesc.frameTimeDelta = deltaMs;
			dispatchDesc.preExposure = 1.0f;
			dispatchDesc.reset = globals::state ? globals::state->ShouldResetHistory() : false;
			dispatchDesc.cameraNear = camNear;
			dispatchDesc.cameraFar = camFar;
			dispatchDesc.cameraFovAngleVertical = camFov;
			dispatchDesc.viewSpaceToMetersFactor = 0.01428222656f;

			ffxReturnCode_t ret = ffx->GetApi().Dispatch(&upscaleContext, &dispatchDesc.header);
			if (ret != FFX_API_RETURN_OK) {
				static ffxReturnCode_t lastRet = FFX_API_RETURN_OK;
				if (ret != lastRet) {
					logger::error("[FSR 4] ffxDispatch failed with code {}", ret);
					lastRet = ret;
				}
			}
		} else {
			// D3D12 fallback blit
			cmdList->CopyResource(sharedColorOut.d3dResource.get(), sharedColorIn.d3dResource.get());
		}

		// Transition D3D12 resources back to COMMON state for cross-API sharing
		D3D12_RESOURCE_BARRIER postBarriers[4]{};
		postBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		postBarriers[0].Transition.pResource = sharedColorOut.d3dResource.get();
		postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		postBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		postBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		postBarriers[1].Transition.pResource = sharedColorIn.d3dResource.get();
		postBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		postBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		postBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		postBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		postBarriers[2].Transition.pResource = sharedDepth.d3dResource.get();
		postBarriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		postBarriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		postBarriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		postBarriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		postBarriers[3].Transition.pResource = sharedMotion.d3dResource.get();
		postBarriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		postBarriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		postBarriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		cmdList->ResourceBarrier(4, postBarriers);

		interop->ExecuteCommandList(slot);
	}

	interop->D3D12QueueSignal(interop->GetMainSharedFence(), d3dSignalVal);
	interop->VulkanWaitFence(interop->GetMainSharedFence(), d3dSignalVal);

	// 4. Copy high-resolution output back to game display surface with full layout barriers
	auto txOut = dxvk->BeginFrameCommandBuffer();
	if (txOut) {
		VkCommandBuffer cb = txOut.GetCommandBuffer();
		VkImage dstColor = VK_NULL_HANDLE;
		VkImageLayout dstColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		dxvk->GetVkImage(a_colorOut, &dstColor, &dstColorLayout);

		if (dstColor && sharedColorOut.vkImage) {
			VkImageCopy copyRegion{};
			copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.srcSubresource.layerCount = 1;
			copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.dstSubresource.layerCount = 1;
			copyRegion.extent = { displayWidth, displayHeight, 1 };

			VkImageLayout curDst = (dstColorLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? dstColorLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			RecordImageBarrier(cb, sharedColorOut.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			RecordImageBarrier(cb, dstColor, curDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT);

			vkCmdCopyImage(cb, sharedColorOut.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			RecordImageBarrier(cb, dstColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, curDst,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
			RecordImageBarrier(cb, sharedColorOut.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		}
		dxvk->SubmitFrameCommandBuffer(txOut);
	}

	return true;
}
