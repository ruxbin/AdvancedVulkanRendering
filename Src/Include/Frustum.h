#pragma once
#include "Common.h"
#include "Plane.h"
#include "spdlog/spdlog.h"
#include <initializer_list>

class Frustum
{
private:
	static const int NUM_PLANES = 6;
	Plane borders[NUM_PLANES];
	

public:
	bool FructumCull(const AAPLBoundingBox3& aabb) const
	{
		for (int i = 0; i < NUM_PLANES; i++)
		{
			if (!borders[i].IsInside(aabb))
				return true;
		}
		return false;
	}

	Frustum(const std::initializer_list<std::initializer_list<vec3>>& borderPts)
	{
		if (borderPts.size() != 6)
			spdlog::error("Frustum doesn't have 6 borders!!");

		//for (int i = 0; i < NUM_PLANES; i++)
		//{
		//	borders[i] = Plane(borderPts[i]);
		//}
		int i = 0;
		for (auto initlist : borderPts)
		{
			borders[i++] = Plane(initlist);
		}
	}

	Frustum() = default;
};