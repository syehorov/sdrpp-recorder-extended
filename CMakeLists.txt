cmake_minimum_required(VERSION 3.13)
project(recorder_extended)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

find_library(LAME_LIBRARY NAMES mp3lame)
find_library(VORBISENC_LIBRARY NAMES vorbisenc)
find_library(VORBIS_LIBRARY NAMES vorbis)
find_library(OGG_LIBRARY NAMES ogg)
find_path(VORBIS_INCLUDE_DIR vorbis/vorbisenc.h)
find_path(OGG_INCLUDE_DIR ogg/ogg.h)
find_path(LAME_INCLUDE_DIR lame/lame.h)

if (NOT LAME_LIBRARY OR NOT LAME_INCLUDE_DIR)
    message(FATAL_ERROR "LAME (libmp3lame) not found")
endif()

if (NOT VORBISENC_LIBRARY OR NOT VORBIS_LIBRARY OR NOT OGG_LIBRARY OR NOT VORBIS_INCLUDE_DIR OR NOT OGG_INCLUDE_DIR)
    message(FATAL_ERROR "Vorbis/Ogg (libvorbis, libvorbisenc, libogg) not found")
endif()

list(APPEND INCLUDE_DIRS ${LAME_INCLUDE_DIR})
list(APPEND INCLUDE_DIRS ${VORBIS_INCLUDE_DIR} ${OGG_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE mp3lame)
target_link_libraries(${PROJECT_NAME} PRIVATE vorbisenc vorbis ogg)
target_include_directories(${PROJECT_NAME} PRIVATE "src/")
target_include_directories(${PROJECT_NAME} PRIVATE "../../decoder_modules/radio/src")