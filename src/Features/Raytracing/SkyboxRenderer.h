#pragma once

#include "PCH.h"

#include "Features/Raytracing/Buffer.h"
#include "Features/Raytracing/Heap.h"
#include "Features/Raytracing/HeapManager.h"
#include "Features/Raytracing/Pipeline.h"
#include "Features/Raytracing/Utils.h"
#include <d3d11_4.h>
#include <d3d12.h>

#include "Features/Raytracing/Types.h"

#include "Raytracing/Denoiser/SVGF/SVGF.hlsli"

#include "Buffer.h"

#include "Features/Upscaling/DX12SwapChain.h"

struct SkyboxRenderer
{
	eastl::unique_ptr<Texture2D> temporalTexture = nullptr;
	eastl::unique_ptr<Texture2D> momentsTexture = nullptr;
	eastl::unique_ptr<Texture2D> varianceTexture = nullptr;
	eastl::unique_ptr<Texture2D> historyMomentsTexture = nullptr;
	eastl::unique_ptr<Texture2D> historyNormalsTexture = nullptr;
	eastl::unique_ptr<Texture2D> historyTexture = nullptr;

	eastl::unique_ptr<SVGF> frameData = nullptr;
	eastl::unique_ptr<ConstantBuffer> frameBuffer = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> temporalCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> varianceCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> spatialDiffuseCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> spatialSpecularCS = nullptr;

	RE::NiPointer<RE::NiCamera> camera;

	void Initialize();
	void Render(WrappedResource* skyHemisphere) const;
};