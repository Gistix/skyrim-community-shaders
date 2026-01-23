#pragma once

#include "PCH.h"

struct UpdateDismemberPartionEvent
{
	RE::BSDismemberSkinInstance* instance;
	std::uint16_t a_slot;
	bool enable;
};