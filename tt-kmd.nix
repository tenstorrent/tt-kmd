{ config, lib, pkgs, ... }:
let gs-drv = { stdenv, lib, fetchFromGitHub, kernel, kmod } :
      stdenv.mkDerivation rec {
        name = "tt-kmd-${version}-${kernel.version}";
        version = "1.27.1";

        src = fetchFromGitHub {
          owner = "tenstorrent";
          repo = "tt-kmd";
          rev = "ttkmd-${version}";
          sha256 = "sha256-TcYOEqXEqyipLEfHUHKC3VA8tXuEo05EObiRVe+J4PE=";
        };

        sourceRoot = "";
        hardeningDisable = [ "pic" ];
        nativeBuildInputs = kernel.moduleBuildDependencies;

        makeFlags = [
          "KERNELRELEASE=${kernel.modDirVersion}"
          "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
        ];

        installPhase = ''
          mkdir -p $out/lib/modules/${kernel.modDirVersion}/misc
          for x in $(find . -name '*.ko'); do
              cp $x $out/lib/modules/${kernel.modDirVersion}/misc/
          done
        '';

        meta = with lib; {
          description = "A kernel module for Tenstorrent AI cards.";
          homepage = "https://github.com/tenstorrent/tt-kmd";
          license = licenses.gpl2;
          platforms = platforms.linux;
        };
      };
    extraModPackages = config.boot.extraModulePackages;
in
{
    boot.extraModulePackages = [ (config.boot.kernelPackages.callPackage gs-drv {}) ];
    boot.kernelModules = [ "tenstorrent" ];
}
