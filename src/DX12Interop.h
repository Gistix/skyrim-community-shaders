#pragma once

#include "Feature.h"
#include "State.h"
#include "DX12Interop/WrappedResource.h"

#include <winrt/base.h>

#include <d3d11_4.h>
#include <directx/d3dx12.h>
#include <type_traits>
#include <utility>

struct DX12Interop
{
	static constexpr UINT kMaxFramesInFlight = 2;

	winrt::com_ptr<ID3D12Device5> d3d12Device;

	winrt::com_ptr<ID3D12CommandQueue> commandQueue;

	UINT64 currentFenceValue = 0;
	HANDLE fenceEvent = nullptr;

	struct SharedResources
	{
		WrappedResource* main = nullptr;
		WrappedResource* depth = nullptr;
		WrappedResource* motionVector = nullptr;
		WrappedResource* reactiveMask = nullptr;
	} sharedResources;

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;

	static DX12Interop* GetSingleton()
	{
		static DX12Interop singleton;
		return &singleton;
	}

	~DX12Interop();

	void Init(ID3D11Device* d3d11Device, ID3D11DeviceContext* a_immediateContext, IDXGIAdapter* a_adapter);

	// Resources
	void SetupResources();

	bool Active() const;

	// Whether DirectX 12 is required or not
	// True when Upscaling is loaded in frame generation mode
	static bool D3D12Mode();

	UINT GetFrameContextIndex() const;

private:
	bool active = false;

	void CreateInterop();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);
	void CreateD3D12Device(IDXGIAdapter* a_adapter);
};