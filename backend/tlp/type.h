#ifndef TLP_TYPE_H_
#define TLP_TYPE_H_

#include <sstream>
#include <string>
#include <type_traits>

#include "clang/AST/AST.h"

inline std::string GetTemplateArgName(const clang::TemplateArgument& arg) {
  std::string name;
  llvm::raw_string_ostream oss{name};
  clang::LangOptions options;
  options.CPlusPlus = true;
  options.Bool = true;
  arg.print(options, oss);
  return name;
}

struct ObjectInfo {
  ObjectInfo(const std::string& name, const std::string& type)
      : name(name), type(type) {
    if (this->type.substr(0, 7) == "struct ") {
      this->type.erase(0, 7);
    } else if (this->type.substr(0, 6) == "class ") {
      this->type.erase(0, 6);
    }
  }
  std::string name;
  std::string type;
};

// Given the AST root stmt, returns the name of the first node that has type T
// via DFS, or empty string if no such node is found.
template <typename T>
inline std::string GetNameOfFirst(
    const clang::Stmt* stmt,
    std::shared_ptr<std::unordered_map<const clang::Stmt*, bool>> visited =
        nullptr) {
  if (visited == nullptr) {
    visited = std::make_shared<decltype(visited)::element_type>();
  }
  if ((*visited)[stmt]) {
    // If stmt is visited for the second, it must have returned empty string
    // last time.
    return "";
  }
  (*visited)[stmt] = true;

  if (const auto expr = llvm::dyn_cast<T>(stmt)) {
    return expr->getNameInfo().getAsString();
  }
  for (const auto child : stmt->children()) {
    if (stmt == nullptr) {
      continue;
    }
    const auto child_result = GetNameOfFirst<T>(child, visited);
    if (!child_result.empty()) {
      return child_result;
    }
  }
  return "";
}

// For enum classes.
template <typename U, typename V>
inline constexpr typename std::enable_if<std::is_enum<U>::value, U>::type
operator|(U lhs, V rhs) {
  return static_cast<U>(
      static_cast<typename std::underlying_type<U>::type>(lhs) |
      static_cast<typename std::underlying_type<V>::type>(rhs));
}
template <typename U, typename V>
inline constexpr typename std::enable_if<std::is_enum<U>::value, U>::type
operator&(U lhs, V rhs) {
  return static_cast<U>(
      static_cast<typename std::underlying_type<U>::type>(lhs) &
      static_cast<typename std::underlying_type<V>::type>(rhs));
}

#endif  // TLP_TYPE_H_