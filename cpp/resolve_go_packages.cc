#include "helpers.h"

#include <nix/expr/json-to-value.hh>
#include <nix/expr/primops.hh>
#include <nix/util/processes.hh>
#include <sstream>

static void prim_resolveGoPackages(EvalState &state, const PosIdx pos,
                                   Value **args, Value &v) {
  state.forceAttrs(
      *args[0], pos,
      "while evaluating the argument to builtins.resolveGoPackages");

  NixStringContext context;

  // 1. Extract attributes
  auto goBin = getRequiredStringAttr(state, *args[0], pos, "go", context);
  auto srcDir = getRequiredStringAttr(state, *args[0], pos, "src", context);
  auto tags = getOptionalStringListAttr(state, *args[0], pos, "tags", {});
  auto subPackages =
      getOptionalStringListAttr(state, *args[0], pos, "subPackages", {"./..."});
  auto moduleDir =
      getOptionalStringAttr(state, *args[0], pos, "moduleDir", ".");
  auto goos = getOptionalStringAttr(state, *args[0], pos, "goos", "");
  auto goarch = getOptionalStringAttr(state, *args[0], pos, "goarch", "");
  auto goProxy = getOptionalStringAttr(state, *args[0], pos, "goProxy", "off");
  auto cgoEnabled =
      getOptionalStringAttr(state, *args[0], pos, "cgoEnabled", "");

  // 2. Realise context — ensures the Go toolchain store path exists
  try {
    auto _ = state.realiseContext(context);
  } catch (InvalidPathError &e) {
    state
        .error<EvalError>(
            "resolveGoPackages: cannot realise context for '%s': %s", e.path,
            e.what())
        .atPos(pos)
        .debugThrow();
  }

  // 3. Build go list command args
  Strings goArgs;
  goArgs.push_back("list");
  goArgs.push_back("-json");
  goArgs.push_back("-deps");

  if (!tags.empty()) {
    std::string tagStr;
    for (auto &t : tags) {
      if (!tagStr.empty())
        tagStr += ",";
      tagStr += t;
    }
    goArgs.push_back("-tags");
    goArgs.push_back(tagStr);
  }

  for (auto &pkg : subPackages)
    goArgs.push_back(pkg);

  // 4. Compute working directory
  std::string workDir = srcDir;
  if (moduleDir != ".")
    workDir = srcDir + "/" + moduleDir;

  // 5. Set up environment: inherit parent env + overrides
  auto env = copyCurrentEnviron();
  env["GOPROXY"] = goProxy;
  env["GONOSUMCHECK"] = "*";
  env["GOFLAGS"] = "-mod=mod";
  if (!goos.empty())
    env["GOOS"] = goos;
  if (!goarch.empty())
    env["GOARCH"] = goarch;
  if (!cgoEnabled.empty())
    env["CGO_ENABLED"] = cgoEnabled;

  // 6. Run go list
  RunOptions opts;
  opts.program = goBin;
  opts.lookupPath = false;
  opts.args = goArgs;
  opts.chdir = workDir;
  opts.environment = env;
  opts.mergeStderrToStdout = false;

  auto [status, output] = runProgram(std::move(opts));

  if (status != 0) {
    state
        .error<EvalError>("resolveGoPackages: 'go list' failed (exit %d).\n"
                          "Output:\n%s\n\n"
                          "Hint: ensure all modules are in your local cache by "
                          "running 'go mod download'.",
                          status, output)
        .atPos(pos)
        .debugThrow();
  }

  // 7. Parse concatenated JSON objects from go list output
  struct PkgData {
    std::string importPath;
    std::string modPath;
    std::string modVersion;
    bool isStdlib = false;
    bool isMainModule = false;
    bool hasModule = false;
    std::string replacePath; // Module.Replace.Path (empty if not replaced)
    std::vector<std::string> imports;
    bool isCgo = false;
    std::vector<std::string> cgoPkgConfig;
    std::vector<std::string> cgoCflags;
    std::vector<std::string> cgoLdflags;
  };

  std::vector<PkgData> allPkgs;
  std::istringstream stream(output);
  nlohmann::json jpkg;

  while (stream >> jpkg) {
    PkgData p;
    p.importPath = jpkg.value("ImportPath", "");
    p.isStdlib = jpkg.value("Standard", false);

    if (jpkg.contains("Module") && jpkg["Module"].is_object()) {
      auto &mod = jpkg["Module"];
      p.hasModule = true;
      p.modPath = mod.value("Path", "");
      p.modVersion = mod.value("Version", "");
      p.isMainModule = mod.value("Main", false);

      // Extract replacement info
      if (mod.contains("Replace") && mod["Replace"].is_object()) {
        auto &repl = mod["Replace"];
        p.replacePath = repl.value("Path", "");
      }
    }

    if (jpkg.contains("Imports") && jpkg["Imports"].is_array()) {
      for (auto &imp : jpkg["Imports"])
        p.imports.push_back(imp.get<std::string>());
    }

    p.isCgo = jpkg.contains("CgoFiles") && jpkg["CgoFiles"].is_array() &&
              !jpkg["CgoFiles"].empty();

    if (jpkg.contains("CgoPkgConfig") && jpkg["CgoPkgConfig"].is_array())
      for (auto &x : jpkg["CgoPkgConfig"])
        p.cgoPkgConfig.push_back(x.get<std::string>());

    if (jpkg.contains("CgoCFLAGS") && jpkg["CgoCFLAGS"].is_array())
      for (auto &x : jpkg["CgoCFLAGS"])
        p.cgoCflags.push_back(x.get<std::string>());

    if (jpkg.contains("CgoLDFLAGS") && jpkg["CgoLDFLAGS"].is_array())
      for (auto &x : jpkg["CgoLDFLAGS"])
        p.cgoLdflags.push_back(x.get<std::string>());

    allPkgs.push_back(std::move(p));
  }

  // Build set of third-party import paths (not stdlib, not main module, has
  // module)
  std::set<std::string> thirdPartyPaths;
  for (auto &p : allPkgs) {
    if (!p.isStdlib && p.hasModule && !p.isMainModule)
      thirdPartyPaths.insert(p.importPath);
  }

  // Collect module replacements: modKey -> replacePath
  // (deduplicated since many packages share the same module)
  nlohmann::json replacements = nlohmann::json::object();
  for (auto &p : allPkgs) {
    if (!p.hasModule || p.isMainModule || p.replacePath.empty())
      continue;
    std::string modKey = p.modPath + "@" + p.modVersion;
    replacements[modKey] = p.replacePath;
  }

  // Build packages
  nlohmann::json packages = nlohmann::json::object();

  for (auto &p : allPkgs) {
    if (!thirdPartyPaths.count(p.importPath))
      continue;

    std::string modKey = p.modPath + "@" + p.modVersion;

    std::string subdir;
    if (p.importPath != p.modPath) {
      std::string prefix = p.modPath + "/";
      if (p.importPath.compare(0, prefix.size(), prefix) == 0)
        subdir = p.importPath.substr(prefix.size());
    }

    std::vector<std::string> filteredImports;
    for (auto &imp : p.imports) {
      if (thirdPartyPaths.count(imp))
        filteredImports.push_back(imp);
    }

    std::string drvName = "gopkg-" + sanitize_name(p.importPath);

    nlohmann::json pkgJson = {
        {"modKey", modKey},
        {"subdir", subdir},
        {"imports", filteredImports},
        {"drvName", drvName},
    };

    if (p.isCgo)
      pkgJson["isCgo"] = true;
    if (!p.cgoPkgConfig.empty())
      pkgJson["cgoPkgConfig"] = p.cgoPkgConfig;
    if (!p.cgoCflags.empty())
      pkgJson["cgoCflags"] = p.cgoCflags;
    if (!p.cgoLdflags.empty())
      pkgJson["cgoLdflags"] = p.cgoLdflags;

    packages[p.importPath] = pkgJson;
  }

  nlohmann::json result = {
      {"packages", packages},
      {"replacements", replacements},
  };
  parseJSON(state, result.dump(), v);
}

static RegisterPrimOp rp2({
    .name = "resolveGoPackages",
    .args = {"attrs"},
    .arity = 1,
    .doc = R"(
      Discover the Go package graph at eval time by running `go list` against
      the host's GOMODCACHE.

      Accepts an attrset with:
      - `go`: Path to the Go binary (string, with store context)
      - `src`: Path to the Go source directory
      - `tags` (optional): List of build tags (default: [])
      - `subPackages` (optional): List of package patterns (default: ["./..."])
      - `moduleDir` (optional): Subdirectory containing go.mod (default: ".")
      - `goos` (optional): Target GOOS for cross-compilation (default: host OS)
      - `goarch` (optional): Target GOARCH for cross-compilation (default: host arch)
      - `goProxy` (optional): GOPROXY value (default: "off", set to
        "https://proxy.golang.org,direct" to allow downloads)
      - `cgoEnabled` (optional): CGO_ENABLED value ("0" or "1", default: Go's default)

      Returns an attrset with:
      - `packages`: attrset keyed by import path, each with:
        - `modKey`: "module@version"
        - `subdir`: subdirectory within the module
        - `imports`: list of third-party import paths
        - `drvName`: sanitized derivation name
        - `isCgo` (optional): true if the package uses CGO
        - `cgoPkgConfig` (optional): list of pkg-config packages
        - `cgoCflags` (optional): list of CGO CFLAGS
        - `cgoLdflags` (optional): list of CGO LDFLAGS
      - `replacements`: attrset mapping "module@version" to replacement path
        (from go.mod replace directives, extracted via Module.Replace in go list)

      Requires the host's GOMODCACHE to be populated (run `go mod download` first),
      unless `goProxy` is set to allow downloads.
    )",
    .fun = prim_resolveGoPackages,
});
