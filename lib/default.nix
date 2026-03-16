{
  goLock, # path to go2nix lockfile (TOML, [mod] only)
}:
let
  inherit (builtins) readFile mapAttrs;

  resolved = builtins.resolveGoModules {
    lock = readFile goLock;
  };

  # Module proxy URL escaping: uppercase letters become !lowercase
  # See https://pkg.go.dev/golang.org/x/mod/module#EscapePath
  escapeModulePath =
    path:
    builtins.replaceStrings
      [
        "A"
        "B"
        "C"
        "D"
        "E"
        "F"
        "G"
        "H"
        "I"
        "J"
        "K"
        "L"
        "M"
        "N"
        "O"
        "P"
        "Q"
        "R"
        "S"
        "T"
        "U"
        "V"
        "W"
        "X"
        "Y"
        "Z"
      ]
      [
        "!a"
        "!b"
        "!c"
        "!d"
        "!e"
        "!f"
        "!g"
        "!h"
        "!i"
        "!j"
        "!k"
        "!l"
        "!m"
        "!n"
        "!o"
        "!p"
        "!q"
        "!r"
        "!s"
        "!t"
        "!u"
        "!v"
        "!w"
        "!x"
        "!y"
        "!z"
      ]
      path;

  # Apply module replacements from resolveGoPackages to resolved modules.
  # replacements: { "path@version" = "replacement-path"; }
  applyReplacements =
    replacements:
    mapAttrs (
      modKey: mod:
      let
        fetchPath = replacements.${modKey} or mod.path;
      in
      mod
      // {
        inherit fetchPath;
        dirSuffix = "${escapeModulePath fetchPath}@${mod.version}";
      }
    ) resolved.modules;

  resolveGoPackages =
    {
      src,
      go,
      tags ? [ ],
      subPackages ? [ "./..." ],
      moduleDir ? ".",
      goos ? null,
      goarch ? null,
      goProxy ? null,
      cgoEnabled ? null,
    }:
    builtins.resolveGoPackages (
      {
        go = "${go}/bin/go";
        inherit
          src
          tags
          subPackages
          moduleDir
          ;
      }
      // (if goos != null then { inherit goos; } else { })
      // (if goarch != null then { inherit goarch; } else { })
      // (if goProxy != null then { inherit goProxy; } else { })
      // (if cgoEnabled != null then { inherit cgoEnabled; } else { })
    );

in
{
  inherit
    resolved
    escapeModulePath
    applyReplacements
    resolveGoPackages
    ;
}
