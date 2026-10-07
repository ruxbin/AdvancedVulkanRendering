#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <unordered_map>
#include <vector>

// CPU-only filtering for the realtime point-light export policy. Input geometry
// is validated by the caller. Keep source indices/UV seams in the output; weld
// exact positions only for connectivity and shell tests.
namespace pbrt_export {
inline std::vector<uint32_t> RemovePointLightShells(
    const vec3* vertices, const std::vector<uint32_t>& indices,
    const std::vector<vec3>& lights, size_t& removed,
    std::vector<bool>* omittedTriangles = nullptr) {
    const size_t triangles = indices.size() / 3;
    if (omittedTriangles) omittedTriangles->assign(triangles, false);
    std::vector<size_t> parent(triangles);
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto root = [&](size_t t) {
        while (parent[t] != t) { parent[t] = parent[parent[t]]; t = parent[t]; }
        return t;
    };
    std::map<std::array<float, 3>, uint32_t> welded;
    std::vector<uint32_t> ids;
    ids.reserve(indices.size());
    for (size_t k = 0; k < indices.size(); ++k) {
        const auto& v = vertices[indices[k]];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            return indices;
        auto [it, inserted] = welded.emplace(std::array<float,3>{v.x,v.y,v.z},
                                             static_cast<uint32_t>(welded.size()));
        ids.push_back(it->second);
    }
    auto edge = [](uint32_t a, uint32_t b) {
        return (uint64_t((std::min)(a,b)) << 32) | (std::max)(a,b);
    };
    std::unordered_map<uint64_t, unsigned> edges;
    std::unordered_map<uint64_t, size_t> firstTriangle;
    std::map<size_t, std::vector<size_t>> components;
    for (size_t t = 0; t < triangles; ++t) {
        for (int j = 0; j < 3; ++j) {
            const auto key = edge(ids[3*t+j],ids[3*t+(j+1)%3]);
            ++edges[key];
            auto [it, inserted] = firstTriangle.emplace(key, t);
            if (!inserted) parent[root(t)] = root(it->second);
        }
    }
    // Sharing a single vertex is not a surface connection: do not absorb
    // decorative sheets that only touch a bulb at one point.
    for (size_t t = 0; t < triangles; ++t) components[root(t)].push_back(t);
    std::vector<bool> omit(triangles, false);
    for (const auto& [id, faces] : components) {
        std::array<double,3> lo{INFINITY,INFINITY,INFINITY}, hi{-INFINITY,-INFINITY,-INFINITY};
        bool manifold = true;
        for (size_t t : faces) {
            for (int j = 0; j < 3; ++j) {
                uint32_t a=ids[3*t+j], b=ids[3*t+(j+1)%3];
                if (a==b || edges[edge(a,b)]>2) manifold=false;
                const auto& v=vertices[indices[3*t+j]];
                const double p[3]={v.x,v.y,v.z};
                for (int c=0;c<3;++c) { lo[c]=(std::min)(lo[c],p[c]); hi[c]=(std::max)(hi[c],p[c]); }
            }
        }
        if (!manifold) continue;
        for (const auto& light : lights) {
            const double p[3]={light.x,light.y,light.z};
            bool insideBox=true;
            for (int c=0;c<3;++c) if (!(p[c]>lo[c] && p[c]<hi[c])) insideBox=false;
            if (!insideBox) continue;
            // Bistro bulbs have a small open neck. Require the emissive shell
            // to cover >=90% of the directions around the light, rather than
            // demanding a watertight mesh. Planar signs and open shades fail
            // this test. AABB membership alone is not evidence of enclosure.
            double angle=0;
            for (size_t t : faces) {
                double a[3][3], len[3];
                for (int j=0;j<3;++j) {
                    const auto& v=vertices[indices[3*t+j]];
                    a[j][0]=v.x-p[0]; a[j][1]=v.y-p[1]; a[j][2]=v.z-p[2];
                    len[j]=std::sqrt(a[j][0]*a[j][0]+a[j][1]*a[j][1]+a[j][2]*a[j][2]);
                }
                auto dot=[&](int x,int y) { return a[x][0]*a[y][0]+a[x][1]*a[y][1]+a[x][2]*a[y][2]; };
                double det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])
                          -a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])
                          +a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
                angle+=2*std::atan2(det,len[0]*len[1]*len[2]+dot(0,1)*len[2]+dot(1,2)*len[0]+dot(2,0)*len[1]);
            }
            const double coverage=std::abs(angle)/(4*3.14159265358979323846);
            if (coverage>=0.9 && coverage<=1.00001) {
                for (size_t t : faces) omit[t]=true;
                ++removed;
                break;
            }
        }
    }
    std::vector<uint32_t> kept;
    kept.reserve(indices.size());
    for (size_t t=0;t<triangles;++t)
        if (!omit[t]) kept.insert(kept.end(),indices.begin()+3*t,indices.begin()+3*t+3);
    if (omittedTriangles) *omittedTriangles = std::move(omit);
    return kept;
}
} // namespace pbrt_export
