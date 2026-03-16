# go2nix-nix-plugin

A Nix plugin that provides builtins for resolving Go module dependencies at evaluation time. It consumes [go2nix](https://github.com/numtide/go2nix) v2 lockfiles and can discover Go package graphs by running `go list`.

## Building

Requires Nix with flakes enabled. The plugin builds a `.so` shared library.

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
  goProxy = "https://proxy.golang.org,direct"; # default: "off"
  cgoEnabled = "0"; # default: Go's default
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

By default `goProxy` is `"off"`, which requires the host's `GOMODCACHE` to be populated (e.g. via `go mod download`). Set `goProxy` to a proxy URL to allow downloading modules at eval time.

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

Format the codebase:

```sh
nix fmt
```

### Project structure

```
cpp/             C++ plugin source
  helpers.h/cc     Shared utilities (escape_mod_path, sanitize_name, ...)
  resolve_go_modules.cc   builtins.resolveGoModules implementation
  resolve_go_packages.cc  builtins.resolveGoPackages implementation
  CMakeLists.txt   Build configuration
lib/             Nix library wrapper (default.nix)
nix/             Plugin derivation (plugin.nix)
packages/        Blueprint package definitions
tests/           Eval tests and fixtures
devshell.nix     Development shell (loaded by blueprint)
formatter.nix    Treefmt configuration (loaded by blueprint)
flake.nix        Flake definition
.envrc           direnv integration
```

### Dependencies

- [Nix](https://nixos.org/) >= 2.33 (nix-expr, nix-util)
- [nlohmann_json](https://github.com/nlohmann/json) — JSON parsing and serialization
- [toml++](https://github.com/marzer/tomlplusplus) — TOML parsing for lockfiles
