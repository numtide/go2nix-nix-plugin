#pragma once

#include <nix/expr/eval.hh>
#include <nlohmann/json.hpp>
#include <string>

using namespace nix;

/// Escape a Go module path: uppercase letters become '!' + lowercase.
/// See https://pkg.go.dev/golang.org/x/mod/module#EscapePath
std::string escape_mod_path(const std::string &path);

/// Sanitize an import path for use as a Nix derivation name.
/// '/' -> '-', '+' -> '_'  (matches helpers.nix)
std::string sanitize_name(const std::string &s);

/// Copy the current process environment into a StringMap.
StringMap copyCurrentEnviron();

/// Get a required string attribute from an attrset.
std::string getRequiredStringAttr(EvalState &state, Value &attrs,
                                  const PosIdx pos, const char *name,
                                  NixStringContext &context);

/// Get an optional list-of-strings attribute, returning a default.
Strings getOptionalStringListAttr(EvalState &state, Value &attrs,
                                  const PosIdx pos, const char *name,
                                  const Strings &defaultVal);

/// Get an optional string attribute, returning a default.
std::string getOptionalStringAttr(EvalState &state, Value &attrs,
                                  const PosIdx pos, const char *name,
                                  const std::string &defaultVal);
