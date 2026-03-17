#pragma once

#include <nix/expr/eval.hh>
#include <string>

using namespace nix;

/// Sanitize an import path for use as a Nix derivation name.
/// '/' -> '-', '+' -> '_'  (matches helpers.nix)
std::string sanitize_name(const std::string &s);

/// Inherit specific environment variables from the current process.
/// Only copies vars named in `keys` that are actually set.
StringMap inheritEnv(const std::vector<std::string> &keys);

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
