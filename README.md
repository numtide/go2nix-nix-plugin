# go2nix-nix-plugin

A Nix plugin that discovers Go package dependency graphs at evaluation time by running `go list`. Used together with [go2nix](https://github.com/numtide/go2nix) v2 lockfiles to enable per-package DAG builds without storing the package graph in the lockfile.

## Building

Requires Nix with flakes enabled. The plugin builds a `.so` shared library.

```sh
nix build .#go2nix-nix-plugin
```

Run the eval test:

```sh
nix build .#checks.x86_64-linux.pkgs-eval-test
```

## Builtins

### `builtins.resolveGoPackages`

Runs `go list -json -deps -e` against a Go source tree and returns the third-party package graph and module replacements.

**Input:**

```nix
builtins.resolveGoPackages {
  go = "${pkgs.go}/bin/go";
  src = ./.; # path to Go source directory
  # Optional:
  tags = [ "netgo" ];
  subPackages = [ "./cmd/..." ];
  modRoot = ".";
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
      drvName = "gopkg-golang-org-x-net-http2-v0.25.0";
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
    "golang.org/x/net@v0.25.0" = { path = "golang.org/x/net-fork"; version = "v0.26.0"; };
  };
  localReplaces = {
    # local replace directives (e.g. replace mod => ../path in go.mod)
    "example.com/mylib" = "../mylib";
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
  helpers.h/cc     Shared utilities (sanitize_name, inheritEnv, ...)
  resolve_go_packages.cc  builtins.resolveGoPackages implementation
  CMakeLists.txt   Build configuration
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
- [nlohmann_json](https://github.com/nlohmann/json) — JSON parsing for `go list` output
