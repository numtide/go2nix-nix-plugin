{ pkgs, ... }:
let
  nixComponents = pkgs.nixVersions.latest.libs;
  plugin = pkgs.callPackage ../../nix/plugin.nix { inherit nixComponents; };
in
pkgs.callPackage ../../tests/eval-test.nix {
  inherit plugin;
  testFixtures = ../../tests/fixtures;
}
