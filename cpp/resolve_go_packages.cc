#include "helpers.h"

#include <map>
#include <nix/expr/primops.hh>
#include <nix/util/processes.hh>
#include <nlohmann/json.hpp>
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
  goArgs.push_back("-e");
  goArgs.push_back("-buildvcs=false");

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

  // 5. Set up environment: only what go list actually needs.
  //    Nix's runProgram replaces the entire child env when opts.environment
  //    is set. GOROOT is self-detected from the binary location. GOCACHE and
  //    TMPDIR are unused by go list. -buildvcs=false avoids VCS tool lookups.
  auto env = inheritEnv({
      "GOMODCACHE", // module cache — the critical one
      "GOPATH",     // fallback: GOMODCACHE defaults to $GOPATH/pkg/mod
      "HOME",       // fallback: GOPATH defaults to $HOME/go
  });
  env["GOPROXY"] = goProxy;
  env["GONOSUMCHECK"] = "*";
  env["GOFLAGS"] = "-mod=readonly";
  env["GOENV"] = "off";  // ignore user's ~/.config/go/env
  env["GOWORK"] = "off"; // ignore go.work files

  // When goProxy allows network access, inherit vars needed for downloads
  if (goProxy != "off") {
    auto netEnv = inheritEnv({
        "PATH",              // git/hg for GOPROXY=direct
        "TMPDIR",            // temp files for downloads
        "SSL_CERT_FILE",     // TLS certs for HTTPS proxy
        "SSL_CERT_DIR",      // TLS cert directory
        "NIX_SSL_CERT_FILE", // Nix-specific TLS cert override
    });
    env.insert(netEnv.begin(), netEnv.end());
    env["GIT_TERMINAL_PROMPT"] = "0"; // prevent git credential prompts
  }

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
        .error<EvalError>(
            "resolveGoPackages: 'go list' failed (exit %d).\n"
            "Hint: check the error output above, and ensure all modules "
            "are in your local cache by running 'go mod download'.",
            status)
        .atPos(pos)
        .debugThrow();
  }

  // 7. Parse concatenated JSON objects from go list output.
  //    With -e, package errors appear in the JSON Error field instead of
  //    stderr. Stdout is clean JSON; stderr (if any) goes to the terminal.
  struct PkgData {
    std::string importPath;
    std::string modPath;
    std::string modVersion;
    bool isStdlib = false;
    bool isMainModule = false;
    bool hasModule = false;
    bool isLocal = false; // main module or local replace (Replace.Version=="")
    std::string replacePath; // Module.Replace.Path (empty if not replaced)
    std::string
        replaceVersion; // Module.Replace.Version (empty if not replaced)
    std::vector<std::string> imports;
    bool isCgo = false;
    std::vector<std::string> cgoPkgConfig;
    std::vector<std::string> cgoCflags;
    std::vector<std::string> cgoLdflags;
  };

  std::vector<PkgData> allPkgs;
  std::vector<std::string> pkgErrors;
  std::istringstream stream(output);
  nlohmann::json jpkg;

  // nlohmann's operator>> throws parse_error on trailing whitespace + EOF
  // instead of returning a failed stream, so skip whitespace and check for
  // EOF before each parse attempt.
  while (stream >> std::ws && stream.peek() != EOF) {
    try {
      stream >> jpkg;
    } catch (const nlohmann::json::parse_error &e) {
      state
          .error<EvalError>(
              "resolveGoPackages: failed to parse go list JSON: %s", e.what())
          .atPos(pos)
          .debugThrow();
    }

    // Check for per-package errors (reported via -e flag)
    if (jpkg.contains("Error") && jpkg["Error"].is_object()) {
      auto importPath = jpkg.value("ImportPath", "<unknown>");
      auto errStr = jpkg["Error"].value("Err", "unknown error");
      pkgErrors.push_back(importPath + ": " + errStr);
      continue;
    }

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
        p.replaceVersion = repl.value("Version", "");
      }

      // A module is local if it's the main module or a local replace
      // (Replace exists with empty version). Matches go2nix's IsLocal().
      p.isLocal = p.isMainModule ||
                  (!p.replacePath.empty() && p.replaceVersion.empty());
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

  if (!pkgErrors.empty()) {
    std::string errMsg = "resolveGoPackages: package errors:\n";
    for (auto &e : pkgErrors)
      errMsg += "  - " + e + "\n";
    errMsg += "Hint: your GOMODCACHE may be stale. "
              "Run 'go mod download' to populate it.\n";
    state.error<EvalError>("%s", errMsg).atPos(pos).debugThrow();
  }

  // Build set of third-party import paths (not stdlib, not local, has module)
  std::set<std::string> thirdPartyPaths;
  for (auto &p : allPkgs) {
    if (!p.isStdlib && p.hasModule && !p.isLocal)
      thirdPartyPaths.insert(p.importPath);
  }

  // Collect module replacements: modKey -> { path, version }
  // Only remote replacements (version != ""); local replaces are filtered out.
  std::map<std::string, std::pair<std::string, std::string>> replMap;
  for (auto &p : allPkgs) {
    if (!p.hasModule || p.isLocal || p.replacePath.empty())
      continue;
    std::string modKey = p.modPath + "@" + p.modVersion;
    replMap[modKey] = {p.replacePath, p.replaceVersion};
  }

  auto replacements = state.buildBindings(replMap.size());
  for (auto &[modKey, repl] : replMap) {
    auto replAttrs = state.buildBindings(2);
    replAttrs.alloc("path").mkString(repl.first, state.mem);
    replAttrs.alloc("version").mkString(repl.second, state.mem);
    replacements.alloc(modKey).mkAttrs(replAttrs.finish());
  }

  // Count third-party packages for attrset capacity
  size_t pkgCount = 0;
  for (auto &p : allPkgs)
    if (thirdPartyPaths.count(p.importPath))
      ++pkgCount;

  // Build packages attrset
  auto packages = state.buildBindings(pkgCount);

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

    // Count optional fields
    size_t attrCount = 4; // modKey, subdir, imports, drvName
    if (p.isCgo)
      ++attrCount;
    if (!p.cgoPkgConfig.empty())
      ++attrCount;
    if (!p.cgoCflags.empty())
      ++attrCount;
    if (!p.cgoLdflags.empty())
      ++attrCount;

    auto pkgAttrs = state.buildBindings(attrCount);
    pkgAttrs.alloc("drvName").mkString(drvName, state.mem);

    auto importsList = state.buildList(filteredImports.size());
    for (size_t i = 0; i < filteredImports.size(); ++i)
      (importsList[i] = state.allocValue())
          ->mkString(filteredImports[i], state.mem);
    pkgAttrs.alloc("imports").mkList(importsList);

    pkgAttrs.alloc("modKey").mkString(modKey, state.mem);
    pkgAttrs.alloc("subdir").mkString(subdir, state.mem);

    if (p.isCgo)
      pkgAttrs.alloc("isCgo").mkBool(true);

    if (!p.cgoPkgConfig.empty()) {
      auto list = state.buildList(p.cgoPkgConfig.size());
      for (size_t i = 0; i < p.cgoPkgConfig.size(); ++i)
        (list[i] = state.allocValue())->mkString(p.cgoPkgConfig[i], state.mem);
      pkgAttrs.alloc("cgoPkgConfig").mkList(list);
    }
    if (!p.cgoCflags.empty()) {
      auto list = state.buildList(p.cgoCflags.size());
      for (size_t i = 0; i < p.cgoCflags.size(); ++i)
        (list[i] = state.allocValue())->mkString(p.cgoCflags[i], state.mem);
      pkgAttrs.alloc("cgoCflags").mkList(list);
    }
    if (!p.cgoLdflags.empty()) {
      auto list = state.buildList(p.cgoLdflags.size());
      for (size_t i = 0; i < p.cgoLdflags.size(); ++i)
        (list[i] = state.allocValue())->mkString(p.cgoLdflags[i], state.mem);
      pkgAttrs.alloc("cgoLdflags").mkList(list);
    }

    packages.alloc(p.importPath).mkAttrs(pkgAttrs.finish());
  }

  // Build result: { packages = { ... }; replacements = { ... }; }
  auto result = state.buildBindings(2);
  result.alloc("packages").mkAttrs(packages.finish());
  result.alloc("replacements").mkAttrs(replacements.finish());
  v.mkAttrs(result.finish());
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
      - `replacements`: attrset mapping "module@version" to { path, version }
        (from go.mod replace directives, extracted via Module.Replace in go list)

      Requires the host's GOMODCACHE to be populated (run `go mod download` first),
      unless `goProxy` is set to allow downloads.
    )",
    .fun = prim_resolveGoPackages,
});
