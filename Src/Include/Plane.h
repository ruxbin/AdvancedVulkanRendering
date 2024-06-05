#pragma once
#include "Matrix.h"
#include "Common.h"
#include "spdlog/spdlog.h"
#include <initializer_list>
#include <limits>
class Plane
{
private:
	union
	{
		float nx, ny, nz;
		vec3 normal;
	};
	
	float w;

public:
	Plane(Plane&& rhs)
	{
		normal = rhs.normal;
		w = rhs.w;
	}

	Plane& operator=(Plane&& rhs)
	{
		normal = rhs.normal;
		w = rhs.w;
		return *this;
	}

	Plane() { normal = vec3(); w = 0; };
	Plane(const vec3& n, float d) : w(d),normal(n) {}
	Plane(const vec3& p1, const vec3& p2, const vec3& p3)
	{
		//right hand?
		normal = normalize((p2 - p1).cross(p3 - p1));
		w = p1.dot(normal);
	}
	Plane(std::initializer_list<vec3> pts)
	{
		if (pts.size() < 3)
		{
			spdlog::error("planes have at least 3 pointes!!");
		}
		auto ite = pts.begin();
		const vec3& p1 = *ite++;
		const vec3& p2 = *ite++;
		const vec3& p3 = *ite++;
		//Plane(p1, p2, p3);
		normal = normalize((p2 - p1).cross(p3 - p1));
		w = p1.dot(normal);
	}
	bool IsInside(const AAPLBoundingBox3& aabb)const
	{
		//nearest point < w
		//vec3 cornors[8];
#undef min
		float maxD = std::numeric_limits<float>::lowest();// , minD = std::numeric_limits<float>::max;
		for (int z = 0; z < 2; z++)
		{
			for (int y = 0; y < 2; y++)
			{
				for (int x = 0; x < 2; x++)
				{
					vec3 cornor_i(x == 0 ? aabb.min.x : aabb.max.x, y == 0 ? aabb.min.y : aabb.max.y, z == 0 ? aabb.min.z: aabb.max.z);
					float d1 = cornor_i.dot(normal)-w;
					if (d1 > maxD)
						maxD = d1;
					//if (d1 < minD)
					//	minD = d1;
				}
			}
		}
		return maxD > 0;
	}
	//TODO:SIMD version
};