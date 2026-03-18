#include "helpers.h"

#include <map>
#include <nix/expr/primops.hh>
#include <nix/util/processes.hh>
#include <nlohmann/json.hpp>
#include <set>
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
  auto modRoot = getOptionalStringAttr(state, *args[0], pos, "modRoot", ".");
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
            "resolveGoPackages: cannot realise context for '%s': %s", e.path.to_string(),
            e.what())
        .atPos(pos)
        .debugThrow();
  }

  // 3. Build go list command args
  //    -json=<fields> (Go 1.24+) tells go list to only emit the fields we
  //    need, reducing output size and serialization time significantly.
  Strings goArgs;
  goArgs.push_back("list");
  goArgs.push_back("-json=ImportPath,Module,Imports,"
                   "CgoFiles,CgoPkgConfig,CgoCFLAGS,CgoLDFLAGS,Error");
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
  if (modRoot != ".")
    workDir = srcDir + "/" + modRoot;

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
    std::string replacePath;
    std::string replaceVersion;
    std::vector<std::string> imports;
    std::vector<std::string> cgoPkgConfig;
    std::vector<std::string> cgoCflags;
    std::vector<std::string> cgoLdflags;
    bool isLocal = false;
    bool isCgo = false;
  };

  // First pass: parse JSON, collect third-party packages and replacements.
  std::vector<PkgData> thirdPartyPkgs;
  std::vector<std::string> pkgErrors;
  std::set<std::string> thirdPartyPaths;
  std::map<std::string, std::pair<std::string, std::string>> replMap;
  // Local replace directives: module path -> relative filesystem path.
  std::map<std::string, std::string> localReplMap;

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

    // Skip packages without a module (stdlib + non-module packages).
    // Go guarantees Standard implies Module == nil (see modload/build.go).
    if (!jpkg.contains("Module") || !jpkg["Module"].is_object())
      continue;

    auto &mod = jpkg["Module"];
    bool isMainModule = mod.value("Main", false);

    std::string replacePath;
    std::string replaceVersion;
    if (mod.contains("Replace") && mod["Replace"].is_object()) {
      auto &repl = mod["Replace"];
      replacePath = repl.value("Path", "");
      replaceVersion = repl.value("Version", "");
    }

    // A module is local if it's the main module or a local replace
    // (Replace exists with empty version). Matches go2nix's IsLocal().
    bool isLocal =
        isMainModule || (!replacePath.empty() && replaceVersion.empty());

    // Collect local replace paths (not main module) before skipping.
    if (isLocal) {
      if (!isMainModule && !replacePath.empty()) {
        auto modPath = mod.value("Path", "");
        localReplMap.try_emplace(modPath, replacePath);
      }
      continue;
    }

    auto importPath = jpkg.value("ImportPath", "");
    auto modPath = mod.value("Path", "");
    auto modVersion = mod.value("Version", "");

    // Collect remote replacements (deduplicated by modKey).
    // Use the replacement version in the key to match go2nix's lockfile
    // convention: the key reflects the effective (fetched) version, while
    // the [replace] section records the fetch path.
    if (!replacePath.empty()) {
      std::string effectiveVersion =
          replaceVersion.empty() ? modVersion : replaceVersion;
      std::string modKey = modPath + "@" + effectiveVersion;
      replMap.try_emplace(modKey, replacePath, replaceVersion);
    }

    thirdPartyPaths.insert(importPath);

    PkgData p;
    p.importPath = std::move(importPath);
    p.modPath = std::move(modPath);
    p.modVersion = std::move(modVersion);
    p.replacePath = std::move(replacePath);
    p.replaceVersion = std::move(replaceVersion);

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

    thirdPartyPkgs.push_back(std::move(p));
  }

  if (!pkgErrors.empty()) {
    std::string errMsg = "resolveGoPackages: package errors:\n";
    for (auto &e : pkgErrors)
      errMsg += "  - " + e + "\n";
    errMsg += "Hint: your GOMODCACHE may be stale. "
              "Run 'go mod download' to populate it.\n";
    state.error<EvalError>("%s", errMsg).atPos(pos).debugThrow();
  }

  // 8. Build replacements attrset.
  auto replacements = state.buildBindings(replMap.size());
  for (auto &[modKey, repl] : replMap) {
    auto replAttrs = state.buildBindings(2);
    replAttrs.alloc("path").mkString(repl.first, state.mem);
    replAttrs.alloc("version").mkString(repl.second, state.mem);
    replacements.alloc(modKey).mkAttrs(replAttrs.finish());
  }

  // 9. Build packages attrset (second pass: only over third-party packages).
  auto packages = state.buildBindings(thirdPartyPkgs.size());

  for (auto &p : thirdPartyPkgs) {
    // Use replacement version when available to match lockfile keys.
    std::string effectiveVersion =
        p.replaceVersion.empty() ? p.modVersion : p.replaceVersion;
    std::string modKey = p.modPath + "@" + effectiveVersion;

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

    std::string drvName =
        "gopkg-" + sanitize_name(p.importPath) + "-" + p.modVersion;

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

  // 10. Build localReplaces attrset: module path -> relative filesystem path.
  auto localReplaces = state.buildBindings(localReplMap.size());
  for (auto &[modPath, relPath] : localReplMap) {
    localReplaces.alloc(modPath).mkString(relPath, state.mem);
  }

  // Build result.
  auto result = state.buildBindings(3);
  result.alloc("localReplaces").mkAttrs(localReplaces.finish());
  result.alloc("packages").mkAttrs(packages.finish());
  result.alloc("replacements").mkAttrs(replacements.finish());
  v.mkAttrs(result.finish());
}

static RegisterPrimOp rp({
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
      - `modRoot` (optional): Subdirectory containing go.mod (default: ".")
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
      - `localReplaces`: attrset mapping module path to relative filesystem path
        for local replace directives (e.g. `replace mod => ../path` in go.mod).
        Only includes filesystem replaces, not versioned module replaces.

      Requires the host's GOMODCACHE to be populated (run `go mod download` first),
      unless `goProxy` is set to allow downloads.
    )",
    .impl = prim_resolveGoPackages,
});
