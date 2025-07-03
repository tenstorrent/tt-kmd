{ lib, inputs }:
pkgs: prev:
with pkgs;
let
  overlayLinuxPackages =
    prevLinuxPackages:
    prevLinuxPackages.extend (
      linuxPackages: super: {
        tt-kmd = super.tt-kmd.overrideAttrs (
          finalAttrs: prevAttrs: {
            name = "tt-kmd-${finalAttrs.version}-${linuxPackages.kernel.version}";
            version = "2.0.0-git+${inputs.self.shortRev or "dirty"}";

            src = inputs.self.outPath;

            passthru.isOverlaid = true;

            meta = prevAttrs.meta // {
              broken = linuxPackages.kernel.kernelOlder "6.10";
            };
          }
        );
      }
    );
in
{
  linuxKernel = prev.linuxKernel // {
    packages = lib.mapAttrs (
      _: linuxPackages: overlayLinuxPackages linuxPackages
    ) prev.linuxKernel.packages;
  };
}
