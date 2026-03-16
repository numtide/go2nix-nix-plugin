# go2nix-nix-plugin

A Nix plugin that provides builtins for resolving Go module dependencies at evaluation time. It consumes [go2nix](https://github.com/numtide/go2nix) v2 lockfiles and can discover Go package graphs by running `go list`.

## Building

Requires Nix with flakes enabled. The plugin targets `x86_64-linux` (Nix plugins are `.so` shared libraries).

```sh
nix build .#go2nix-nix-plugin
```

Run the eval test:

```sh
nix build .#eval-test
```

## Builtins

### `builtins.resolveGoModules`

Parses a go2nix lockfile (TOML `[mod]` table) and returns structured module metadata.

**Input:**

```nix
builtins.resolveGoModules {
  lock = builtins.readFile ./go2nix.toml;
}
```

**Output:**

```nix
{
  modules = {
    "golang.org/x/net@v0.25.0" = {
      hash = "sha256-abc123...";
      path = "golang.org/x/net";
      version = "v0.25.0";
      fetchPath = "golang.org/x/net";          # defaults to path
      dirSuffix = "golang.org/x/net@v0.25.0";  # escaped for GOMODCACHE layout
    };
    # ...
  };
}
```

`fetchPath` and `dirSuffix` default to the module's own path. Use `applyReplacements` from the Nix library to override them with replacement info from `resolveGoPackages`.

### `builtins.resolveGoPackages`

Runs `go list -json -deps` against a Go source tree and returns the third-party package graph and module replacements.

**Input:**

```nix
builtins.resolveGoPackages {
  go = "${pkgs.go}/bin/go";
  src = ./.; # path to Go source directory
  # Optional:
  tags = [ "netgo" ];
  subPackages = [ "./cmd/..." ];
  moduleDir = ".";
  goos = "linux";
  goarch = "amd64";
}
```

**Output:**

```nix
{
  packages = {
    "golang.org/x/net/http2" = {
      modKey = "golang.org/x/net@v0.25.0";
      subdir = "http2";
      imports = [ "golang.org/x/text/encoding" ];
      drvName = "gopkg-golang.org-x-net-http2";
      # CGO fields (only present when applicable):
      isCgo = true;
      cgoPkgConfig = [ "sqlite3" ];
      cgoCflags = [ "-std=gnu99" ];
      cgoLdflags = [ "-ldl" ];
    };
    # ...
  };
  replacements = {
    # from go.mod replace directives (extracted via Module.Replace in go list)
    "golang.org/x/net@v0.25.0" = "golang.org/x/net-fork";
  };
}
```

Requires the host's `GOMODCACHE` to be populated (`go mod download`).

## Lockfile format

The lockfile only needs a `[mod]` table — module hashes:

```toml
[mod]
"golang.org/x/net@v0.25.0" = "sha256-abc123..."
"golang.org/x/text@v0.15.0" = "sha256-def456..."
```

Keys are `path@version`, values are SRI hashes. Replace directives and the package graph are discovered at eval time via `resolveGoPackages` (which runs `go list`).

## Nix library

`lib/default.nix` provides a higher-level interface:

```nix
let
  goNixPlugin = import ./lib {
    inherit pkgs;
    goLock = ./go2nix.toml;
  };

  # Discover package graph + replacements from source (runs go list at eval time)
  graph = goNixPlugin.resolveGoPackages {
    src = ./.;
    go = pkgs.go;
  };

  # Apply replacements to get correct fetchPath/dirSuffix for replaced modules
  modules = goNixPlugin.applyReplacements graph.replacements;
in
{
  # modules: { "path@version" = { hash, path, version, fetchPath, dirSuffix }; }
  inherit modules;

  # graph.packages: { "import/path" = { modKey, subdir, imports, drvName, ... }; }
  inherit (graph) packages;
}
```

## Development

Enter the dev shell:

```sh
nix develop
```

### Project structure

```
cpp/           C++ plugin source (plugin.cc, CMakeLists.txt)
lib/           Nix library wrapper (default.nix)
nix/           Plugin derivation (plugin.nix)
tests/         Eval tests and fixtures
flake.nix      Flake definition
```

### Dependencies

- [Nix](https://nixos.org/) >= 2.33 (nix-expr, nix-util)
- [nlohmann_json](https://github.com/nlohmann/json) — JSON parsing and serialization
- [toml++](https://github.com/marzer/tomlplusplus) — TOML parsing for lockfiles
