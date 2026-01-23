#pragma once

#include "PCH.h"

#include "Features/Raytracing/Core/Shape.h"

struct BSDismemberSkinInstanceRT : public RE::BSDismemberSkinInstance
{
	eastl::shared_ptr<Shape> shape;
};
