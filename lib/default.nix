{
  goLock, # path to go2nix lockfile (TOML, [mod] only)
}:
let
  inherit (builtins)
    readFile
    fromTOML
    mapAttrs
    match
    elemAt
    ;

  lockfile = fromTOML (readFile goLock);
  modTable = lockfile.mod or { };

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

  # Parse "path@version" = "hash" into structured module data
  parseModEntry =
    modKey: hash:
    let
      parsed = match "(.+)@(.+)" modKey;
      path = elemAt parsed 0;
      version = elemAt parsed 1;
    in
    {
      inherit hash path version;
      fetchPath = path;
      dirSuffix = "${escapeModulePath path}@${version}";
    };

  resolved = {
    modules = mapAttrs parseModEntry modTable;
  };

  # Apply module replacements from resolveGoPackages to resolved modules.
  # replacements: { "path@version" = { path, version }; }
  applyReplacements =
    replacements:
    mapAttrs (
      modKey: mod:
      let
        repl = replacements.${modKey} or null;
        fetchPath = if repl != null then repl.path else mod.path;
        version = if repl != null && repl.version != "" then repl.version else mod.version;
      in
      mod
      // {
        inherit fetchPath version;
        dirSuffix = "${escapeModulePath fetchPath}@${version}";
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
