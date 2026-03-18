{ pkgs }:
let
  nixComponents = pkgs.nixVersions.latest.libs;
in
pkgs.mkShell {
  packages = [
    pkgs.go
    pkgs.cmake
    pkgs.pkg-config
    pkgs.clang-tools # clang-format
  ];

  buildInputs = [
    nixComponents.nix-expr
    nixComponents.nix-util
    pkgs.boost
    pkgs.nlohmann_json
  ];
}
