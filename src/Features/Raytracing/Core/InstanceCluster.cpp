#include "InstanceCluster.h"

#include "Features/Raytracing.h"
#include "Features/Raytracing/Heap.h"

eastl::unordered_map<eastl::string, eastl::unique_ptr<Model>>& InstanceCluster::Models() const
{
	return globals::features::raytracing.models;
}

eastl::unordered_map<RE::NiAVObject*, Instance>& InstanceCluster::Instances() const
{
	return globals::features::raytracing.instances;
}

eastl::vector<D3D12_RAYTRACING_GEOMETRY_DESC> InstanceCluster::GeometryDescs(uint& updateIndex)
{
	eastl::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;

	auto& rt = globals::features::raytracing;

	for (auto& root : instances) {
		// There are a lot of layers..
		auto instanceIt = rt.instances.find(root);
		if (instanceIt == rt.instances.end())
			continue;

		auto& instance = instanceIt->second;

		auto world = GetXMFromNiTransform(instance.root->world);

		auto modelIt = rt.models.find(instance.filename);
		if (modelIt == rt.models.end())
			continue;

		for (auto& shape : modelIt->second->shapes) {
			auto offset = sizeof(float3x4) * static_cast<uint64_t>(updateIndex);

			float3x4 transform;
			XMStoreFloat3x4(&transform, XMMatrixMultiply(XMLoadFloat3x4(&shape->transform) ,world));

			rt.transformBuffer->UpdateAt(&transform, updateIndex);
			rt.transformBuffer->UploadRegion(rt.commandList.get(), sizeof(float3x4), offset);
			D3D12_GPU_VIRTUAL_ADDRESS transformVirtualAddress = rt.transformBuffer->resource->GetGPUVirtualAddress() + offset;

			geometryDescs.push_back(shape->GeometryDesc(transformVirtualAddress));

			logger::info("[RT] GeometryDescs - {}", updateIndex);

			updateIndex++;
		}
	}

	return geometryDescs;
}

void InstanceCluster::Commit(ID3D12GraphicsCommandList4* commandList, uint& updateIndex)
{
	auto& rt = globals::features::raytracing;

	auto geometryDescs = GeometryDescs(updateIndex);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
		.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
		.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
		.NumDescs = static_cast<uint>(geometryDescs.size()),
		.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
		.pGeometryDescs = geometryDescs.data()
	};

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
	rt.d3d12Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

	D3D12_RESOURCE_DESC desc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = prebuildInfo.ScratchDataSizeInBytes,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.SampleDesc = Raytracing::NO_AA,
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
	};

	auto blasScratchDesc = Raytracing::DEFAULT_HEAP_MA;
	blasScratchDesc.CustomPool = rt.blasScratchPool.get();

	DX::ThrowIfFailed(rt.allocator->CreateResource(&blasScratchDesc, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, scratch.put(), IID_NULL, NULL));
	scratch->SetName(L"BLAS Scratch Resource");

	auto blasDesc = Raytracing::DEFAULT_HEAP_MA;
	blasDesc.CustomPool = rt.blasPool.get();

	desc.Width = prebuildInfo.ResultDataMaxSizeInBytes;
	DX::ThrowIfFailed(rt.allocator->CreateResource(&blasDesc, &desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, blas.put(), IID_NULL, NULL));
	blas->SetName(L"BLAS Resource");

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
		.DestAccelerationStructureData = blas->GetResource()->GetGPUVirtualAddress(),
		.Inputs = inputs,
		.ScratchAccelerationStructureData = scratch->GetResource()->GetGPUVirtualAddress()
	};

	commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	rt.destASFrame.emplace(blas->GetResource()->GetGPUVirtualAddress());

	const auto& asBarrier = CD3DX12_RESOURCE_BARRIER::UAV(blas->GetResource());
	commandList->ResourceBarrier(1, &asBarrier);
}

void InstanceCluster::Update(ID3D12GraphicsCommandList4* commandList, uint& updateIndex)
{
	auto gpuVirtualAddr = blas->GetResource()->GetGPUVirtualAddress();

	auto& rt = globals::features::raytracing;

	if (rt.destASFrame.find(gpuVirtualAddr) != rt.destASFrame.end())
		return;

	auto geometryDescs = GeometryDescs(updateIndex);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
		.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
		.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE,
		.NumDescs = static_cast<uint>(geometryDescs.size()),
		.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
		.pGeometryDescs = geometryDescs.data()
	};

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
		.DestAccelerationStructureData = gpuVirtualAddr,
		.Inputs = inputs,
		.SourceAccelerationStructureData = gpuVirtualAddr,
		.ScratchAccelerationStructureData = scratch->GetResource()->GetGPUVirtualAddress()
	};

	commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	rt.destASFrame.emplace(blas->GetResource()->GetGPUVirtualAddress());
}