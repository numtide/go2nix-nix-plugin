# go2nix-nix-plugin

A Nix plugin that discovers Go package dependency graphs at evaluation time by running `go list`. Used together with [go2nix](https://github.com/numtide/go2nix) v2 lockfiles to enable per-package DAG builds without storing the package graph in the lockfile.

The core logic is a pure Rust library (`rust/`) that takes JSON input and returns JSON output. The nix integration layer (`plugin/resolveGoPackages.cc`) registers `builtins.resolveGoPackages`, serializes the nix attrset to JSON via the [nix C API](https://nix.dev/manual/nix/latest/c-api.html), calls the Rust library, and parses the result back.

## Building

```sh
nix build
```

This produces a plugin `.so` loadable via `--option plugin-files`.

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
    # from go.mod replace directives
    "golang.org/x/net@v0.25.0" = { path = "golang.org/x/net-fork"; version = "v0.26.0"; };
  };
  localReplaces = {
    # local replace directives (e.g. replace mod => ../path in go.mod)
    "example.com/mylib" = "../mylib";
  };
}
```

By default `goProxy` is `"off"`, which requires the host's `GOMODCACHE` to be populated (e.g. via `go mod download`). Set `goProxy` to a proxy URL to allow downloading modules at eval time.

## Dependencies

- [Nix](https://nixos.org/) >= 2.34
