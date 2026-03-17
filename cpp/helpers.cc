#include "helpers.h"

#include <cstdlib>
#include <vector>

std::string sanitize_name(const std::string &s) {
  std::string result = s;
  for (auto &c : result) {
    if (c == '/')
      c = '-';
    else if (c == '+')
      c = '_';
  }
  return result;
}

StringMap inheritEnv(const std::vector<std::string> &keys) {
  StringMap env;
  for (auto &key : keys) {
    const char *val = std::getenv(key.c_str());
    if (val)
      env[key] = val;
  }
  return env;
}

std::string getRequiredStringAttr(EvalState &state, Value &attrs,
                                  const PosIdx pos, const char *name,
                                  NixStringContext &context) {
  auto sym = state.symbols.create(name);
  auto *attr = attrs.attrs()->get(sym);
  if (!attr)
    state.error<EvalError>("missing required attribute '%s'", name)
        .atPos(pos)
        .debugThrow();
  return state
      .coerceToString(pos, *attr->value, context,
                      fmt("while evaluating the '%s' attribute", name), false,
                      false)
      .toOwned();
}

Strings getOptionalStringListAttr(EvalState &state, Value &attrs,
                                  const PosIdx pos, const char *name,
                                  const Strings &defaultVal) {
  auto sym = state.symbols.create(name);
  auto *attr = attrs.attrs()->get(sym);
  if (!attr)
    return defaultVal;

  state.forceList(*attr->value, pos,
                  fmt("while evaluating the '%s' attribute", name));

  Strings result;
  for (auto *elem : attr->value->listView()) {
    NixStringContext dummy;
    result.push_back(
        state
            .coerceToString(pos, *elem, dummy,
                            fmt("while evaluating an element of '%s'", name),
                            false, false)
            .toOwned());
  }
  return result;
}

std::string getOptionalStringAttr(EvalState &state, Value &attrs,
                                  const PosIdx pos, const char *name,
                                  const std::string &defaultVal) {
  auto sym = state.symbols.create(name);
  auto *attr = attrs.attrs()->get(sym);
  if (!attr)
    return defaultVal;
  NixStringContext dummy;
  return state
      .coerceToString(pos, *attr->value, dummy,
                      fmt("while evaluating the '%s' attribute", name), false,
                      false)
      .toOwned();
}
