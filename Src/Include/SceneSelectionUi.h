#pragma once
#include "SceneSelection.h"
#include "Camera.h"
#include "imgui.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>

namespace scene_selection {
template<class Chunk,class Mesh>
void DrawPanel(Controller& selection,const Chunk* chunks,const Mesh* meshes,bool supported=true) {
  ImGui::SeparatorText("Selection");
  ImGui::TextDisabled("Left click: select | drag: camera | Esc: clear");
  if(!supported) ImGui::TextDisabled("Selection unavailable in ray tracing / decal mode");
  if(!selection.error.empty()) ImGui::TextWrapped("Selection error: %s",selection.error.c_str());
  if(!selection.ready) { ImGui::TextDisabled("Scene ownership mapping unavailable"); return; }
  ImGui::BeginDisabled(!supported);
  if(ImGui::Checkbox("Pick transparent surfaces",&selection.pickTransparent)) selection.Cancel();
  ImGui::Checkbox("Show selection bounds",&selection.showBounds);
  if(ImGui::Button("Clear Selection")) selection.Clear();
  ImGui::EndDisabled();
  if(selection.waiting) ImGui::TextUnformatted("Pending (previous selection retained)");
  if(selection.selected.clusterId==InvalidId || !chunks || !meshes) {
    ImGui::TextUnformatted("None selected"); return;
  }
  const auto& chunk=chunks[selection.selected.clusterId];
  const auto& mesh=meshes[selection.selected.meshId];
  ImGui::Text("Cluster: %u  (local %u)",selection.selected.clusterId,selection.selected.clusterId-mesh.chunkStart);
  ImGui::Text("Mesh: %u  |  Material: %u",selection.selected.meshId,chunk.materialIndex);
  ImGui::Text("Cluster triangles: %u",chunk.indexCount/3);
  ImGui::Text("Mesh: %u clusters, %u triangles",mesh.chunkCount,mesh.indexCount/3);
  ImGui::Text("Export name: mesh_%u.ply",selection.selected.meshId);
  ImGui::TextDisabled("Mesh = material submesh; bounds show through occluders");
  if(ImGui::TreeNode("Bounds")) {
    ImGui::Text("Cluster min: %.2f %.2f %.2f",chunk.boundingBox.min.x,chunk.boundingBox.min.y,chunk.boundingBox.min.z);
    ImGui::Text("Cluster max: %.2f %.2f %.2f",chunk.boundingBox.max.x,chunk.boundingBox.max.y,chunk.boundingBox.max.z);
    ImGui::Text("Mesh min: %.2f %.2f %.2f",mesh.boundingBox.min.x,mesh.boundingBox.min.y,mesh.boundingBox.min.z);
    ImGui::Text("Mesh max: %.2f %.2f %.2f",mesh.boundingBox.max.x,mesh.boundingBox.max.y,mesh.boundingBox.max.z);
    ImGui::TreePop();
  }
}

// Clip homogeneous edges to the frustum before perspective division. A box
// crossing the near plane must not emit enormous or inverted screen lines.
inline bool ClipBoundsEdge(vec4& a,vec4& b) {
  auto planes=[](const vec4& p,int i) {
    switch(i) {
      case 0:return p.w-1e-5f; case 1:return p.x+p.w; case 2:return p.w-p.x;
      case 3:return p.y+p.w; case 4:return p.w-p.y; case 5:return p.z;
      default:return p.w-p.z;
    }
  };
  for(int i=0;i<7;++i) {
    float da=planes(a,i),db=planes(b,i);
    if(!std::isfinite(da) || !std::isfinite(db) || (da<0 && db<0)) return false;
    if((da<0)!=(db<0)) {
      vec4 point=a+(b-a)*(da/(da-db));
      if(da<0) a=point; else b=point;
    }
  }
  return true;
}

template<class Chunk,class Mesh>
void DrawBounds(const Controller& selection,const Chunk* chunks,const Mesh* meshes,Camera* camera,SDL_Window* window) {
  if(!selection.ready || !selection.showBounds || selection.selected.clusterId==InvalidId ||
     !chunks || !meshes || !camera || !window) return;
  const auto* viewport=ImGui::GetMainViewport();
  if(viewport->Size.x<=0 || viewport->Size.y<=0) return;
  auto* draw=ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(viewport));
  mat4 vp=transpose(camera->getObjectToCamera()*camera->getProjectMatrix());
  auto box=[&](const auto& bounds,ImU32 color,float thickness) {
    vec4 corners[8];
    for(int i=0;i<8;++i)
      corners[i]=vp*vec4((i&1)?bounds.max.x:bounds.min.x,(i&2)?bounds.max.y:bounds.min.y,
                        (i&4)?bounds.max.z:bounds.min.z,1);
    auto pixel=[&](const vec4& p) {
      // Positive-height Vulkan scene viewport: NDC +Y maps down.
      return ImVec2(viewport->Pos.x+(p.x/p.w*0.5f+0.5f)*viewport->Size.x,
                    viewport->Pos.y+(p.y/p.w*0.5f+0.5f)*viewport->Size.y);
    };
    for(int i=0;i<8;++i) for(int bit=1;bit<=4;bit<<=1) if(!(i&bit)) {
      auto a=corners[i],b=corners[i|bit];
      if(ClipBoundsEdge(a,b)) draw->AddLine(pixel(a),pixel(b),color,thickness);
    }
  };
  draw->PushClipRect(viewport->Pos,ImVec2(viewport->Pos.x+viewport->Size.x,viewport->Pos.y+viewport->Size.y),true);
  box(meshes[selection.selected.meshId].boundingBox,IM_COL32(40,210,240,230),1.5f);
  box(chunks[selection.selected.clusterId].boundingBox,IM_COL32(255,155,35,255),2.5f);
  draw->PopClipRect();
}
}
