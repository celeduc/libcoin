
# FIXME: For OS X, need flag for Framework or dylib
IF(DYNAMIC_LIBCOIN)
    ADD_DEFINITIONS(-DCOINPOOL_LIBRARY)
ELSE()
    ADD_DEFINITIONS(-DCOINPOOL_LIBRARY_STATIC)
ENDIF()

SET(LIB_NAME coinPool)

SET(HEADER_PATH ${PROJECT_SOURCE_DIR}/include/${LIB_NAME})
SET(TARGET_H
    ${HEADER_PATH}/Export.h
    ${HEADER_PATH}/GetBlockTemplate.h
    ${HEADER_PATH}/GetWork.h
    ${HEADER_PATH}/Miner.h
    ${HEADER_PATH}/Pool.h
    ${HEADER_PATH}/SubmitBlock.h
    ${LIBCOIN_CONFIG_HEADER}
)

#ADD_LIBRARY(${LIB_NAME}
#    ${LIBCOIN_USER_DEFINED_DYNAMIC_OR_STATIC}
#    ${LIB_PUBLIC_HEADERS}
SET(TARGET_SRC
    GetBlockTemplate.cpp
    GetWork.cpp
    Miner.cpp
    Pool.cpp
    SubmitBlock.cpp
    ${LIBCOIN_VERSIONINFO_RC}
)
SET(TARGET_LIBRARIES coin coinChain coinHTTP coinWallet)

IF(DYNAMIC_LIBCOIN)
SET(TARGET_EXTERNAL_LIBRARIES
    ${CMAKE_THREAD_LIBS_INIT}    
    ${MATH_LIBRARY} 
    ${OPENSSL_LIBRARIES} 
    ${Boost_LIBRARIES} 
    ${BDB_LIBRARY} 
    ${DL_LIBRARY}
)
ENDIF()


SETUP_LIBRARY(${LIB_NAME})

