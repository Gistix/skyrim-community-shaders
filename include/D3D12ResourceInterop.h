#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <winrt/base.h>

namespace D3D12ResourceInterop
{
	inline winrt::com_ptr<ID3D12Resource> GetD3D12Resource(
		ID3D11Resource* a_resource,
		UINT64* a_suballocationOffset = nullptr,
		D3D12_GPU_VIRTUAL_ADDRESS* a_gpuVA = nullptr)
	{
		if (!a_resource)
			return nullptr;

		winrt::com_ptr<ID3D12Resource> d12Res;
		if (SUCCEEDED(a_resource->QueryInterface(IID_PPV_ARGS(d12Res.put())))) {
			if (a_gpuVA) {
				*a_gpuVA = d12Res->GetGPUVirtualAddress();
			}
			if (a_suballocationOffset) {
				*a_suballocationOffset = 0;
			}
			winrt::com_ptr<ID3D12Device> dev;
			if (SUCCEEDED(d12Res->GetDevice(IID_PPV_ARGS(dev.put())))) {
				ID3D12Pageable* pageable = d12Res.get();
				dev->MakeResident(1, &pageable);
			}
			return d12Res;
		}

		return nullptr;
	}

	inline winrt::com_ptr<ID3D12Device5> GetD3D12Device(ID3D11Device* a_device)
	{
		if (!a_device)
			return nullptr;

		// DX12Rim / D3D11On12 hook handles __uuidof(ID3D12Device); query that first, then QI for ID3D12Device5
		winrt::com_ptr<ID3D12Device> baseDev;
		if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(baseDev.put())))) {
			winrt::com_ptr<ID3D12Device5> d12Dev5;
			if (SUCCEEDED(baseDev->QueryInterface(IID_PPV_ARGS(d12Dev5.put())))) {
				return d12Dev5;
			}
		}

		return nullptr;
	}

	inline winrt::com_ptr<ID3D12CommandQueue> GetD3D12CommandQueue(ID3D11Device* a_device)
	{
		if (!a_device)
			return nullptr;

		winrt::com_ptr<ID3D12CommandQueue> d12Queue;
		if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(d12Queue.put())))) {
			return d12Queue;
		}

		return nullptr;
	}
}
