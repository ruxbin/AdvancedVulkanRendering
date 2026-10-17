# SVT 离线工具（设计 §11 接入点表：新增 Tools/SvtCooker.cpp）。
# lzfse 以静态方式编入工具，避免依赖共享 DLL 部署。

set(LZFSE_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/lzfse/src)
set(SVT_LZFSE_SOURCES
    ${LZFSE_SRC_DIR}/lzfse_decode.c
    ${LZFSE_SRC_DIR}/lzfse_decode_base.c
    ${LZFSE_SRC_DIR}/lzfse_encode.c
    ${LZFSE_SRC_DIR}/lzfse_encode_base.c
    ${LZFSE_SRC_DIR}/lzfse_fse.c
    ${LZFSE_SRC_DIR}/lzvn_decode_base.c
    ${LZFSE_SRC_DIR}/lzvn_encode_base.c
)

add_executable(SvtCooker
    SvtCooker.cpp
    ../Src/SvtAsset.cpp
    ../Src/SvtCooker.cpp
    ${SVT_LZFSE_SOURCES}
)
target_compile_features(SvtCooker PRIVATE cxx_std_17)
target_include_directories(SvtCooker PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../Src/Include
    ${LZFSE_SRC_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/json/single_include
)
