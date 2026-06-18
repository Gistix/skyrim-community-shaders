#pragma once

#include <d3d11on12.h>

struct D3D11On12SwapChainState
{
	struct Resource
	{
		winrt::com_ptr<ID3D12Resource> native;
		winrt::com_ptr<ID3D11Resource> wrapped;
	};

	static inline winrt::com_ptr<ID3D11On12Device1> d3d11On12Device;
	static inline Resource wrappedBuffers[3];
	static inline eastl::hash_set<ID3D11Resource*> acquiredResources;

	static inline eastl::vector<ID3D11Resource*> tempResources;

	static inline UINT bufferCount = 0;
	static inline bool installed = false;

	static bool Acquired(ID3D11Resource* resource)
	{
		if (!resource)
			return false;

		return acquiredResources.find(resource) != acquiredResources.end();
	}

	static void Acquire(ID3D11Resource* resource)
	{
		if (!resource)
			return;

		auto [it, emplaced] = acquiredResources.emplace(resource);
		if (!emplaced)
			return;

		logger::info("D3D11On12SwapChainState::Acquire - {}", fmt::ptr(resource));

		d3d11On12Device->AcquireWrappedResources(&resource, 1);
	}

	static void Acquire(ID3D11View* view)
	{
		if (!view)
			return;

		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);

		Acquire(resource);

		if (resource)
			resource->Release();
	}

	static void Acquire(UINT numViews, ID3D11RenderTargetView* const* views)
	{
		if (!views || numViews == 0)
			return;

		for (size_t i = 0; i < numViews; i++) {
			Acquire(views[i]);
		}
	}

	static void ReleaseAll()
	{
		const auto numResources = acquiredResources.size();

		if (numResources == 0)
			return;

		tempResources.clear();
		tempResources.reserve(numResources);

		for (const auto& resource : acquiredResources) {
			tempResources.push_back(resource);
		}

		d3d11On12Device->ReleaseWrappedResources(tempResources.data(), (UINT)numResources);

		acquiredResources.clear();

		logger::info("D3D11On12SwapChainState::ReleaseAll - {}", numResources);
	}
};