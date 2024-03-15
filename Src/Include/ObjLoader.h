//
//  ObjLoader.h
//  MetalKitAndRenderingSetup-iOS
//
//  Created by ruxpinjiang on 2021/10/29.
//  Copyright © 2021 Apple. All rights reserved.
//

#pragma once

#if defined __cplusplus
extern "C"{

#endif

void LoadObj(const char * filepath);

unsigned long getVertexSize();

unsigned long getIndexSize();

void * getRawVertexData();

void * getRawIndexData();

#if defined __cplusplus
}
#endif


