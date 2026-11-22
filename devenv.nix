{ pkgs, lib, ... }:
let
  graniteRoot = ./app/src/main/jni/pyrowave/upstream/Granite;
  hasGranite = builtins.pathExists (graniteRoot + "/slangmosh/slangmosh.cpp");

  slangmosh = pkgs.stdenv.mkDerivation {
    pname = "slangmosh";
    version = "0-unstable-granite";

    src = lib.fileset.toSource {
      root = graniteRoot;
      fileset = lib.fileset.unions [
        (graniteRoot + "/CMakeLists.txt")
        (graniteRoot + "/application")
        (graniteRoot + "/compiler")
        (graniteRoot + "/ecs")
        (graniteRoot + "/event")
        (graniteRoot + "/filesystem")
        (graniteRoot + "/math")
        (graniteRoot + "/path")
        (graniteRoot + "/slangmosh")
        (graniteRoot + "/threading")
        (graniteRoot + "/util")
        (graniteRoot + "/vulkan")
        (graniteRoot + "/third_party/CMakeLists.txt")
        (graniteRoot + "/third_party/rapidjson_wrapper.hpp")
        (graniteRoot + "/third_party/khronos")
        (graniteRoot + "/third_party/renderdoc")
        (graniteRoot + "/third_party/volk")
      ];
    };

    nativeBuildInputs = [
      pkgs.cmake
      pkgs.ninja
    ];

    buildInputs = [
      pkgs.rapidjson
      pkgs.shaderc.dev
      pkgs.shaderc.lib
      pkgs.spirv-cross
      pkgs.spirv-headers
      pkgs.spirv-tools.dev
      pkgs.spirv-tools.lib
    ];

    postPatch = ''
      substituteInPlace third_party/CMakeLists.txt \
        --replace-fail 'add_subdirectory(spirv-cross EXCLUDE_FROM_ALL)' \
          'add_library(spirv-cross-core STATIC IMPORTED GLOBAL)
           set_target_properties(spirv-cross-core PROPERTIES
             IMPORTED_LOCATION "${pkgs.spirv-cross}/lib/libspirv-cross-core.a"
             INTERFACE_INCLUDE_DIRECTORIES "${pkgs.spirv-cross}/include/spirv_cross")' \
        --replace-fail 'add_subdirectory(shaderc EXCLUDE_FROM_ALL)' \
          'add_library(shaderc SHARED IMPORTED GLOBAL)
           set_target_properties(shaderc PROPERTIES
             IMPORTED_LOCATION "${pkgs.shaderc.lib}/lib/libshaderc_shared.so"
             INTERFACE_INCLUDE_DIRECTORIES "${pkgs.shaderc.dev}/include")
           add_library(SPIRV-Tools STATIC IMPORTED GLOBAL)
           set_target_properties(SPIRV-Tools PROPERTIES
             IMPORTED_LOCATION "${pkgs.spirv-tools.lib}/lib/libSPIRV-Tools.a"
             INTERFACE_INCLUDE_DIRECTORIES "${pkgs.spirv-tools.dev}/include;${pkgs.spirv-headers}/include")'

      cat >> third_party/CMakeLists.txt <<'EOF'
if (NOT TARGET granite-rapidjson)
    add_library(granite-rapidjson INTERFACE)
    target_include_directories(granite-rapidjson INTERFACE
        ''${CMAKE_CURRENT_SOURCE_DIR}
        "${pkgs.rapidjson}/include")
endif()
EOF
    '';

    cmakeFlags = [
      "-G" "Ninja"
      "-DCMAKE_BUILD_TYPE=Release"
      "-DGRANITE_TOOLS=ON"
      "-DGRANITE_VULKAN_SPIRV_CROSS=ON"
      "-DGRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER=ON"
      "-DGRANITE_VULKAN_SYSTEM_HANDLES=OFF"
      "-DGRANITE_VULKAN_FOSSILIZE=OFF"
      "-DGRANITE_RENDERER=OFF"
      "-DGRANITE_RENDERDOC_CAPTURE=OFF"
      "-DGRANITE_PLATFORM=null"
      "-DGRANITE_AUDIO=OFF"
      "-DGRANITE_SHADER_COMPILER_OPTIMIZE=ON"
    ];

    buildPhase = ''
      runHook preBuild
      ninja slangmosh
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      install -Dm755 slangmosh/slangmosh "$out/bin/slangmosh"
      runHook postInstall
    '';
  };
in
{
  packages = [
    pkgs.cmake
    pkgs.ninja
  ] ++ lib.optional hasGranite slangmosh;

  android = {
    enable = true;
    platforms.version = [ "37" ];
    buildTools.version = [ "37.0.0" ];
    cmake.version = [ ];
    ndk = {
      enable = true;
      version = [ "29.0.14206865" ];
    };
    emulator.enable = false;
    extras = [ ];
    sources.enable = false;
    systemImages.enable = false;
  };
}
