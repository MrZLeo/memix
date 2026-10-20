#pragma once

#include "memix/view.hpp"

// Test fixtures explicitly perform two separate materialization steps.
// Production callers use select/compose; there is no single-owner build API.
inline memix::ViewResult<memix::ViewEnvironment>
compose_for_test(const memix::ViewService &views,
                 const memix::AuthoredViewExpression &expression,
                 const memix::BuildOptions &options = {}) {
  auto derivation = views.select(expression, options);
  if (!derivation)
    return std::unexpected(derivation.error());
  return views.compose({{.mount = {},
                         .derivation_id = derivation->id,
                         .output_role = "file_tree"}},
                       options);
}

inline memix::ViewResult<memix::ResolvedViewFile>
read_derivation_for_test(const memix::ViewService &views, std::string_view id,
                         const memix::fs::path &path) {
  auto result = views.read_output(
      {.kind = memix::ViewObjectKind::Derivation, .id = std::string(id)}, path);
  if (!result)
    return std::unexpected(result.error());
  return std::move(result->file);
}
