#pragma once

#include "value.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace csy {

enum class Op : uint8_t {
    PUSH_NONE,
    PUSH_BOOL,
    PUSH_INT,
    PUSH_FLOAT,
    PUSH_STR,
    PUSH_FUNC,
    PUSH_BUILTIN,
    PUSH_SINK,
    LOAD_LOCAL,
    STORE_LOCAL,
    LOAD_GLOBAL,
    STORE_GLOBAL,
    LOAD_UPVALUE,
    STORE_UPVALUE,
    LOAD_UPVALUE_BOX,
    LOAD_LOCAL_BOXED,
    STORE_LOCAL_BOXED,
    MAKE_CLOSURE,
    PUSH_SELF,
    TRY_ENTER,
    TRY_EXIT,
    ADD, SUB, MUL, DIV, MOD,
    EQ, NEQ, LT, GT, LE, GE,
    AND, OR,
    NEG, NOT,
    POP, DUP,
    MAKE_VEC,
    MAKE_DICT,
    GET_INDEX,
    SET_INDEX,
    OUTPUT,
    JMP,
    JMPF,
    JMPR,
    CALL,
    RET,
    CALL_NATIVE,
    HALT,
    LEN,
    VEC_PUSH,
    Q_BIN,
    Q_BIN_STORE,
    Q_BIN_JMPF,
    Q_CMP_JMPF,
    Q_GETIDX,
};

struct Instr {
    Op op;
    int arg = 0;
    int line = 0;
    int col = 0;
};

struct NativeArg {
    double number;
    long long ival;
    double* data;
    long long length;
    int is_array;
};
union NativeRet { long long i; double f; };
typedef void (*NativeEntry)(const NativeArg* args, int* err, int* rtag, NativeRet* out);
struct NativeVariant {
    NativeEntry entry = nullptr;
    uint32_t sig = 0;
    uint8_t nparams = 0;
    std::vector<int> global_indices;
    bool ret_float = false;
};

struct QInstr {
    Op op = Op::HALT;
    uint8_t sub = 0;
    uint8_t ak = 0;
    uint8_t bk = 0;
    int arg = 0;
    int b = 0;
    int t = 0;
};

struct IRFunction {
    std::string name;
    std::vector<std::string> param_names;
    std::vector<Instr> code;
    std::vector<QInstr> qcode;
    int num_locals = 0;
    std::unordered_map<std::string, int> local_slots;
    std::vector<std::string> upvalue_names;
    std::unordered_map<std::string, int> upvalue_slots;
    std::unordered_set<std::string> boxed_locals;
    std::vector<NativeVariant> native_variants;
};

struct IRModule {
    std::string file;
    std::vector<IRFunction> functions;
    int main_func_index = -1;
    std::vector<long long> int_pool;
    std::vector<double> float_pool;
    std::vector<std::string> str_pool;
    std::unordered_map<long long, int> int_index;
    std::unordered_map<double, int> float_index;
    std::unordered_map<std::string, int> str_index;
    std::vector<std::string> global_names;
    std::unordered_map<std::string, int> global_index;
    std::vector<std::string> builtin_names;
    std::unordered_map<std::string, int> builtin_index;
    std::string native_c_source;
    std::string native_library;
    std::string native_target;
    std::string native_x64;
    int add_int(long long v);
    int add_float(double v);
    int add_str(const std::string& s);
    int add_global(const std::string& name);
    int add_builtin(const std::string& name);
};

std::string disassemble(const IRModule& mod);

}

