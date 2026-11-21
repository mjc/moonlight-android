{ pkgs, ... }:
{
  packages = [
    pkgs.cmake
    pkgs.ninja
  ];

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
