#include "task.h"

#include <algorithm>
#include <array>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "clang/AST/AST.h"
#include "clang/Lex/Lexer.h"

#include "nlohmann/json.hpp"

#include "async_mmap.h"
#include "mmap.h"
#include "stream.h"

using std::array;
using std::binary_search;
using std::initializer_list;
using std::make_shared;
using std::pair;
using std::shared_ptr;
using std::sort;
using std::string;
using std::to_string;
using std::unordered_map;
using std::vector;

using clang::CharSourceRange;
using clang::CXXMemberCallExpr;
using clang::CXXMethodDecl;
using clang::CXXOperatorCallExpr;
using clang::DeclGroupRef;
using clang::DeclRefExpr;
using clang::DeclStmt;
using clang::DoStmt;
using clang::ElaboratedType;
using clang::Expr;
using clang::ExprWithCleanups;
using clang::ForStmt;
using clang::FunctionDecl;
using clang::FunctionProtoType;
using clang::IntegerLiteral;
using clang::Lexer;
using clang::LValueReferenceType;
using clang::MemberExpr;
using clang::PrintingPolicy;
using clang::RecordType;
using clang::SourceLocation;
using clang::SourceRange;
using clang::Stmt;
using clang::StringLiteral;
using clang::TemplateArgument;
using clang::TemplateSpecializationType;
using clang::VarDecl;
using clang::WhileStmt;

using llvm::dyn_cast;
using llvm::join;
using llvm::StringRef;

using nlohmann::json;

namespace tapa {
namespace internal {

extern const string* top_name;

// Get a string representation of the function signature a stream operation.
std::string GetSignature(const CXXMemberCallExpr* call_expr) {
  auto target = call_expr->getDirectCallee();
  assert(target != nullptr);

  if (const auto instantiated = target->getTemplateInstantiationPattern()) {
    target = instantiated;
  }

  string signature{target->getQualifiedNameAsString()};

  signature += "(";

  for (auto param : target->parameters()) {
    PrintingPolicy policy{{}};
    policy.Bool = true;
    signature.append(param->getType().getAsString(policy));
    signature += ", ";
  }

  if (target->isVariadic()) {
    signature += ("...");
  } else if (target->getNumParams() > 0) {
    signature.resize(signature.size() - 2);
  }
  signature += ")";

  if (auto target_type =
          dyn_cast<FunctionProtoType>(target->getType().getTypePtr())) {
    if (target_type->isConst()) signature.append(" const");
    if (target_type->isVolatile()) signature.append(" volatile");
    if (target_type->isRestrict()) signature.append(" restrict");

    switch (target_type->getRefQualifier()) {
      case clang::RQ_LValue:
        signature.append(" &");
        break;
      case clang::RQ_RValue:
        signature.append(" &&");
        break;
      default:
        break;
    }
  }

  return signature;
}

// Given a Stmt, find the first tapa::task in its children.
const ExprWithCleanups* GetTapaTask(const Stmt* stmt) {
  for (auto child : stmt->children()) {
    if (auto expr = dyn_cast<ExprWithCleanups>(child)) {
      if (expr->getType().getAsString() == "struct tapa::task") {
        return expr;
      }
    }
  }
  return nullptr;
}

// Given a Stmt, find all tapa::task::invoke's via DFS and update invokes.
void GetTapaInvokes(const Stmt* stmt,
                    vector<const CXXMemberCallExpr*>& invokes) {
  for (auto child : stmt->children()) {
    GetTapaInvokes(child, invokes);
  }
  if (const auto invoke = dyn_cast<CXXMemberCallExpr>(stmt)) {
    if (invoke->getRecordDecl()->getQualifiedNameAsString() == "tapa::task" &&
        invoke->getMethodDecl()->getNameAsString() == "invoke") {
      invokes.push_back(invoke);
    }
  }
}

// Given a Stmt, return all tapa::task::invoke's via DFS.
vector<const CXXMemberCallExpr*> GetTapaInvokes(const Stmt* stmt) {
  vector<const CXXMemberCallExpr*> invokes;
  GetTapaInvokes(stmt, invokes);
  return invokes;
}

// Return all loops that do not contain other loops but do contain FIFO
// operations.
void GetInnermostLoops(const Stmt* stmt, vector<const Stmt*>& loops) {
  for (auto child : stmt->children()) {
    if (child != nullptr) {
      GetInnermostLoops(child, loops);
    }
  }
  if (RecursiveInnermostLoopsVisitor().IsInnermostLoop(stmt)) {
    loops.push_back(stmt);
  }
}
vector<const Stmt*> GetInnermostLoops(const Stmt* stmt) {
  vector<const Stmt*> loops;
  GetInnermostLoops(stmt, loops);
  return loops;
}

thread_local const FunctionDecl* Visitor::current_task{nullptr};

namespace {

void AddPragmaForStream(
    const clang::ParmVarDecl* param,
    const std::function<void(initializer_list<StringRef>)>& add) {
  assert(IsTapaType(param, "(i|o)streams?"));
  const auto name = param->getNameAsString();
  add({"disaggregate variable =", name});

  vector<string> names;
  if (IsTapaType(param, "(i|o)streams")) {
    add({"array_partition variable =", name, "complete"});
    const int64_t array_size = GetArraySize(param);
    names.reserve(array_size);
    for (int64_t i = 0; i < array_size; ++i) {
      names.push_back(ArrayNameAt(name, i));
    }
  } else {
    names.push_back(name);
  }

  for (const auto& name : names) {
    const auto fifo_var = GetFifoVar(name);
    add({"interface ap_fifo port =", fifo_var});
    add({"aggregate variable =", fifo_var, "bit"});
    if (IsTapaType(param, "istreams?")) {
      const auto peek_var = GetPeekVar(name);
      add({"interface ap_fifo port =", peek_var});
      add({"aggregate variable =", peek_var, "bit"});
    }
  }
}

}  // namespace

// Apply tapa s2s transformations on a function.
bool Visitor::VisitFunctionDecl(FunctionDecl* func) {
  if (func->hasBody() && func->isGlobal() &&
      context_.getSourceManager().isWrittenInMainFile(func->getBeginLoc())) {
    if (rewriters_.size() == 0) {
      funcs_.push_back(func);
    } else {
      if (rewriters_.count(func) > 0) {
        if (func == current_task) {
          if (auto task = GetTapaTask(func->getBody())) {
            // Run this before "extern C" is injected by
            // `ProcessUpperLevelTask`.
            if (*top_name == func->getNameAsString()) {
              metadata_[func]["frt_interface"] = GetFrtInterface(func);
            }
            ProcessUpperLevelTask(task, func);
          } else {
            ProcessLowerLevelTask(func);
          }
        } else {
          GetRewriter().RemoveText(func->getSourceRange());
        }
      }
    }
  }
  // Let the recursion continue.
  return true;
}

// Insert `#pragma HLS ...` after the token specified by loc.
bool Visitor::InsertHlsPragma(const SourceLocation& loc, const string& pragma,
                              const vector<pair<string, string>>& args) {
  string line{"\n#pragma HLS " + pragma};
  for (const auto& arg : args) {
    line += " " + arg.first;
    if (!arg.second.empty()) {
      line += " = " + arg.second;
    }
  }
  line += "\n";
  return GetRewriter().InsertTextAfterToken(loc, line);
}

// Apply tapa s2s transformations on a upper-level task.
void Visitor::ProcessUpperLevelTask(const ExprWithCleanups* task,
                                    const FunctionDecl* func) {
  const auto func_body = func->getBody();
  // TODO: implement qdma streams

  // Replace mmaps arguments with 64-bit base addresses.
  for (const auto param : func->parameters()) {
    const string param_name = param->getNameAsString();
    if (IsTapaType(param, "(async_)?mmap")) {
      GetRewriter().ReplaceText(
          param->getTypeSourceInfo()->getTypeLoc().getSourceRange(),
          "uint64_t");
    } else if (IsTapaType(param, "(async_)?mmaps")) {
      string rewritten_text;
      for (int i = 0; i < GetArraySize(param); ++i) {
        if (!rewritten_text.empty()) rewritten_text += ", ";
        rewritten_text += "uint64_t " + GetArrayElem(param_name, i);
      }
      GetRewriter().ReplaceText(param->getSourceRange(), rewritten_text);
    }
  }

  // Add pragmas.
  string replaced_body{"{\n"};  // TODO: replace with vector<string> lines
  for (const auto param : func->parameters()) {
    auto param_name = param->getNameAsString();
    auto add_pragma = [&](string port = "") {
      if (port.empty()) port = param_name;
      replaced_body += "#pragma HLS interface s_axilite port = " + port +
                       " bundle = control\n";
    };
    if (IsTapaType(param, "(i|o)streams?")) {
      AddPragmaForStream(
          param, [&replaced_body](initializer_list<StringRef> args) {
            replaced_body += "#pragma HLS " + join(args, " ") + "\n";
          });
    } else if (*top_name == func->getNameAsString()) {
      if (IsTapaType(param, "(async_)?mmaps")) {
        // For top-level mmaps and scalars, generate AXI base addresses.
        for (int i = 0; i < GetArraySize(param); ++i) {
          add_pragma(GetArrayElem(param_name, i));
        }
      } else {
        add_pragma();
      }
    } else {
      // Make sure ap_clk and ap_rst_n are generated for middle-level mmaps and
      // scalars.
      replaced_body +=
          "#pragma HLS interface ap_none port = " + param_name + " register\n";
    }

    replaced_body += "\n";  // Separate pragmas for each parameter.
  }
  if (*top_name == func->getNameAsString()) {
    replaced_body +=
        "#pragma HLS interface s_axilite port = return bundle = control\n";
  }
  replaced_body += "\n";

  // Add dummy reads and/or writes.
  for (const auto param : func->parameters()) {
    auto param_name = param->getNameAsString();
    if (IsStreamInterface(param)) {
      if (IsTapaType(param, "istream")) {
        replaced_body += "{ auto val = " + param_name + ".read(); }\n";
      } else if (IsTapaType(param, "ostream")) {
        replaced_body +=
            param_name + ".write(" + GetStreamElemType(param) + "());\n";
      }
    } else if (IsTapaType(param, "istreams")) {
      for (int i = 0; i < GetArraySize(param); ++i) {
        replaced_body +=
            "{ auto val = " + ArrayNameAt(param_name, i) + ".read(); }\n";
      }
    } else if (IsTapaType(param, "ostreams")) {
      for (int i = 0; i < GetArraySize(param); ++i) {
        replaced_body += ArrayNameAt(param_name, i) + ".write(" +
                         GetStreamElemType(param) + "());\n";
      }
    } else if (IsTapaType(param, "(async_)?mmaps")) {
      for (int i = 0; i < GetArraySize(param); ++i) {
        replaced_body += "{ auto val = reinterpret_cast<volatile uint8_t&>(" +
                         GetArrayElem(param_name, i) + "); }\n";
      }
    } else {
      auto elem_type = param->getType();
      const bool is_const = elem_type.isConstQualified();
      replaced_body += "{ auto val = reinterpret_cast<volatile ";
      if (is_const) {
        replaced_body += "const ";
      }
      replaced_body += "uint8_t&>(" + param_name + "); }\n";
    }
  }

  replaced_body += "}\n";

  // We need a empty shell.
  GetRewriter().ReplaceText(func_body->getSourceRange(), replaced_body);

  // Obtain the connection schema from the task.
  // metadata: {tasks, fifos}
  // tasks: {task_name: [{step, {args: port_name: {var_type, var_name}}}]}
  // fifos: {fifo_name: {depth, produced_by, consumed_by}}
  auto& metadata = GetMetadata();
  metadata["fifos"] = json::object();

  for (const auto param : func->parameters()) {
    const auto param_name = param->getNameAsString();
    auto add_mmap_meta = [&](const string& name) {
      metadata["ports"].push_back(
          {{"name", name},
           {"cat", IsTapaType(param, "async_mmaps?") ? "async_mmap" : "mmap"},
           {"width",
            context_
                .getTypeInfo(GetTemplateArg(param->getType(), 0)->getAsType())
                .Width},
           {"type", GetMmapElemType(param) + "*"}});
    };
    if (IsTapaType(param, "(async_)?mmap")) {
      add_mmap_meta(param_name);
    } else if (IsTapaType(param, "(async_)?mmaps")) {
      for (int i = 0; i < GetArraySize(param); ++i) {
        add_mmap_meta(param_name + "[" + to_string(i) + "]");
      }
    } else if (IsStreamInterface(param)) {
      // TODO
    } else {
      metadata["ports"].push_back(
          {{"name", param_name},
           {"cat", "scalar"},
           {"width", context_.getTypeInfo(param->getType()).Width},
           {"type", param->getType().getAsString()}});
    }
  }

  // Process stream declarations.
  unordered_map<string, const VarDecl*> fifo_decls;
  for (const auto child : func_body->children()) {
    if (const auto decl_stmt = dyn_cast<DeclStmt>(child)) {
      if (const auto var_decl = dyn_cast<VarDecl>(*decl_stmt->decl_begin())) {
        if (auto decl = GetTapaStreamDecl(var_decl->getType())) {
          const auto args = decl->getTemplateArgs().asArray();
          const string elem_type = GetTemplateArgName(args[0]);
          const uint64_t fifo_depth{*args[1].getAsIntegral().getRawData()};
          const string var_name{var_decl->getNameAsString()};
          metadata["fifos"][var_name]["depth"] = fifo_depth;
          fifo_decls[var_name] = var_decl;
        } else if (auto decl = GetTapaStreamsDecl(var_decl->getType())) {
          const auto args = decl->getTemplateArgs().asArray();
          const string elem_type = GetTemplateArgName(args[0]);
          const uint64_t fifo_depth = *args[2].getAsIntegral().getRawData();
          for (int i = 0; i < GetArraySize(decl); ++i) {
            const string var_name = ArrayNameAt(var_decl->getNameAsString(), i);
            metadata["fifos"][var_name]["depth"] = fifo_depth;
            fifo_decls[var_name] = var_decl;
          }
        }
      }
    }
  }

  // Instanciate tasks.
  vector<const CXXMemberCallExpr*> invokes = GetTapaInvokes(task);

  for (auto invoke : invokes) {
    int step = -1;
    bool has_name = false;
    bool is_vec = false;
    uint64_t vec_length = 1;
    if (const auto method = dyn_cast<CXXMethodDecl>(invoke->getCalleeDecl())) {
      auto args = method->getTemplateSpecializationArgs()->asArray();
      step =
          *reinterpret_cast<const int*>(args[0].getAsIntegral().getRawData());
      if (args.size() > 1 && args[1].getKind() == TemplateArgument::Integral) {
        is_vec = true;
        vec_length = *args[1].getAsIntegral().getRawData();
      }
      if (args.rbegin()->getKind() == TemplateArgument::Integral) {
        has_name = true;
      }
    } else {
      static const auto diagnostic_id =
          this->context_.getDiagnostics().getCustomDiagID(
              clang::DiagnosticsEngine::Error, "unexpected invocation: %0");
      this->context_.getDiagnostics()
          .Report(invoke->getCallee()->getBeginLoc(), diagnostic_id)
          .AddString(invoke->getStmtClassName());
    }
    const FunctionDecl* task = nullptr;
    string task_name;
    auto get_name = [&](const string& name, uint64_t i,
                        const DeclRefExpr* decl_ref) -> string {
      if (is_vec && IsTapaType(decl_ref, "(async_mmaps|streams)")) {
        const auto ts_type =
            decl_ref->getType()->getAs<TemplateSpecializationType>();
        assert(ts_type != nullptr);
        assert(ts_type->getNumArgs() > 1);
        const auto length = this->EvalAsInt(ts_type->getArg(1).getAsExpr());
        if (i >= length) {
          auto& diagnostics = context_.getDiagnostics();
          static const auto diagnostic_id =
              diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Remark,
                                          "invocation #%0 accesses '%1[%2]'");
          auto diagnostics_builder =
              diagnostics.Report(decl_ref->getBeginLoc(), diagnostic_id);
          diagnostics_builder.AddString(to_string(i));
          diagnostics_builder.AddString(decl_ref->getNameInfo().getAsString());
          diagnostics_builder.AddString(to_string(i % length));
          diagnostics_builder.AddString(decl_ref->getType().getAsString());
          diagnostics_builder.AddSourceRange(
              GetCharSourceRange(decl_ref->getSourceRange()));
        }
        return ArrayNameAt(name, i % length);
      }
      return name;
    };
    for (uint64_t i_vec = 0; i_vec < vec_length; ++i_vec) {
      for (unsigned i = 0; i < invoke->getNumArgs(); ++i) {
        const auto arg = invoke->getArg(i);
        const auto decl_ref = dyn_cast<DeclRefExpr>(arg);  // a variable
        clang::Expr::EvalResult arg_eval_as_int_result;
        const bool arg_is_int =
            arg->EvaluateAsInt(arg_eval_as_int_result, this->context_);
        const auto op_call =
            dyn_cast<CXXOperatorCallExpr>(arg);  // element in an array
        const auto arg_is_seq = IsTapaType(arg, "seq");
        if (decl_ref || op_call || arg_is_int || arg_is_seq) {
          string arg_name;
          if (decl_ref) {
            arg_name = decl_ref->getNameInfo().getName().getAsString();
          }
          if (op_call) {
            const auto array_name = dyn_cast<DeclRefExpr>(op_call->getArg(0))
                                        ->getNameInfo()
                                        .getName()
                                        .getAsString();
            const auto array_idx = this->EvalAsInt(op_call->getArg(1));
            arg_name = ArrayNameAt(array_name, array_idx);
          }
          if (arg_is_int) {
            arg_name = "64'd" +
                       std::to_string(uint64_t(
                           arg_eval_as_int_result.Val.getInt().getExtValue()));
          }
          if (i == 0) {
            task_name = arg_name;
            metadata["tasks"][task_name].push_back({{"step", step}});
            task = decl_ref->getDecl()->getAsFunction();
          } else {
            assert(task != nullptr);
            auto param = task->getParamDecl(has_name ? i - 2 : i - 1);
            auto param_name = param->getNameAsString();
            string param_cat;

            // register this argument to task
            auto register_arg = [&](string arg = "", string port = "") {
              if (arg.empty())
                arg = arg_name;  // use global arg_name by default
              if (port.empty()) port = param_name;
              (*metadata["tasks"][task_name].rbegin())["args"][port] = {
                  {"cat", param_cat}, {"arg", arg}};
            };

            // regsiter stream info to task
            auto register_consumer = [&, ast_arg = arg](string arg = "") {
              // use global arg_name by default
              if (arg.empty()) arg = arg_name;
              if (metadata["fifos"][arg].contains("consumed_by")) {
                static const auto diagnostic_id =
                    this->context_.getDiagnostics().getCustomDiagID(
                        clang::DiagnosticsEngine::Error,
                        "tapa::stream '%0' consumed more than once");
                auto diagnostics_builder =
                    this->context_.getDiagnostics().Report(
                        ast_arg->getBeginLoc(), diagnostic_id);
                diagnostics_builder.AddString(arg);
                diagnostics_builder.AddSourceRange(GetCharSourceRange(ast_arg));
              }
              metadata["fifos"][arg]["consumed_by"] = {
                  task_name, metadata["tasks"][task_name].size() - 1};
            };
            auto register_producer = [&, ast_arg = arg](string arg = "") {
              // use global arg_name by default
              if (arg.empty()) arg = arg_name;
              if (metadata["fifos"][arg].contains("produced_by")) {
                static const auto diagnostic_id =
                    this->context_.getDiagnostics().getCustomDiagID(
                        clang::DiagnosticsEngine::Error,
                        "tapa::stream '%0' produced more than once");
                auto diagnostics_builder =
                    this->context_.getDiagnostics().Report(
                        ast_arg->getBeginLoc(), diagnostic_id);
                diagnostics_builder.AddString(arg);
                diagnostics_builder.AddSourceRange(GetCharSourceRange(ast_arg));
              }
              metadata["fifos"][arg]["produced_by"] = {
                  task_name, metadata["tasks"][task_name].size() - 1};
            };
            if (IsTapaType(param, "mmap")) {
              param_cat = "mmap";
              register_arg(get_name(arg_name, i_vec, decl_ref));
            } else if (IsTapaType(param, "async_mmap")) {
              param_cat = "async_mmap";
              // vector invocation can map async_mmaps to async_mmap
              register_arg(get_name(arg_name, i_vec, decl_ref));
            } else if (IsTapaType(param, "istream")) {
              param_cat = "istream";
              // vector invocation can map istreams to istream
              auto arg = get_name(arg_name, i_vec, decl_ref);
              register_consumer(arg);
              register_arg(arg);
            } else if (IsTapaType(param, "ostream")) {
              param_cat = "ostream";
              // vector invocation can map ostreams to ostream
              auto arg = get_name(arg_name, i_vec, decl_ref);
              register_producer(arg);
              register_arg(arg);
            } else if (IsTapaType(param, "istreams")) {
              param_cat = "istream";
              for (int i = 0; i < GetArraySize(param); ++i) {
                auto arg = ArrayNameAt(arg_name, i);
                register_consumer(arg);
                register_arg(arg, ArrayNameAt(param_name, i));
              }
            } else if (IsTapaType(param, "ostreams")) {
              param_cat = "ostream";
              for (int i = 0; i < GetArraySize(param); ++i) {
                auto arg = ArrayNameAt(arg_name, i);
                register_producer(arg);
                register_arg(arg, ArrayNameAt(param_name, i));
              }
            } else if (arg_is_seq) {
              param_cat = "scalar";
              register_arg("64'd" + std::to_string(i_vec));
            } else {
              param_cat = "scalar";
              register_arg();
            }
          }
          continue;
        } else if (const auto string_literal = dyn_cast<StringLiteral>(arg)) {
          if (i == 1 && has_name) {
            (*metadata["tasks"][task_name].rbegin())["name"] =
                string_literal->getString();
            continue;
          }
        }
        static const auto diagnostic_id =
            this->context_.getDiagnostics().getCustomDiagID(
                clang::DiagnosticsEngine::Error, "unexpected argument: %0");
        auto diagnostics_builder = this->context_.getDiagnostics().Report(
            arg->getBeginLoc(), diagnostic_id);
        diagnostics_builder.AddString(arg->getStmtClassName());
        diagnostics_builder.AddSourceRange(GetCharSourceRange(arg));
      }
    }
  }

  for (auto fifo = metadata["fifos"].begin();
       fifo != metadata["fifos"].end();) {
    const auto is_consumed = fifo.value().contains("consumed_by");
    const auto is_produced = fifo.value().contains("produced_by");
    const auto& fifo_name = fifo.key();
    const auto fifo_decl = fifo_decls.find(fifo_name);
    auto& diagnostics = context_.getDiagnostics();
    if (!is_consumed && !is_produced) {
      static const auto diagnostic_id = diagnostics.getCustomDiagID(
          clang::DiagnosticsEngine::Warning, "unused stream: %0");
      auto diagnostics_builder =
          diagnostics.Report(fifo_decl->second->getBeginLoc(), diagnostic_id);
      diagnostics_builder.AddString(fifo_name);
      diagnostics_builder.AddSourceRange(
          GetCharSourceRange(fifo_decl->second->getSourceRange()));
      fifo = metadata["fifos"].erase(fifo);
    } else {
      ++fifo;
      if (fifo_decl != fifo_decls.end() && is_consumed != is_produced) {
        static const auto consumed_diagnostic_id =
            diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "consumed but not produced stream: %0");
        static const auto produced_diagnostic_id =
            diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "produced but not consumed stream: %0");
        auto diagnostics_builder = diagnostics.Report(
            fifo_decl->second->getBeginLoc(),
            is_consumed ? consumed_diagnostic_id : produced_diagnostic_id);
        diagnostics_builder.AddString(fifo_name);
        diagnostics_builder.AddSourceRange(
            GetCharSourceRange(fifo_decl->second->getSourceRange()));
      }
    }
  }

  if (*top_name == func->getNameAsString()) {
    // SDAccel only works with extern C kernels.
    GetRewriter().InsertText(func->getBeginLoc(), "extern \"C\" {\n\n");
    GetRewriter().InsertTextAfterToken(func->getEndLoc(),
                                       "\n\n}  // extern \"C\"\n");
  }
}

// Apply tapa s2s transformations on a lower-level task.
void Visitor::ProcessLowerLevelTask(const FunctionDecl* func) {
  for (const auto param : func->parameters()) {
    vector<string> lines = {""};  // Make sure pragmas start with a new line.
    auto add = [&lines](initializer_list<StringRef> args) {
      lines.push_back("#pragma HLS " + join(args, " "));
    };

    const auto name = param->getNameAsString();
    if (IsTapaType(param, "(i|o)streams?")) {
      AddPragmaForStream(param, add);
    } else if (IsTapaType(param, "async_mmap")) {
      add({"disaggregate variable =", name});
      for (auto tag : {"read_addr", "read_data", "read_peek", "write_addr",
                       "write_data"}) {
        add({"interface ap_fifo port =", name, ".", tag});
        add({"aggregate variable =", name, " . ", tag, " bit"});
      }
    } else if (IsTapaType(param, "mmap")) {
      add({"interface m_axi port =", name, "offset = direct bundle =", name});
    }

    lines.push_back("");  // Make sure pragmas finish with a new line.
    GetRewriter().InsertTextAfterToken(func->getBody()->getBeginLoc(),
                                       join(lines, "\n"));
  }
}

string Visitor::GetFrtInterface(const FunctionDecl* func) {
  auto func_body_source_range = func->getBody()->getSourceRange();
  auto& source_manager = context_.getSourceManager();
  auto main_file_id = source_manager.getMainFileID();

  vector<string> content;
  content.reserve(5 + func->getNumParams());

  // Content before the function body.
  content.push_back(join(
      initializer_list<StringRef>{
          "#include <sstream>",
          "#include <stdexcept>",
          "#include <frt.h>",
          "\n",
      },
      "\n"));
  content.push_back(GetRewriter().getRewrittenText(
      SourceRange(source_manager.getLocForStartOfFile(main_file_id),
                  func_body_source_range.getBegin())));

  // Function body.
  content.push_back(join(
      initializer_list<StringRef>{
          "\n#define TAPAB_APP \"TAPAB_",
          func->getNameAsString(),
          "\"\n",
      },
      ""));
  content.push_back(R"(#define TAPAB "TAPAB"
  const char* _tapa_bitstream = nullptr;
  if ((_tapa_bitstream = getenv(TAPAB_APP)) ||
      (_tapa_bitstream = getenv(TAPAB))) {
    fpga::Instance _tapa_instance(_tapa_bitstream);
    int _tapa_arg_index = 0;
    for (const auto& _tapa_arg_info : _tapa_instance.GetArgsInfo()) {
      if (false) {)");
  for (auto param : func->parameters()) {
    const auto name = param->getNameAsString();
    if (IsTapaType(param, "(async_)?mmaps?")) {
      // TODO: Leverage kernel information.
      bool write_device = true;
      bool read_device =
          !GetTemplateArg(param->getType(), 0)->getAsType().isConstQualified();
      auto direction =
          write_device ? (read_device ? "ReadWrite" : "WriteOnly") : "ReadOnly";
      auto add_param = [&content, direction](StringRef name, StringRef var) {
        content.push_back(join(
            initializer_list<StringRef>{
                R"(
      } else if (_tapa_arg_info.name == ")",
                name,
                R"(") {
        auto _tapa_arg = fpga::)",
                direction,
                R"(()",
                var,
                R"(.get(), )",
                var,
                R"(.size());
        _tapa_instance.AllocBuf(_tapa_arg_index, _tapa_arg);
        _tapa_instance.SetArg(_tapa_arg_index, _tapa_arg);)",
            },
            ""));
      };
      if (IsTapaType(param, "(async_)?mmaps")) {
        const uint64_t array_size = GetArraySize(param);
        for (uint64_t i = 0; i < array_size; ++i) {
          add_param(GetArrayElem(name, i), ArrayNameAt(name, i));
        }
      } else {
        add_param(name, name);
      }
    } else if (IsTapaType(param, "(i|o)streams?")) {
      content.push_back("\n#error stream not supported yet\n");
    } else {
      content.push_back(join(
          initializer_list<StringRef>{
              R"(
      } else if (_tapa_arg_info.name == ")",
              name,
              R"(") {
        _tapa_instance.SetArg(_tapa_arg_index, )",
              name,
              R"();)",
          },
          ""));
    }
  }
  content.push_back(
      R"(
      } else {
        std::stringstream ss;
        ss << "unknown argument: " << _tapa_arg_info;
        throw std::runtime_error(ss.str());
      }
      ++_tapa_arg_index;
    }
    _tapa_instance.WriteToDevice();
    _tapa_instance.Exec();
    _tapa_instance.ReadFromDevice();
    _tapa_instance.Finish();
  } else {
    throw std::runtime_error("no bitstream found; please set `" TAPAB_APP
                             "` or `" TAPAB "`");
  }
)");

  // Content after the function body.
  content.push_back(GetRewriter().getRewrittenText(
      SourceRange(func_body_source_range.getEnd(),
                  source_manager.getLocForEndOfFile(main_file_id))));

  // Join everything together (without excessive copying).
  return llvm::join(content.begin(), content.end(), "");
}

SourceLocation Visitor::GetEndOfLoc(SourceLocation loc) {
  return loc.getLocWithOffset(Lexer::MeasureTokenLength(
      loc, GetRewriter().getSourceMgr(), GetRewriter().getLangOpts()));
}
CharSourceRange Visitor::GetCharSourceRange(SourceRange range) {
  return CharSourceRange::getCharRange(range.getBegin(),
                                       GetEndOfLoc(range.getEnd()));
}
CharSourceRange Visitor::GetCharSourceRange(const Stmt* stmt) {
  return GetCharSourceRange(stmt->getSourceRange());
}

int64_t Visitor::EvalAsInt(const Expr* expr) {
  clang::Expr::EvalResult result;
  if (expr->EvaluateAsInt(result, this->context_)) {
    return result.Val.getInt().getExtValue();
  }
  static const auto diagnostic_id =
      this->context_.getDiagnostics().getCustomDiagID(
          clang::DiagnosticsEngine::Error,
          "fail to evaluate as integer at compile time");
  this->context_.getDiagnostics()
      .Report(expr->getBeginLoc(), diagnostic_id)
      .AddSourceRange(this->GetCharSourceRange(expr));
  return -1;
}

}  // namespace internal
}  // namespace tapa
