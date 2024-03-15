//
//  ObjLoader.c
//  MetalKitAndRenderingSetup-iOS
//
//  Created by ruxpinjiang on 2021/10/29.
//  Copyright © 2021 Apple. All rights reserved.
//

#include "ObjLoader.h"
#include "Matrix.h"
#include "Primitives.h"
#include <vector>
#include <math.h>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdio>
#include <assert.h>

using namespace std;

typedef struct VertexWithAttribute
{
    vec3 pos;
    vec3 normal;
    vec2 uv;
}VertexWithAttribute;




static std::vector<VertexWithAttribute> _vertexBuffer;
static std::vector<unsigned short> _indexBuffer;

unsigned long getVertexSize()
{
    return sizeof(VertexWithAttribute)*_vertexBuffer.size();
}

void * getRawVertexData()
{
    return static_cast<void*>(&_vertexBuffer[0]);
}

void * getRawIndexData()
{
    return static_cast<void*>(&_indexBuffer[0]);
}

unsigned long getIndexSize()
{
    return sizeof(unsigned short) * _indexBuffer.size();
}

void LoadObj(const char * filepath)
{
    string path(filepath);
    string src;
        //std::vector<vec3> vertices;
        std::vector<vec3> normals;
        std::vector<vec2> uvs;
        //std::vector<Mesh> meshes;
        //std::vector<int> indexBuffer;
        std::ifstream in(path, std::ios::in);
        //std::vector<VertexWithAttribute> vertexBuffer;
        //std::string msg = "Failed to open file " + path;
        assert(in.is_open() && in.good());
        string line;
        while(std::getline(in,line))
        {
            float v_x,v_y,v_z;
            float u,v;
            VertexWithAttribute vertex;
            float n_x,n_y,n_z;
            unsigned short v1,v2,v3;
            int vt1,vt2,vt3;
            int vn1,vn2,vn3;
            switch(line[0]){
                case 'o':
                    //
                    //vertices.clear();
                    //normals.clear();
                    //uvs.clear();
                    break;
                case 'v':


                    //vertices.push_back(vec3(v_x,v_y,v_z));
                    if(line[1]=='t')
                    {
                        sscanf(line.c_str()+3,"%f %f",&u,&v);
                        uvs.push_back(vec2(u,1.f-v));
                    }
                    else if(line[1]=='n')
                    {
                        sscanf(line.c_str()+3,"%f %f %f",&n_x,&n_y,&n_z);
                        normals.push_back(vec3(n_x,n_y,n_z));}
                    else
                    {
                        sscanf(line.c_str()+2,"%f %f %f",&v_x,&v_y,&v_z);
                        vertex.pos.x = v_x;
                        vertex.pos.y = v_y;
                        vertex.pos.z = v_z;
                        _vertexBuffer.push_back(vertex);
                    }


                    break;

                case 's'://smooth group omit
                    break;
                case 'f':

                    sscanf(line.c_str()+2,"%hu/%d/%d %hu/%d/%d %hu/%d/%d",&v1,&vt1,&vn1,
                                                                        &v2,&vt2,&vn2,
                                                                        &v3,&vt3,&vn3);

                    _vertexBuffer[v1-1].normal = normals[vn1-1];
                    _vertexBuffer[v1-1].uv =uvs[vt1-1];
                    _vertexBuffer[v2-1].normal = normals[vn2-1];
                    _vertexBuffer[v2-1].uv =uvs[vt2-1];
                    _vertexBuffer[v3-1].normal = normals[vn3-1];
                    _vertexBuffer[v3-1].uv =uvs[vt3-1];

                    _indexBuffer.push_back(v1-1);
                    _indexBuffer.push_back(v2-1);
                    _indexBuffer.push_back(v3-1);

                    break;
            }
        }
}
