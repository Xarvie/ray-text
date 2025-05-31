cmake_minimum_required(VERSION 3.10)
project(PerfectTextEditor)

set(CMAKE_CXX_STANDARD 17)

# Find raylib
find_package(raylib REQUIRED)
include_directories(${RAYLIB_INCLUDE_DIRS})
link_directories(${RAYLIB_LIB_DIRS})

# Find ICU
find_package(ICU REQUIRED uc i18n)
if(ICU_FOUND)
    include_directories(${ICU_INCLUDE_DIRS})
    link_directories(${ICU_LIBRARY_DIRS})
    set(ICU_LIBRARIES ${ICU_UC_LIBRARY} ${ICU_I18N_LIBRARY})
endif()

# Find FreeType
include_directories(SYSTEM "C:\\msys64\\clang64\\include\\freetype2\\freetype")
include_directories(SYSTEM "C:\\msys64\\clang64\\include\\freetype2")
set(FREETYPE_LIBRARIES freetype)
link_directories(${FreeType_LIBRARIES})

# Find HarfBuzz
find_package(HarfBuzz REQUIRED)
if(HarfBuzz_FOUND)
    include_directories(${HarfBuzz_INCLUDE_DIRS} )
    link_directories(${HarfBuzz_LIBRARIES}) # HarfBuzz provides LIBRARIES variable
    set(HARFBUZZ_LIBRARIES HarfBuzz::HarfBuzz) # Use the target
endif()

set(STB_BACKEND ON)


if(STB_BACKEND)
    set(BACKEND_SRC src/RaylibSDFText.cpp)
else()
    set(BACKEND_SRC src/RaylibSDFTextEx.cpp)
endif()

# Add your project source files
add_executable(PerfectTextEditor
        src/main.cpp
        src/RaylibSDFText.cpp
)
# Add your project source files
add_executable(PerfectTextEditorEx
        src/main.cpp
        src/RaylibSDFTextEx.cpp
)
if(STB_BACKEND)
    target_compile_definitions(PerfectTextEditor PUBLIC -DSTB_BACKEND)
else()
    target_compile_definitions(PerfectTextEditorEx PUBLIC -DFT_BACKEND)
endif()

target_include_directories(PerfectTextEditor PUBLIC src/)
target_include_directories(PerfectTextEditorEx PUBLIC src/)
# Link against raylib
target_link_libraries(PerfectTextEditor raylib)
target_link_libraries(PerfectTextEditorEx raylib)
# Link against ICU
if(ICU_FOUND)
    target_link_libraries(PerfectTextEditor ${ICU_LIBRARIES})
    target_link_libraries(PerfectTextEditorEx ${ICU_LIBRARIES})
endif()

# Link against FreeType
#if(FreeType_FOUND)
    target_link_libraries(PerfectTextEditor freetype)
    target_link_libraries(PerfectTextEditorEx freetype)
#endif()

# Link against HarfBuzz
if(HarfBuzz_FOUND)
    target_link_libraries(PerfectTextEditor harfbuzz)
    target_link_libraries(PerfectTextEditorEx harfbuzz)
endif()