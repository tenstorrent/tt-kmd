{
  description = "Tenstorrent Kernel Module";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    systems.url = "github:nix-systems/default-linux";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs =
    {
      self,
      nixpkgs,
      systems,
      flake-parts,
      ...
    }@inputs:
    flake-parts.lib.mkFlake { inherit inputs; } (
      { lib, inputs, ... }:
      {
        systems = import inputs.systems;
        flake.overlays.default = import ./contrib/packaging/nix/overlay.nix { inherit lib inputs; };
        perSystem =
          { pkgs, system, ... }:
          {
            _module.args.pkgs = import inputs.nixpkgs {
              inherit system;
              overlays = [
                inputs.self.overlays.default
              ];
            };

            packages =
              {
                default = pkgs.linuxPackages.tt-kmd;
              }
              // lib.mapAttrs (_: linuxPackages: linuxPackages.tt-kmd) (
                lib.filterAttrs (
                  name: linuxPackages:
                  let
                    eval = builtins.tryEval (
                      linuxPackages ? "tt-kmd"
                      && linuxPackages.kernel.meta.available
                      && linuxPackages.tt-kmd.meta.available
                      && linuxPackages.tt-kmd.passthru.isOverlaid or false
                    );
                  in
                  lib.hasPrefix "linuxPackages_" name && eval.success && eval.value
                ) pkgs
              );

            legacyPackages = pkgs;
          };
      }
    );
}
