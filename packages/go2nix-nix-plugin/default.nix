{ pkgs, ... }:
let
  nixComponents = pkgs.nixVersions.nixComponents_2_33;
in
pkgs.callPackage ../../nix/plugin.nix {
  inherit nixComponents;
}
