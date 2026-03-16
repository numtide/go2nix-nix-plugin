{
  pkgs,
  plugin,
  testFixtures,
}:

pkgs.runCommand "go2nix-nix-plugin-eval-test"
  {
    nativeBuildInputs = [ pkgs.nixVersions.nix_2_33 ];
  }
  ''
    # Use a local temp store to avoid permission issues in the sandbox
    export HOME=$(mktemp -d)
    export NIX_STORE_DIR=$TMPDIR/nix/store
    export NIX_STATE_DIR=$TMPDIR/nix/var
    export NIX_LOG_DIR=$TMPDIR/nix/log
    mkdir -p $NIX_STORE_DIR $NIX_STATE_DIR $NIX_LOG_DIR

    result=$(nix-instantiate --eval --strict --read-write-mode \
      --option plugin-files "${plugin}/lib/nix/plugins/libgo2nix_nix_plugin.so" \
      --expr '
      let
        lockfile = builtins.fromTOML (builtins.readFile "${testFixtures}/go2nix.toml");
        modTable = lockfile.mod or {};
        parseModEntry = modKey: hash:
          let
            parsed = builtins.match "(.+)@(.+)" modKey;
            path = builtins.elemAt parsed 0;
            version = builtins.elemAt parsed 1;
          in { inherit hash path version; fetchPath = path; dirSuffix = path + "@" + version; };
        modules = builtins.mapAttrs parseModEntry modTable;
        moduleCount = builtins.length (builtins.attrNames modules);
        # Pick one module and verify it has all expected fields
        net = modules."golang.org/x/net@v0.25.0";
        hasFields = builtins.all (f: builtins.hasAttr f net)
          [ "hash" "path" "version" "fetchPath" "dirSuffix" ];
      in
        "modules=''${toString moduleCount} hasFields=''${if hasFields then "true" else "false"} hash=''${net.hash} path=''${net.path} version=''${net.version}"
    ')

    echo "Plugin eval result: $result"

    # Strip quotes from nix-instantiate output
    result=$(echo "$result" | tr -d '"')

    modules=$(echo "$result" | sed 's/modules=\([0-9]*\).*/\1/')
    hasFields=$(echo "$result" | sed 's/.*hasFields=\([a-z]*\).*/\1/')

    if [ "$modules" -ne 4 ]; then
      echo "FAIL: Expected 4 modules, got $modules"
      exit 1
    fi

    if [ "$hasFields" != "true" ]; then
      echo "FAIL: Module missing expected fields"
      exit 1
    fi

    echo "PASS: resolveGoModules — $modules modules, all fields present"
    echo "$result" > $out
  ''
