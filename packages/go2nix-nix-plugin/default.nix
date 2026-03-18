{ pkgs, ... }:
let
  nixComponents = pkgs.nixVersions.latest.libs;
in
pkgs.callPackage ../../nix/plugin.nix {
  inherit nixComponents;
}
