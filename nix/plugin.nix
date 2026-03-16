{
  lib,
  stdenv,
  nixComponents,
  pkg-config,
  cmake,
  boost,
  nlohmann_json,
  tomlplusplus,
}:

stdenv.mkDerivation {
  pname = "go2nix-nix-plugin";
  version = "0.1.0";

  src = ../cpp;

  nativeBuildInputs = [
    pkg-config
    cmake
  ];

  buildInputs = [
    nixComponents.nix-expr
    nixComponents.nix-util
    boost
    nlohmann_json
    tomlplusplus
  ];

  meta = {
    description = "Nix plugin for resolving Go module dependencies";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
  };
}
