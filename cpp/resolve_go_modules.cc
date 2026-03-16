#include "helpers.h"

#include <nix/expr/json-to-value.hh>
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

  nlohmann::json modules = nlohmann::json::object();

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

      modules[modKey] = {
          {"hash", hash},
          {"path", path},
          {"version", version},
          {"fetchPath", path},
          {"dirSuffix", escape_mod_path(path) + "@" + version},
      };
    }
  }

  nlohmann::json result = {{"modules", modules}};
  parseJSON(state, result.dump(), v);
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
