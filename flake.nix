{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs:
        let
          plugin = pkgs.callPackage ./nix/plugin.nix {
            nixComponents = pkgs.nixVersions.nix_2_34.libs;
          };
        in
        {
          default = plugin;
          go2nix-nix-plugin = plugin;
        }
      );

      checks = forAllSystems (pkgs:
        let
          plugin = pkgs.callPackage ./nix/plugin.nix {
            nixComponents = pkgs.nixVersions.nix_2_34.libs;
          };

          core = pkgs.rustPlatform.buildRustPackage {
            pname = "go2nix-nix-plugin-core";
            version = "0.1.0";
            src = ./rust;
            cargoLock.lockFile = ./rust/Cargo.lock;
            doCheck = false;
          };
        in
        {
          build = plugin;

          clippy = core.overrideAttrs (old: {
            pname = "go2nix-nix-plugin-clippy";
            nativeBuildInputs = (old.nativeBuildInputs or [ ]) ++ [ pkgs.clippy ];
            buildPhase = ''
              cargo clippy --all-targets -- -D warnings
            '';
            installPhase = ''
              touch $out
            '';
          });

          rustfmt = pkgs.runCommand "go2nix-nix-plugin-rustfmt" {
            nativeBuildInputs = [ pkgs.rustfmt ];
          } ''
            find ${./rust/src} -name '*.rs' -exec ${pkgs.rustfmt}/bin/rustfmt --check {} +
            touch $out
          '';

          eval-test = pkgs.callPackage ./tests/eval-test.nix {
            inherit plugin;
            testFixtures = ./tests/fixtures;
          };
        }
      );
    };
}
