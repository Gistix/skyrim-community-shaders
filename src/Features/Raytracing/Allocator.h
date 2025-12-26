#pragma once

#include <D3D12MemAlloc.h>

class Allocator;

class Allocation
{
	D3D12MA::VirtualAllocation alloc;
	uint16_t index;
	Allocator* allocator;

public:
	Allocation(D3D12MA::VirtualAllocation alloc, uint16_t slot, Allocator* allocator) :
		alloc(alloc), index(slot), allocator(allocator) {}

    Allocation(const Allocation&) = delete;
	Allocation& operator=(const Allocation&) = delete;

	Allocation(Allocation&&) = default;
	Allocation& operator=(Allocation&&) = default;

	uint16_t GetIndex() const
	{
		return index;
	}

	void FreeAllocation() const;
};

class Allocator
{
public:
	eastl::string name;
	winrt::com_ptr<D3D12MA::VirtualBlock> block;

	explicit Allocator(uint16_t maxSlots, const char* name = nullptr) :
		slots(maxSlots), name(name)
	{
		D3D12MA::VIRTUAL_BLOCK_DESC blockDesc{};
		blockDesc.Size = maxSlots;

		CreateVirtualBlock(&blockDesc, block.put());
	}

	// Allocate the lowest available slot
	Allocation* Allocate()
	{
		D3D12MA::VIRTUAL_ALLOCATION_DESC allocDesc = {};
		allocDesc.Size = 1;

		D3D12MA::VirtualAllocation alloc;
		UINT64 allocOffset;

		block->Allocate(&allocDesc, &alloc, &allocOffset);

		return new Allocation(alloc, static_cast<uint16_t>(allocOffset), this);
	}

	// Free a slot
	void Free(D3D12MA::VirtualAllocation alloc) const
	{
		block->FreeAllocation(alloc);
	}

	uint16_t UsedCount() const
	{
		D3D12MA::Statistics outStats;
		block->GetStatistics(&outStats);

		return static_cast<uint16_t>(outStats.AllocationCount);
	}

private:
	uint16_t slots;
};

struct AllocationDeleter
{
	void operator()(Allocation* a) const noexcept
	{
		a->FreeAllocation();
		delete a;
	}
};