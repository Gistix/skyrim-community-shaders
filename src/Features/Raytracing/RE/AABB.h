#pragma once

#include "PCH.h"

namespace RE
{
	struct AABB
	{
		static BSFixedString DataName()
		{
			return BSFixedString("BBX");
		}

		std::uint8_t pad[0x18];
		NiPoint3 center;
		NiPoint3 extents;
	};
	static_assert(sizeof(AABB) == 0x30);
}