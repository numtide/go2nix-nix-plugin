#include "helpers.h"

#include <nix/expr/primops.hh>
#include <toml++/toml.hpp>

static void prim_resolveGoModules(EvalState &state, const PosIdx pos,
                                  Value **args, Value &v) {
  state.forceAttrs(
      *args[0], pos,
      "while evaluating the argument to builtins.resolveGoModules");

  NixStringContext context;
  auto lockStr = getRequiredStringAttr(state, *args[0], pos, "lock", context);

  toml::table tbl;
  try {
    tbl = toml::parse(lockStr);
  } catch (const toml::parse_error &e) {
    state
        .error<EvalError>("resolveGoModules: failed to parse TOML: %s",
                          std::string(e.what()))
        .atPos(pos)
        .debugThrow();
  }

  auto *modTbl = tbl["mod"].as_table();
  size_t modCount = modTbl ? modTbl->size() : 0;

  auto modules = state.buildBindings(modCount);

  if (modTbl) {
    for (auto &&[key, val] : *modTbl) {
      std::string modKey(key.str());
      auto *hashVal = val.as_string();
      if (!hashVal) {
        state
            .error<EvalError>(
                "resolveGoModules: value for '%s' is not a string", modKey)
            .atPos(pos)
            .debugThrow();
      }
      std::string hash = hashVal->get();

      auto atIdx = modKey.find('@');
      if (atIdx == std::string::npos) {
        state
            .error<EvalError>(
                "resolveGoModules: module key '%s' missing '@version'", modKey)
            .atPos(pos)
            .debugThrow();
      }
      std::string path = modKey.substr(0, atIdx);
      std::string version = modKey.substr(atIdx + 1);
      std::string dirSuffix = escape_mod_path(path) + "@" + version;

      auto modAttrs = state.buildBindings(5);
      modAttrs.alloc("dirSuffix").mkString(dirSuffix, state.mem);
      modAttrs.alloc("fetchPath").mkString(path, state.mem);
      modAttrs.alloc("hash").mkString(hash, state.mem);
      modAttrs.alloc("path").mkString(path, state.mem);
      modAttrs.alloc("version").mkString(version, state.mem);
      modules.alloc(modKey).mkAttrs(modAttrs.finish());
    }
  }

  auto result = state.buildBindings(1);
  result.alloc("modules").mkAttrs(modules);
  v.mkAttrs(result);
}

static RegisterPrimOp rp({
    .name = "resolveGoModules",
    .args = {"attrs"},
    .arity = 1,
    .doc = R"(
      Parse a go2nix lockfile (TOML) containing module hashes.

      Accepts an attrset with:
      - `lock`: TOML string of the go2nix lockfile contents (only `[mod]` table)

      Returns an attrset with `modules` keyed by "path@version", each containing:
      `hash`, `path`, `version`, `fetchPath`, `dirSuffix`.

      Note: `fetchPath` defaults to `path`. Use replacements from
      `builtins.resolveGoPackages` to override for replaced modules.
    )",
    .fun = prim_resolveGoModules,
});
