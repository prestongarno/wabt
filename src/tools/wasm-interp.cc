/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "binary-reader-interpreter.h"
#include "binary-reader.h"
#include "error-handler.h"
#include "interpreter.h"
#include "literal.h"
#include "option-parser.h"
#include "stream.h"
#include "wast-lexer.h"
#include "wast-parser.h"

using namespace wabt;
using namespace wabt::interpreter;

#define V(name, str) str,
static const char* s_trap_strings[] = {FOREACH_INTERPRETER_RESULT(V)};
#undef V

static int s_verbose;
static const char* s_infile;
static ReadBinaryOptions s_read_binary_options;
static Thread::Options s_thread_options;
static bool s_trace;
static bool s_spec;
static bool s_run_all_exports;

static std::unique_ptr<FileStream> s_log_stream;
static std::unique_ptr<FileStream> s_stdout_stream;

enum class RunVerbosity {
  Quiet = 0,
  Verbose = 1,
};

static const char s_description[] =
R"(  read a file in the wasm binary format, and run in it a stack-based
  interpreter.

examples:
  # parse binary file test.wasm, and type-check it
  $ wasm-interp test.wasm

  # parse test.wasm and run all its exported functions
  $ wasm-interp test.wasm --run-all-exports

  # parse test.wasm, run the exported functions and trace the output
  $ wasm-interp test.wasm --run-all-exports --trace

  # parse test.json and run the spec tests
  $ wasm-interp test.json --spec

  # parse test.wasm and run all its exported functions, setting the
  # value stack size to 100 elements
  $ wasm-interp test.wasm -V 100 --run-all-exports
)";

static void parse_options(int argc, char** argv) {
  OptionParser parser("wasm-interp", s_description);

  parser.AddOption('v', "verbose", "Use multiple times for more info", []() {
    s_verbose++;
    s_log_stream = FileStream::CreateStdout();
    s_read_binary_options.log_stream = s_log_stream.get();
  });
  parser.AddHelpOption();
  parser.AddOption('V', "value-stack-size", "SIZE",
                   "Size in elements of the value stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.value_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('C', "call-stack-size", "SIZE",
                   "Size in elements of the call stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.call_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('t', "trace", "Trace execution", []() { s_trace = true; });
  parser.AddOption("spec", "Run spec tests (input file should be .json)",
                   []() { s_spec = true; });
  parser.AddOption(
      "run-all-exports",
      "Run all the exported functions, in order. Useful for testing",
      []() { s_run_all_exports = true; });

  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) { s_infile = argument; });
  parser.Parse(argc, argv);

  if (s_spec && s_run_all_exports)
    WABT_FATAL("--spec and --run-all-exports are incompatible.\n");
}

enum class ModuleType {
  Text,
  Binary,
};

static string_view get_dirname(string_view path) {
  // Strip everything after and including the last slash (or backslash), e.g.:
  //
  // s = "foo/bar/baz", => "foo/bar"
  // s = "/usr/local/include/stdio.h", => "/usr/local/include"
  // s = "foo.bar", => ""
  // s = "some\windows\directory", => "some\windows"
  size_t last_slash = path.find_last_of('/');
  size_t last_backslash = path.find_last_of('\\');
  if (last_slash == string_view::npos)
    last_slash = 0;
  if (last_backslash == string_view::npos)
    last_backslash = 0;

  return path.substr(0, std::max(last_slash, last_backslash));
}

/* Not sure, but 100 chars is probably safe */
#define MAX_TYPED_VALUE_CHARS 100

static void sprint_typed_value(char* buffer,
                               size_t size,
                               const TypedValue* tv) {
  switch (tv->type) {
    case Type::I32:
      snprintf(buffer, size, "i32:%u", tv->value.i32);
      break;

    case Type::I64:
      snprintf(buffer, size, "i64:%" PRIu64, tv->value.i64);
      break;

    case Type::F32: {
      float value;
      memcpy(&value, &tv->value.f32_bits, sizeof(float));
      snprintf(buffer, size, "f32:%f", value);
      break;
    }

    case Type::F64: {
      double value;
      memcpy(&value, &tv->value.f64_bits, sizeof(double));
      snprintf(buffer, size, "f64:%f", value);
      break;
    }

    default:
      assert(0);
      break;
  }
}

static void print_typed_value(const TypedValue* tv) {
  char buffer[MAX_TYPED_VALUE_CHARS];
  sprint_typed_value(buffer, sizeof(buffer), tv);
  printf("%s", buffer);
}

static void print_typed_value_vector(const std::vector<TypedValue>& values) {
  for (size_t i = 0; i < values.size(); ++i) {
    print_typed_value(&values[i]);
    if (i != values.size() - 1)
      printf(", ");
  }
}

static void print_interpreter_result(const char* desc,
                                     interpreter::Result iresult) {
  printf("%s: %s\n", desc, s_trap_strings[static_cast<size_t>(iresult)]);
}

static void print_call(string_view module_name,
                       string_view func_name,
                       const std::vector<TypedValue>& args,
                       const std::vector<TypedValue>& results,
                       interpreter::Result iresult) {
  if (!module_name.empty())
    printf(PRIstringview ".", WABT_PRINTF_STRING_VIEW_ARG(module_name));
  printf(PRIstringview "(", WABT_PRINTF_STRING_VIEW_ARG(func_name));
  print_typed_value_vector(args);
  printf(") =>");
  if (iresult == interpreter::Result::Ok) {
    if (results.size() > 0) {
      printf(" ");
      print_typed_value_vector(results);
    }
    printf("\n");
  } else {
    print_interpreter_result(" error", iresult);
  }
}

static interpreter::Result run_function(Thread* thread,
                                        Index func_index,
                                        const std::vector<TypedValue>& args,
                                        std::vector<TypedValue>* out_results) {
  return s_trace ? thread->TraceFunction(func_index, s_stdout_stream.get(),
                                         args, out_results)
                 : thread->RunFunction(func_index, args, out_results);
}

static interpreter::Result run_start_function(Thread* thread,
                                              DefinedModule* module) {
  if (module->start_func_index == kInvalidIndex)
    return interpreter::Result::Ok;

  if (s_trace)
    printf(">>> running start function:\n");
  std::vector<TypedValue> args;
  std::vector<TypedValue> results;
  interpreter::Result iresult =
      run_function(thread, module->start_func_index, args, &results);
  assert(results.size() == 0);
  return iresult;
}

static interpreter::Result run_export(Thread* thread,
                                      const Export* export_,
                                      const std::vector<TypedValue>& args,
                                      std::vector<TypedValue>* out_results) {
  if (s_trace) {
    printf(">>> running export \"" PRIstringview "\":\n",
           WABT_PRINTF_STRING_VIEW_ARG(export_->name));
  }

  assert(export_->kind == ExternalKind::Func);
  return run_function(thread, export_->index, args, out_results);
}

static interpreter::Result run_export_by_name(
    Thread* thread,
    Module* module,
    string_view name,
    const std::vector<TypedValue>& args,
    std::vector<TypedValue>* out_results,
    RunVerbosity verbose) {
  Export* export_ = module->GetExport(name);
  if (!export_)
    return interpreter::Result::UnknownExport;
  if (export_->kind != ExternalKind::Func)
    return interpreter::Result::ExportKindMismatch;
  return run_export(thread, export_, args, out_results);
}

static interpreter::Result get_global_export_by_name(
    Thread* thread,
    Module* module,
    string_view name,
    std::vector<TypedValue>* out_results) {
  Export* export_ = module->GetExport(name);
  if (!export_)
    return interpreter::Result::UnknownExport;
  if (export_->kind != ExternalKind::Global)
    return interpreter::Result::ExportKindMismatch;

  Global* global = thread->env()->GetGlobal(export_->index);
  out_results->clear();
  out_results->push_back(global->typed_value);
  return interpreter::Result::Ok;
}

static void run_all_exports(Module* module,
                            Thread* thread,
                            RunVerbosity verbose) {
  std::vector<TypedValue> args;
  std::vector<TypedValue> results;
  for (const Export& export_ : module->exports) {
    interpreter::Result iresult = run_export(thread, &export_, args, &results);
    if (verbose == RunVerbosity::Verbose) {
      print_call(string_view(), export_.name, args, results, iresult);
    }
  }
}

static wabt::Result read_module(const char* module_filename,
                                Environment* env,
                                ErrorHandler* error_handler,
                                DefinedModule** out_module) {
  wabt::Result result;
  std::vector<uint8_t> file_data;

  *out_module = nullptr;

  result = ReadFile(module_filename, &file_data);
  if (Succeeded(result)) {
    result = read_binary_interpreter(env, DataOrNull(file_data),
                                     file_data.size(), &s_read_binary_options,
                                     error_handler, out_module);

    if (Succeeded(result)) {
      if (s_verbose)
        env->DisassembleModule(s_stdout_stream.get(), *out_module);
    }
  }
  return result;
}

static interpreter::Result default_host_callback(const HostFunc* func,
                                                 const FuncSignature* sig,
                                                 Index num_args,
                                                 TypedValue* args,
                                                 Index num_results,
                                                 TypedValue* out_results,
                                                 void* user_data) {
  memset(out_results, 0, sizeof(TypedValue) * num_results);
  for (Index i = 0; i < num_results; ++i)
    out_results[i].type = sig->result_types[i];

  std::vector<TypedValue> vec_args(args, args + num_args);
  std::vector<TypedValue> vec_results(out_results, out_results + num_results);

  printf("called host ");
  print_call(func->module_name, func->field_name, vec_args, vec_results,
             interpreter::Result::Ok);
  return interpreter::Result::Ok;
}

#define PRIimport "\"" PRIstringview "." PRIstringview "\""
#define PRINTF_IMPORT_ARG(x)                    \
  WABT_PRINTF_STRING_VIEW_ARG((x).module_name) \
  , WABT_PRINTF_STRING_VIEW_ARG((x).field_name)

class SpectestHostImportDelegate : public HostImportDelegate {
 public:
  wabt::Result ImportFunc(Import* import,
                          Func* func,
                          FuncSignature* func_sig,
                          const ErrorCallback& callback) override {
    if (import->field_name == "print") {
      func->as_host()->callback = default_host_callback;
      return wabt::Result::Ok;
    } else {
      PrintError(callback, "unknown host function import " PRIimport,
                 PRINTF_IMPORT_ARG(*import));
      return wabt::Result::Error;
    }
  }

  wabt::Result ImportTable(Import* import,
                           Table* table,
                           const ErrorCallback& callback) override {
    if (import->field_name == "table") {
      table->limits.has_max = true;
      table->limits.initial = 10;
      table->limits.max = 20;
      return wabt::Result::Ok;
    } else {
      PrintError(callback, "unknown host table import " PRIimport,
                 PRINTF_IMPORT_ARG(*import));
      return wabt::Result::Error;
    }
  }

  wabt::Result ImportMemory(Import* import,
                            Memory* memory,
                            const ErrorCallback& callback) override {
    if (import->field_name == "memory") {
      memory->page_limits.has_max = true;
      memory->page_limits.initial = 1;
      memory->page_limits.max = 2;
      memory->data.resize(memory->page_limits.initial * WABT_MAX_PAGES);
      return wabt::Result::Ok;
    } else {
      PrintError(callback, "unknown host memory import " PRIimport,
                 PRINTF_IMPORT_ARG(*import));
      return wabt::Result::Error;
    }
  }

  wabt::Result ImportGlobal(Import* import,
                            Global* global,
                            const ErrorCallback& callback) override {
    if (import->field_name == "global") {
      switch (global->typed_value.type) {
        case Type::I32:
          global->typed_value.value.i32 = 666;
          break;

        case Type::F32: {
          float value = 666.6f;
          memcpy(&global->typed_value.value.f32_bits, &value, sizeof(value));
          break;
        }

        case Type::I64:
          global->typed_value.value.i64 = 666;
          break;

        case Type::F64: {
          double value = 666.6;
          memcpy(&global->typed_value.value.f64_bits, &value, sizeof(value));
          break;
        }

        default:
          PrintError(callback, "bad type for host global import " PRIimport,
                     PRINTF_IMPORT_ARG(*import));
          return wabt::Result::Error;
      }

      return wabt::Result::Ok;
    } else {
      PrintError(callback, "unknown host global import " PRIimport,
                 PRINTF_IMPORT_ARG(*import));
      return wabt::Result::Error;
    }
  }

 private:
  void PrintError(const ErrorCallback& callback, const char* format, ...) {
    WABT_SNPRINTF_ALLOCA(buffer, length, format);
    callback(buffer);
  }
};

static void init_environment(Environment* env) {
  HostModule* host_module = env->AppendHostModule("spectest");
  host_module->import_delegate.reset(new SpectestHostImportDelegate());
}

static wabt::Result read_and_run_module(const char* module_filename) {
  wabt::Result result;
  Environment env;
  init_environment(&env);

  ErrorHandlerFile error_handler(Location::Type::Binary);
  DefinedModule* module = nullptr;
  result = read_module(module_filename, &env, &error_handler, &module);
  if (Succeeded(result)) {
    Thread thread(&env, s_thread_options);
    interpreter::Result iresult = run_start_function(&thread, module);
    if (iresult == interpreter::Result::Ok) {
      if (s_run_all_exports)
        run_all_exports(module, &thread, RunVerbosity::Verbose);
    } else {
      print_interpreter_result("error running start function", iresult);
    }
  }
  return result;
}

/* An extremely simple JSON parser that only knows how to parse the expected
 * format from wast2wasm. */
struct Context {
  Context()
      : thread(&env, s_thread_options),
        last_module(nullptr),
        json_offset(0),
        has_prev_loc(0),
        command_line_number(0),
        passed(0),
        total(0) {}

  Environment env;
  Thread thread;
  DefinedModule* last_module;

  /* Parsing info */
  std::vector<uint8_t> json_data;
  std::string source_filename;
  size_t json_offset;
  Location loc;
  Location prev_loc;
  bool has_prev_loc;
  uint32_t command_line_number;

  /* Test info */
  int passed;
  int total;
};

enum class ActionType {
  Invoke,
  Get,
};

struct Action {
  ActionType type = ActionType::Invoke;
  std::string module_name;
  std::string field_name;
  std::vector<TypedValue> args;
};

#define CHECK_RESULT(x)           \
  do {                            \
    if (Failed(x))                \
      return wabt::Result::Error; \
  } while (0)

#define EXPECT(x) CHECK_RESULT(expect(ctx, x))
#define EXPECT_KEY(x) CHECK_RESULT(expect_key(ctx, x))
#define PARSE_KEY_STRING_VALUE(key, value) \
  CHECK_RESULT(parse_key_string_value(ctx, key, value))

static void WABT_PRINTF_FORMAT(2, 3)
    print_parse_error(Context* ctx, const char* format, ...) {
  WABT_SNPRINTF_ALLOCA(buffer, length, format);
  fprintf(stderr, "%s:%d:%d: %s\n", ctx->loc.filename, ctx->loc.line,
          ctx->loc.first_column, buffer);
}

static void WABT_PRINTF_FORMAT(2, 3)
    print_command_error(Context* ctx, const char* format, ...) {
  WABT_SNPRINTF_ALLOCA(buffer, length, format);
  printf(PRIstringview ":%u: %s\n",
         WABT_PRINTF_STRING_VIEW_ARG(ctx->source_filename),
         ctx->command_line_number, buffer);
}

static void putback_char(Context* ctx) {
  assert(ctx->has_prev_loc);
  ctx->json_offset--;
  ctx->loc = ctx->prev_loc;
  ctx->has_prev_loc = false;
}

static int read_char(Context* ctx) {
  if (ctx->json_offset >= ctx->json_data.size())
    return -1;
  ctx->prev_loc = ctx->loc;
  char c = ctx->json_data[ctx->json_offset++];
  if (c == '\n') {
    ctx->loc.line++;
    ctx->loc.first_column = 1;
  } else {
    ctx->loc.first_column++;
  }
  ctx->has_prev_loc = true;
  return c;
}

static void skip_whitespace(Context* ctx) {
  while (1) {
    switch (read_char(ctx)) {
      case -1:
        return;

      case ' ':
      case '\t':
      case '\n':
      case '\r':
        break;

      default:
        putback_char(ctx);
        return;
    }
  }
}

static bool match(Context* ctx, const char* s) {
  skip_whitespace(ctx);
  Location start_loc = ctx->loc;
  size_t start_offset = ctx->json_offset;
  while (*s && *s == read_char(ctx))
    s++;

  if (*s == 0) {
    return true;
  } else {
    ctx->json_offset = start_offset;
    ctx->loc = start_loc;
    return false;
  }
}

static wabt::Result expect(Context* ctx, const char* s) {
  if (match(ctx, s)) {
    return wabt::Result::Ok;
  } else {
    print_parse_error(ctx, "expected %s", s);
    return wabt::Result::Error;
  }
}

static wabt::Result expect_key(Context* ctx, const char* key) {
  size_t keylen = strlen(key);
  size_t quoted_len = keylen + 2 + 1;
  char* quoted = static_cast<char*>(alloca(quoted_len));
  snprintf(quoted, quoted_len, "\"%s\"", key);
  EXPECT(quoted);
  EXPECT(":");
  return wabt::Result::Ok;
}

static wabt::Result parse_uint32(Context* ctx, uint32_t* out_int) {
  uint32_t result = 0;
  skip_whitespace(ctx);
  while (1) {
    int c = read_char(ctx);
    if (c >= '0' && c <= '9') {
      uint32_t last_result = result;
      result = result * 10 + static_cast<uint32_t>(c - '0');
      if (result < last_result) {
        print_parse_error(ctx, "uint32 overflow");
        return wabt::Result::Error;
      }
    } else {
      putback_char(ctx);
      break;
    }
  }
  *out_int = result;
  return wabt::Result::Ok;
}

static wabt::Result parse_string(Context* ctx, std::string* out_string) {
  out_string->clear();

  skip_whitespace(ctx);
  if (read_char(ctx) != '"') {
    print_parse_error(ctx, "expected string");
    return wabt::Result::Error;
  }

  while (1) {
    int c = read_char(ctx);
    if (c == '"') {
      break;
    } else if (c == '\\') {
      /* The only escape supported is \uxxxx. */
      c = read_char(ctx);
      if (c != 'u') {
        print_parse_error(ctx, "expected escape: \\uxxxx");
        return wabt::Result::Error;
      }
      uint16_t code = 0;
      for (int i = 0; i < 4; ++i) {
        c = read_char(ctx);
        int cval;
        if (c >= '0' && c <= '9') {
          cval = c - '0';
        } else if (c >= 'a' && c <= 'f') {
          cval = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
          cval = c - 'A' + 10;
        } else {
          print_parse_error(ctx, "expected hex char");
          return wabt::Result::Error;
        }
        code = (code << 4) + cval;
      }

      if (code < 256) {
        *out_string += code;
      } else {
        print_parse_error(ctx, "only escape codes < 256 allowed, got %u\n",
                          code);
      }
    } else {
      *out_string += c;
    }
  }
  return wabt::Result::Ok;
}

static wabt::Result parse_key_string_value(Context* ctx,
                                           const char* key,
                                           std::string* out_string) {
  out_string->clear();
  EXPECT_KEY(key);
  return parse_string(ctx, out_string);
}

static wabt::Result parse_opt_name_string_value(Context* ctx,
                                                std::string* out_string) {
  out_string->clear();
  if (match(ctx, "\"name\"")) {
    EXPECT(":");
    CHECK_RESULT(parse_string(ctx, out_string));
    EXPECT(",");
  }
  return wabt::Result::Ok;
}

static wabt::Result parse_line(Context* ctx) {
  EXPECT_KEY("line");
  CHECK_RESULT(parse_uint32(ctx, &ctx->command_line_number));
  return wabt::Result::Ok;
}

static wabt::Result parse_type_object(Context* ctx, Type* out_type) {
  std::string type_str;
  EXPECT("{");
  PARSE_KEY_STRING_VALUE("type", &type_str);
  EXPECT("}");

  if (type_str == "i32") {
    *out_type = Type::I32;
    return wabt::Result::Ok;
  } else if (type_str == "f32") {
    *out_type = Type::F32;
    return wabt::Result::Ok;
  } else if (type_str == "i64") {
    *out_type = Type::I64;
    return wabt::Result::Ok;
  } else if (type_str == "f64") {
    *out_type = Type::F64;
    return wabt::Result::Ok;
  } else {
    print_parse_error(ctx, "unknown type: \"" PRIstringview "\"",
                      WABT_PRINTF_STRING_VIEW_ARG(type_str));
    return wabt::Result::Error;
  }
}

static wabt::Result parse_type_vector(Context* ctx, TypeVector* out_types) {
  out_types->clear();
  EXPECT("[");
  bool first = true;
  while (!match(ctx, "]")) {
    if (!first)
      EXPECT(",");
    Type type;
    CHECK_RESULT(parse_type_object(ctx, &type));
    first = false;
    out_types->push_back(type);
  }
  return wabt::Result::Ok;
}

static wabt::Result parse_const(Context* ctx, TypedValue* out_value) {
  std::string type_str;
  std::string value_str;
  EXPECT("{");
  PARSE_KEY_STRING_VALUE("type", &type_str);
  EXPECT(",");
  PARSE_KEY_STRING_VALUE("value", &value_str);
  EXPECT("}");

  const char* value_start = value_str.data();
  const char* value_end = value_str.data() + value_str.size();

  if (type_str == "i32") {
    uint32_t value;
    CHECK_RESULT(parse_int32(value_start, value_end, &value,
                             ParseIntType::UnsignedOnly));
    out_value->type = Type::I32;
    out_value->value.i32 = value;
    return wabt::Result::Ok;
  } else if (type_str == "f32") {
    uint32_t value_bits;
    CHECK_RESULT(parse_int32(value_start, value_end, &value_bits,
                             ParseIntType::UnsignedOnly));
    out_value->type = Type::F32;
    out_value->value.f32_bits = value_bits;
    return wabt::Result::Ok;
  } else if (type_str == "i64") {
    uint64_t value;
    CHECK_RESULT(parse_int64(value_start, value_end, &value,
                             ParseIntType::UnsignedOnly));
    out_value->type = Type::I64;
    out_value->value.i64 = value;
    return wabt::Result::Ok;
  } else if (type_str == "f64") {
    uint64_t value_bits;
    CHECK_RESULT(parse_int64(value_start, value_end, &value_bits,
                             ParseIntType::UnsignedOnly));
    out_value->type = Type::F64;
    out_value->value.f64_bits = value_bits;
    return wabt::Result::Ok;
  } else {
    print_parse_error(ctx, "unknown type: \"" PRIstringview "\"",
                      WABT_PRINTF_STRING_VIEW_ARG(type_str));
    return wabt::Result::Error;
  }
}

static wabt::Result parse_const_vector(Context* ctx,
                                       std::vector<TypedValue>* out_values) {
  out_values->clear();
  EXPECT("[");
  bool first = true;
  while (!match(ctx, "]")) {
    if (!first)
      EXPECT(",");
    TypedValue value;
    CHECK_RESULT(parse_const(ctx, &value));
    out_values->push_back(value);
    first = false;
  }
  return wabt::Result::Ok;
}

static wabt::Result parse_action(Context* ctx, Action* out_action) {
  EXPECT_KEY("action");
  EXPECT("{");
  EXPECT_KEY("type");
  if (match(ctx, "\"invoke\"")) {
    out_action->type = ActionType::Invoke;
  } else {
    EXPECT("\"get\"");
    out_action->type = ActionType::Get;
  }
  EXPECT(",");
  if (match(ctx, "\"module\"")) {
    EXPECT(":");
    CHECK_RESULT(parse_string(ctx, &out_action->module_name));
    EXPECT(",");
  }
  PARSE_KEY_STRING_VALUE("field", &out_action->field_name);
  if (out_action->type == ActionType::Invoke) {
    EXPECT(",");
    EXPECT_KEY("args");
    CHECK_RESULT(parse_const_vector(ctx, &out_action->args));
  }
  EXPECT("}");
  return wabt::Result::Ok;
}

static wabt::Result parse_module_type(Context* ctx, ModuleType* out_type) {
  std::string module_type_str;

  PARSE_KEY_STRING_VALUE("module_type", &module_type_str);
  if (module_type_str == "text") {
    *out_type = ModuleType::Text;
    return wabt::Result::Ok;
  } else if (module_type_str == "binary") {
    *out_type = ModuleType::Binary;
    return wabt::Result::Ok;
  } else {
    print_parse_error(ctx, "unknown module type: \"" PRIstringview "\"",
                      WABT_PRINTF_STRING_VIEW_ARG(module_type_str));
    return wabt::Result::Error;
  }
}

static std::string CreateModulePath(Context* ctx, string_view filename) {
  const char* spec_json_filename = ctx->loc.filename;
  string_view dirname = get_dirname(spec_json_filename);
  std::string path;

  if (dirname.size() == 0) {
    path = filename.to_string();
  } else {
    path = dirname.to_string();
    path += '/';
    path += filename.to_string();
  }

  ConvertBackslashToSlash(&path);
  return path;
}

static wabt::Result on_module_command(Context* ctx,
                                      string_view filename,
                                      string_view name) {
  std::string path = CreateModulePath(ctx, filename);
  Environment::MarkPoint mark = ctx->env.Mark();
  ErrorHandlerFile error_handler(Location::Type::Binary);
  wabt::Result result =
      read_module(path.c_str(), &ctx->env, &error_handler, &ctx->last_module);

  if (Failed(result)) {
    ctx->env.ResetToMarkPoint(mark);
    print_command_error(ctx, "error reading module: \"%s\"", path.c_str());
    return wabt::Result::Error;
  }

  interpreter::Result iresult =
      run_start_function(&ctx->thread, ctx->last_module);
  if (iresult != interpreter::Result::Ok) {
    ctx->env.ResetToMarkPoint(mark);
    print_interpreter_result("error running start function", iresult);
    return wabt::Result::Error;
  }

  if (!name.empty()) {
    ctx->last_module->name = name.to_string();
    ctx->env.EmplaceModuleBinding(name.to_string(),
                                  Binding(ctx->env.GetModuleCount() - 1));
  }
  return wabt::Result::Ok;
}

static wabt::Result run_action(Context* ctx,
                               Action* action,
                               interpreter::Result* out_iresult,
                               std::vector<TypedValue>* out_results,
                               RunVerbosity verbose) {
  out_results->clear();

  Module* module;
  if (!action->module_name.empty()) {
    module = ctx->env.FindModule(action->module_name);
  } else {
    module = ctx->env.GetLastModule();
  }
  assert(module);

  switch (action->type) {
    case ActionType::Invoke:
      *out_iresult =
          run_export_by_name(&ctx->thread, module, action->field_name,
                             action->args, out_results, verbose);
      if (verbose == RunVerbosity::Verbose) {
        print_call(string_view(), action->field_name, action->args,
                   *out_results, *out_iresult);
      }
      return wabt::Result::Ok;

    case ActionType::Get: {
      *out_iresult = get_global_export_by_name(&ctx->thread, module,
                                               action->field_name, out_results);
      return wabt::Result::Ok;
    }

    default:
      print_command_error(ctx, "invalid action type %d",
                          static_cast<int>(action->type));
      return wabt::Result::Error;
  }
}

static wabt::Result on_action_command(Context* ctx, Action* action) {
  std::vector<TypedValue> results;
  interpreter::Result iresult;

  ctx->total++;
  wabt::Result result =
      run_action(ctx, action, &iresult, &results, RunVerbosity::Verbose);
  if (Succeeded(result)) {
    if (iresult == interpreter::Result::Ok) {
      ctx->passed++;
    } else {
      print_command_error(ctx, "unexpected trap: %s",
                          s_trap_strings[static_cast<size_t>(iresult)]);
      result = wabt::Result::Error;
    }
  }

  return result;
}

static wabt::Result read_invalid_text_module(const char* module_filename,
                                             Environment* env,
                                             ErrorHandler* error_handler) {
  std::unique_ptr<WastLexer> lexer =
      WastLexer::CreateFileLexer(module_filename);
  wabt::Result result = parse_wast(lexer.get(), nullptr, error_handler);
  return result;
}

static wabt::Result read_invalid_module(Context* ctx,
                                        const char* module_filename,
                                        Environment* env,
                                        ModuleType module_type,
                                        const char* desc) {
  std::string header =
      string_printf(PRIstringview ":%d: %s passed",
                    WABT_PRINTF_STRING_VIEW_ARG(ctx->source_filename),
                    ctx->command_line_number, desc);

  switch (module_type) {
    case ModuleType::Text: {
      ErrorHandlerFile error_handler(Location::Type::Text, stdout, header,
                                     ErrorHandlerFile::PrintHeader::Once);
      return read_invalid_text_module(module_filename, env, &error_handler);
    }

    case ModuleType::Binary: {
      DefinedModule* module;
      ErrorHandlerFile error_handler(Location::Type::Binary, stdout, header,
                                     ErrorHandlerFile::PrintHeader::Once);
      return read_module(module_filename, env, &error_handler, &module);
    }

    default:
      return wabt::Result::Error;
  }
}

static wabt::Result on_assert_malformed_command(Context* ctx,
                                                string_view filename,
                                                string_view text,
                                                ModuleType module_type) {
  Environment env;
  init_environment(&env);

  ctx->total++;
  std::string path = CreateModulePath(ctx, filename);
  wabt::Result result = read_invalid_module(ctx, path.c_str(), &env,
                                            module_type, "assert_malformed");
  if (Failed(result)) {
    ctx->passed++;
    result = wabt::Result::Ok;
  } else {
    print_command_error(ctx, "expected module to be malformed: \"%s\"",
                        path.c_str());
    result = wabt::Result::Error;
  }

  return result;
}

static wabt::Result on_register_command(Context* ctx,
                                        string_view name,
                                        string_view as) {
  Index module_index;
  if (!name.empty()) {
    module_index = ctx->env.FindModuleIndex(name);
  } else {
    module_index = ctx->env.GetLastModuleIndex();
  }

  if (module_index == kInvalidIndex) {
    print_command_error(ctx, "unknown module in register");
    return wabt::Result::Error;
  }

  ctx->env.EmplaceRegisteredModuleBinding(as.to_string(),
                                          Binding(module_index));
  return wabt::Result::Ok;
}

static wabt::Result on_assert_unlinkable_command(Context* ctx,
                                                 string_view filename,
                                                 string_view text,
                                                 ModuleType module_type) {
  ctx->total++;
  std::string path = CreateModulePath(ctx, filename);
  Environment::MarkPoint mark = ctx->env.Mark();
  wabt::Result result = read_invalid_module(ctx, path.c_str(), &ctx->env,
                                            module_type, "assert_unlinkable");
  ctx->env.ResetToMarkPoint(mark);

  if (Failed(result)) {
    ctx->passed++;
    result = wabt::Result::Ok;
  } else {
    print_command_error(ctx, "expected module to be unlinkable: \"%s\"",
                        path.c_str());
    result = wabt::Result::Error;
  }

  return result;
}

static wabt::Result on_assert_invalid_command(Context* ctx,
                                              string_view filename,
                                              string_view text,
                                              ModuleType module_type) {
  Environment env;
  init_environment(&env);

  ctx->total++;
  std::string path = CreateModulePath(ctx, filename);
  wabt::Result result = read_invalid_module(ctx, path.c_str(), &env,
                                            module_type, "assert_invalid");
  if (Failed(result)) {
    ctx->passed++;
    result = wabt::Result::Ok;
  } else {
    print_command_error(ctx, "expected module to be invalid: \"%s\"",
                        path.c_str());
    result = wabt::Result::Error;
  }

  return result;
}

static wabt::Result on_assert_uninstantiable_command(Context* ctx,
                                                     string_view filename,
                                                     string_view text,
                                                     ModuleType module_type) {
  ErrorHandlerFile error_handler(Location::Type::Binary);
  ctx->total++;
  std::string path = CreateModulePath(ctx, filename);
  DefinedModule* module;
  Environment::MarkPoint mark = ctx->env.Mark();
  wabt::Result result =
      read_module(path.c_str(), &ctx->env, &error_handler, &module);

  if (Succeeded(result)) {
    interpreter::Result iresult = run_start_function(&ctx->thread, module);
    if (iresult == interpreter::Result::Ok) {
      print_command_error(ctx, "expected error running start function: \"%s\"",
                          path.c_str());
      result = wabt::Result::Error;
    } else {
      ctx->passed++;
      result = wabt::Result::Ok;
    }
  } else {
    print_command_error(ctx, "error reading module: \"%s\"", path.c_str());
    result = wabt::Result::Error;
  }

  ctx->env.ResetToMarkPoint(mark);
  return result;
}

static bool typed_values_are_equal(const TypedValue* tv1,
                                   const TypedValue* tv2) {
  if (tv1->type != tv2->type)
    return false;

  switch (tv1->type) {
    case Type::I32:
      return tv1->value.i32 == tv2->value.i32;
    case Type::F32:
      return tv1->value.f32_bits == tv2->value.f32_bits;
    case Type::I64:
      return tv1->value.i64 == tv2->value.i64;
    case Type::F64:
      return tv1->value.f64_bits == tv2->value.f64_bits;
    default:
      assert(0);
      return false;
  }
}

static wabt::Result on_assert_return_command(
    Context* ctx,
    Action* action,
    const std::vector<TypedValue>& expected) {
  std::vector<TypedValue> results;
  interpreter::Result iresult;

  ctx->total++;
  wabt::Result result =
      run_action(ctx, action, &iresult, &results, RunVerbosity::Quiet);

  if (Succeeded(result)) {
    if (iresult == interpreter::Result::Ok) {
      if (results.size() == expected.size()) {
        for (size_t i = 0; i < results.size(); ++i) {
          const TypedValue* expected_tv = &expected[i];
          const TypedValue* actual_tv = &results[i];
          if (!typed_values_are_equal(expected_tv, actual_tv)) {
            char expected_str[MAX_TYPED_VALUE_CHARS];
            char actual_str[MAX_TYPED_VALUE_CHARS];
            sprint_typed_value(expected_str, sizeof(expected_str), expected_tv);
            sprint_typed_value(actual_str, sizeof(actual_str), actual_tv);
            print_command_error(ctx,
                                "mismatch in result %" PRIzd
                                " of assert_return: expected %s, got %s",
                                i, expected_str, actual_str);
            result = wabt::Result::Error;
          }
        }
      } else {
        print_command_error(
            ctx,
            "result length mismatch in assert_return: expected %" PRIzd
            ", got %" PRIzd,
            expected.size(), results.size());
        result = wabt::Result::Error;
      }
    } else {
      print_command_error(ctx, "unexpected trap: %s",
                          s_trap_strings[static_cast<size_t>(iresult)]);
      result = wabt::Result::Error;
    }
  }

  if (Succeeded(result))
    ctx->passed++;

  return result;
}

static wabt::Result on_assert_return_nan_command(Context* ctx,
                                                 Action* action,
                                                 bool canonical) {
  std::vector<TypedValue> results;
  interpreter::Result iresult;

  ctx->total++;
  wabt::Result result =
      run_action(ctx, action, &iresult, &results, RunVerbosity::Quiet);
  if (Succeeded(result)) {
    if (iresult == interpreter::Result::Ok) {
      if (results.size() != 1) {
        print_command_error(ctx, "expected one result, got %" PRIzd,
                            results.size());
        result = wabt::Result::Error;
      }

      const TypedValue& actual = results[0];
      switch (actual.type) {
        case Type::F32: {
          bool is_nan = canonical ? IsCanonicalNan(actual.value.f32_bits)
                                  : IsArithmeticNan(actual.value.f32_bits);
          if (!is_nan) {
            char actual_str[MAX_TYPED_VALUE_CHARS];
            sprint_typed_value(actual_str, sizeof(actual_str), &actual);
            print_command_error(ctx, "expected result to be nan, got %s",
                                actual_str);
            result = wabt::Result::Error;
          }
          break;
        }

        case Type::F64: {
          bool is_nan = canonical ? IsCanonicalNan(actual.value.f64_bits)
                                  : IsArithmeticNan(actual.value.f64_bits);
          if (!is_nan) {
            char actual_str[MAX_TYPED_VALUE_CHARS];
            sprint_typed_value(actual_str, sizeof(actual_str), &actual);
            print_command_error(ctx, "expected result to be nan, got %s",
                                actual_str);
            result = wabt::Result::Error;
          }
          break;
        }

        default:
          print_command_error(ctx,
                              "expected result type to be f32 or f64, got %s",
                              get_type_name(actual.type));
          result = wabt::Result::Error;
          break;
      }
    } else {
      print_command_error(ctx, "unexpected trap: %s",
                          s_trap_strings[static_cast<int>(iresult)]);
      result = wabt::Result::Error;
    }
  }

  if (Succeeded(result))
    ctx->passed++;

  return wabt::Result::Ok;
}

static wabt::Result on_assert_trap_command(Context* ctx,
                                           Action* action,
                                           string_view text) {
  std::vector<TypedValue> results;
  interpreter::Result iresult;

  ctx->total++;
  wabt::Result result =
      run_action(ctx, action, &iresult, &results, RunVerbosity::Quiet);
  if (Succeeded(result)) {
    if (iresult != interpreter::Result::Ok) {
      ctx->passed++;
    } else {
      print_command_error(ctx, "expected trap: \"" PRIstringview "\"",
                          WABT_PRINTF_STRING_VIEW_ARG(text));
      result = wabt::Result::Error;
    }
  }

  return result;
}

static wabt::Result on_assert_exhaustion_command(Context* ctx, Action* action) {
  std::vector<TypedValue> results;
  interpreter::Result iresult;

  ctx->total++;
  wabt::Result result =
      run_action(ctx, action, &iresult, &results, RunVerbosity::Quiet);
  if (Succeeded(result)) {
    if (iresult == interpreter::Result::TrapCallStackExhausted ||
        iresult == interpreter::Result::TrapValueStackExhausted) {
      ctx->passed++;
    } else {
      print_command_error(ctx, "expected call stack exhaustion");
      result = wabt::Result::Error;
    }
  }

  return result;
}

static wabt::Result parse_command(Context* ctx) {
  EXPECT("{");
  EXPECT_KEY("type");
  if (match(ctx, "\"module\"")) {
    std::string name;
    std::string filename;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_opt_name_string_value(ctx, &name));
    PARSE_KEY_STRING_VALUE("filename", &filename);
    on_module_command(ctx, filename, name);
  } else if (match(ctx, "\"action\"")) {
    Action action;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_action(ctx, &action));
    on_action_command(ctx, &action);
  } else if (match(ctx, "\"register\"")) {
    std::string as;
    std::string name;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_opt_name_string_value(ctx, &name));
    PARSE_KEY_STRING_VALUE("as", &as);
    on_register_command(ctx, name, as);
  } else if (match(ctx, "\"assert_malformed\"")) {
    std::string filename;
    std::string text;
    ModuleType module_type;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("filename", &filename);
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("text", &text);
    EXPECT(",");
    CHECK_RESULT(parse_module_type(ctx, &module_type));
    on_assert_malformed_command(ctx, filename, text, module_type);
  } else if (match(ctx, "\"assert_invalid\"")) {
    std::string filename;
    std::string text;
    ModuleType module_type;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("filename", &filename);
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("text", &text);
    EXPECT(",");
    CHECK_RESULT(parse_module_type(ctx, &module_type));
    on_assert_invalid_command(ctx, filename, text, module_type);
  } else if (match(ctx, "\"assert_unlinkable\"")) {
    std::string filename;
    std::string text;
    ModuleType module_type;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("filename", &filename);
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("text", &text);
    EXPECT(",");
    CHECK_RESULT(parse_module_type(ctx, &module_type));
    on_assert_unlinkable_command(ctx, filename, text, module_type);
  } else if (match(ctx, "\"assert_uninstantiable\"")) {
    std::string filename;
    std::string text;
    ModuleType module_type;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("filename", &filename);
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("text", &text);
    EXPECT(",");
    CHECK_RESULT(parse_module_type(ctx, &module_type));
    on_assert_uninstantiable_command(ctx, filename, text, module_type);
  } else if (match(ctx, "\"assert_return\"")) {
    Action action;
    std::vector<TypedValue> expected;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_action(ctx, &action));
    EXPECT(",");
    EXPECT_KEY("expected");
    CHECK_RESULT(parse_const_vector(ctx, &expected));
    on_assert_return_command(ctx, &action, expected);
  } else if (match(ctx, "\"assert_return_canonical_nan\"")) {
    Action action;
    TypeVector expected;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_action(ctx, &action));
    EXPECT(",");
    /* Not needed for wabt-interp, but useful for other parsers. */
    EXPECT_KEY("expected");
    CHECK_RESULT(parse_type_vector(ctx, &expected));
    on_assert_return_nan_command(ctx, &action, true);
  } else if (match(ctx, "\"assert_return_arithmetic_nan\"")) {
    Action action;
    TypeVector expected;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_action(ctx, &action));
    EXPECT(",");
    /* Not needed for wabt-interp, but useful for other parsers. */
    EXPECT_KEY("expected");
    CHECK_RESULT(parse_type_vector(ctx, &expected));
    on_assert_return_nan_command(ctx, &action, false);
  } else if (match(ctx, "\"assert_trap\"")) {
    Action action;
    std::string text;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_action(ctx, &action));
    EXPECT(",");
    PARSE_KEY_STRING_VALUE("text", &text);
    on_assert_trap_command(ctx, &action, text);
  } else if (match(ctx, "\"assert_exhaustion\"")) {
    Action action;
    std::string text;

    EXPECT(",");
    CHECK_RESULT(parse_line(ctx));
    EXPECT(",");
    CHECK_RESULT(parse_action(ctx, &action));
    on_assert_exhaustion_command(ctx, &action);
  } else {
    print_command_error(ctx, "unknown command type");
    return wabt::Result::Error;
  }
  EXPECT("}");
  return wabt::Result::Ok;
}

static wabt::Result parse_commands(Context* ctx) {
  EXPECT("{");
  PARSE_KEY_STRING_VALUE("source_filename", &ctx->source_filename);
  EXPECT(",");
  EXPECT_KEY("commands");
  EXPECT("[");
  bool first = true;
  while (!match(ctx, "]")) {
    if (!first)
      EXPECT(",");
    CHECK_RESULT(parse_command(ctx));
    first = false;
  }
  EXPECT("}");
  return wabt::Result::Ok;
}

static wabt::Result read_and_run_spec_json(const char* spec_json_filename) {
  Context ctx;
  ctx.loc.filename = spec_json_filename;
  ctx.loc.line = 1;
  ctx.loc.first_column = 1;
  init_environment(&ctx.env);

  wabt::Result result = ReadFile(spec_json_filename, &ctx.json_data);
  if (Failed(result))
    return wabt::Result::Error;

  result = parse_commands(&ctx);
  printf("%d/%d tests passed.\n", ctx.passed, ctx.total);
  return result;
}

int ProgramMain(int argc, char** argv) {
  init_stdio();
  parse_options(argc, argv);

  s_stdout_stream = FileStream::CreateStdout();

  wabt::Result result;
  if (s_spec) {
    result = read_and_run_spec_json(s_infile);
  } else {
    result = read_and_run_module(s_infile);
  }
  return result != wabt::Result::Ok;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
