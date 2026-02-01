#pragma once

#include <directxpackedvector.h>

#define FMT_STRUCT(name, ...)                                                        \
	template <>                                                                      \
	struct fmt::formatter<name>                                                      \
	{                                                                                \
		constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); } \
		template <typename FormatContext>                                            \
		auto format(const name& s, FormatContext& ctx) const                         \
		{                                                                            \
			return fmt::format_to(ctx.out(), "{}", s.to_string());                   \
		}                                                                            \
	};

struct half
{
	DirectX::PackedVector::HALF v;

	half() = default;
	half(const half&) = default;
	half& operator=(const half&) = default;

	half(const float& fv)
	{
		v = DirectX::PackedVector::XMConvertFloatToHalf(fv);
	}

	operator float() const
	{
		return DirectX::PackedVector::XMConvertHalfToFloat(v);
	}

	half& operator+=(const half& rhs)
	{
		v = DirectX::PackedVector::XMConvertFloatToHalf(float(*this) + float(rhs));
		return *this;
	}

	half& operator-=(const half& rhs)
	{
		v = DirectX::PackedVector::XMConvertFloatToHalf(float(*this) - float(rhs));
		return *this;
	}

	half& operator*=(const half& rhs)
	{
		v = DirectX::PackedVector::XMConvertFloatToHalf(float(*this) * float(rhs));
		return *this;
	}

	half& operator/=(const half& rhs)
	{
		v = DirectX::PackedVector::XMConvertFloatToHalf(float(*this) / float(rhs));
		return *this;
	}
};
static_assert(sizeof(half) == 2);

struct half2
{
	half x;
	half y;

	half2() = default;

	constexpr half2(half _x, half _y) :
		x(_x), y(_y) {}

	half2(float _x, float _y) :
		x(_x), y(_y) {}

	half2(const float2& v) :
		x(v.x), y(v.y) {}

	operator float2() const
	{
		return float2(
			static_cast<float>(x),
			static_cast<float>(y));
	}

	std::string to_string() const
	{
		return "[" + std::to_string(x) + ", " + std::to_string(y) + "]";
	}
};
static_assert(sizeof(half2) == 4);

FMT_STRUCT(half2, s.x, s.y)

struct half3
{
	half x;
	half y;
	half z;

	half3() = default;

	constexpr half3(half _x, half _y, half _z) :
		x(_x), y(_y), z(_z) {}

	half3(float _x, float _y, float _z) :
		x(_x), y(_y), z(_z) {}

	half3(const float3& v) :
		x(v.x), y(v.y), z(v.z) {}

	operator float3() const
	{
		return float3(
			static_cast<float>(x),
			static_cast<float>(y),
			static_cast<float>(z));
	}

	half3& operator+=(const half3& rhs)
	{
		x += rhs.x;
		y += rhs.y;
		z += rhs.z;
		return *this;
	}

	std::string to_string() const
	{
		return "[" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + "]";
	}
};
static_assert(sizeof(half3) == 6);

FMT_STRUCT(half3, s.x, s.y, s.z)

struct half4
{
	half x;
	half y;
	half z;
	half w;

	half4() = default;

	constexpr half4(half _x, half _y, half _z, half _w) :
		x(_x), y(_y), z(_z), w(_w) {}

	half4(float _x, float _y, float _z, float _w) :
		x(_x), y(_y), z(_z), w(_w) {}

	half4(const float4& v) :
		x(v.x), y(v.y), z(v.z), w(v.w) {}

	operator float4() const
	{
		return float4(
			static_cast<float>(x),
			static_cast<float>(y),
			static_cast<float>(z),
			static_cast<float>(w));
	}

	std::string to_string() const
	{
		return "[" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ", " + std::to_string(w) + "]";
	}
};
static_assert(sizeof(half4) == 8);

FMT_STRUCT(half4, s.x, s.y, s.z, s.w)

struct uint2
{
	uint x;
	uint y;

	bool operator==(const uint2&) const = default;
	bool operator!=(const uint2&) const = default;
};
static_assert(sizeof(uint2) == 8);

struct uint3
{
	uint x;
	uint y;
	uint z;
};
static_assert(sizeof(uint3) == 12);

struct uint4
{
	uint x;
	uint y;
	uint z;
	uint w;
};
static_assert(sizeof(uint4) == 16);

typedef half4 float16_t4;

struct AABB
{
	float3 center;
	float3 size;
	float3 extents;
	float3 min;
	float3 max;

	AABB() = default;

	AABB(float3 center, float3 size) :
		center(center), size(size)
	{
		extents = size * 0.5f;
		min = center - extents;
		max = center + extents;
	}

	AABB(float3 center, float3 size, float3 extents, float3 min, float3 max) :
		center(center), size(size), extents(extents), min(min), max(max) {};

	static AABB FromMinMax(float3 min, float3 max)
	{
		const float3 size = max - min;
		return AABB((min + max) * 0.5f, size, size * 0.5f, min, max);
	}

	float Overlap(const AABB& b) const
	{
		const float3 overlapMin = float3::Max(min, b.min);
		const float3 overlapMax = float3::Min(max, b.max);

		const float3 overlapSize = float3::Max(overlapMax - overlapMin, float3::Zero);

		const float overlapVolume = overlapSize.x * overlapSize.y * overlapSize.z;

		const float volumeA = size.x * size.y * size.z;

		const float volumeB = b.size.x * b.size.y * b.size.z;

		const float minVolume = std::min(volumeA, volumeB);

		if (minVolume <= 0.0f)
			return 0.0f;

		return std::clamp(overlapVolume / minVolume, 0.0f, 1.0f);
	}

    EA_FORCE_INLINE bool Intersects(const AABB& b) const
	{
		return !(max.x < b.min.x || min.x > b.max.x ||
				 max.y < b.min.y || min.y > b.max.y ||
				 max.z < b.min.z || min.z > b.max.z);
	}

	EA_FORCE_INLINE bool IntersectsSIMD(const AABB& b) const
	{
		// Load mins and maxs (xyz + padding)
		__m128 minA = _mm_set_ps(0.0f, min.z, min.y, min.x);
		__m128 maxA = _mm_set_ps(0.0f, max.z, max.y, max.x);
		__m128 minB = _mm_set_ps(0.0f, b.min.z, b.min.y, b.min.x);
		__m128 maxB = _mm_set_ps(0.0f, b.max.z, b.max.y, b.max.x);

		// Compare
		__m128 cmp1 = _mm_cmple_ps(minA, maxB);
		__m128 cmp2 = _mm_cmple_ps(minB, maxA);

		// AND results
		__m128 overlap = _mm_and_ps(cmp1, cmp2);

		// Check xyz lanes
		return (_mm_movemask_ps(overlap) & 0x7) == 0x7;
	}

	static AABB Merge(const AABB& a, const AABB& b)
	{
		float3 mergedMin = float3::Min(a.min, b.min);
		float3 mergedMax = float3::Max(a.max, b.max);
		return FromMinMax(mergedMin, mergedMax);
	}
};