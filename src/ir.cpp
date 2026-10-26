#include "ir.hpp"
#include <sstream>

namespace csy {

int IRModule::add_int(long long v) {
    auto it = int_index.find(v);
    if (it != int_index.end()) return it->second;
    int idx = (int)int_pool.size();
    int_pool.push_back(v);
    int_index[v] = idx;
    return idx;
}

int IRModule::add_float(double v) {
    auto it = float_index.find(v);
    if (it != float_index.end()) return it->second;
    int idx = (int)float_pool.size();
    float_pool.push_back(v);
    float_index[v] = idx;
    return idx;
}

int IRModule::add_str(const std::string& s) {
    auto it = str_index.find(s);
    if (it != str_index.end()) return it->second;
    int idx = (int)str_pool.size();
    str_pool.push_back(s);
    str_index[s] = idx;
    return idx;
}

int IRModule::add_global(const std::string& name) {
    auto it = global_index.find(name);
    if (it != global_index.end()) return it->second;
    int idx = (int)global_names.size();
    global_names.push_back(name);
    global_index[name] = idx;
    return idx;
}

int IRModule::add_builtin(const std::string& name) {
    auto it = builtin_index.find(name);
    if (it != builtin_index.end()) return it->second;
    int idx = (int)builtin_names.size();
    builtin_names.push_back(name);
    builtin_index[name] = idx;
    return idx;
}

static const char* op_name(Op op) {
    switch (op) {
        case Op::PUSH_NONE: return "PUSH_NONE";
        case Op::PUSH_BOOL: return "PUSH_BOOL";
        case Op::PUSH_INT: return "PUSH_INT";
        case Op::PUSH_FLOAT: return "PUSH_FLOAT";
        case Op::PUSH_STR: return "PUSH_STR";
        case Op::PUSH_FUNC: return "PUSH_FUNC";
        case Op::PUSH_BUILTIN: return "PUSH_BUILTIN";
        case Op::PUSH_SINK: return "PUSH_SINK";
        case Op::LOAD_LOCAL: return "LOAD_LOCAL";
        case Op::STORE_LOCAL: return "STORE_LOCAL";
        case Op::LOAD_GLOBAL: return "LOAD_GLOBAL";
        case Op::STORE_GLOBAL: return "STORE_GLOBAL";
        case Op::LOAD_UPVALUE: return "LOAD_UPVALUE";
        case Op::STORE_UPVALUE: return "STORE_UPVALUE";
        case Op::LOAD_UPVALUE_BOX: return "LOAD_UPVALUE_BOX";
        case Op::LOAD_LOCAL_BOXED: return "LOAD_LOCAL_BOXED";
        case Op::STORE_LOCAL_BOXED: return "STORE_LOCAL_BOXED";
        case Op::MAKE_CLOSURE: return "MAKE_CLOSURE";
        case Op::PUSH_SELF: return "PUSH_SELF";
        case Op::TRY_ENTER: return "TRY_ENTER";
        case Op::TRY_EXIT: return "TRY_EXIT";
        case Op::ADD: return "ADD";
        case Op::SUB: return "SUB";
        case Op::MUL: return "MUL";
        case Op::DIV: return "DIV";
        case Op::MOD: return "MOD";
        case Op::EQ: return "EQ";
        case Op::NEQ: return "NEQ";
        case Op::LT: return "LT";
        case Op::GT: return "GT";
        case Op::LE: return "LE";
        case Op::GE: return "GE";
        case Op::AND: return "AND";
        case Op::OR: return "OR";
        case Op::NEG: return "NEG";
        case Op::NOT: return "NOT";
        case Op::POP: return "POP";
        case Op::DUP: return "DUP";
        case Op::MAKE_VEC: return "MAKE_VEC";
        case Op::MAKE_DICT: return "MAKE_DICT";
        case Op::GET_INDEX: return "GET_INDEX";
        case Op::SET_INDEX: return "SET_INDEX";
        case Op::OUTPUT: return "OUTPUT";
        case Op::JMP: return "JMP";
        case Op::JMPF: return "JMPF";
        case Op::JMPR: return "JMPR";
        case Op::CALL: return "CALL";
        case Op::RET: return "RET";
        case Op::CALL_NATIVE: return "CALL_NATIVE";
        case Op::HALT: return "HALT";
        case Op::LEN: return "LEN";
        case Op::VEC_PUSH: return "VEC_PUSH";
        case Op::Q_BIN: return "Q_BIN";
        case Op::Q_BIN_STORE: return "Q_BIN_STORE";
        case Op::Q_BIN_JMPF: return "Q_BIN_JMPF";
        case Op::Q_CMP_JMPF: return "Q_CMP_JMPF";
        case Op::Q_GETIDX: return "Q_GETIDX";
    }
    return "?";
}

std::string disassemble(const IRModule& mod) {
    std::stringstream ss;
    ss << "IR Module: " << mod.file << "\n";
    ss << "  constants: ints=" << mod.int_pool.size()
       << " floats=" << mod.float_pool.size()
       << " strs=" << mod.str_pool.size() << "\n";
    ss << "  globals: " << mod.global_names.size() << "\n";
    ss << "  builtins: " << mod.builtin_names.size() << "\n";
    ss << "  functions: " << mod.functions.size() << " (main=" << mod.main_func_index << ")\n\n";
    for (size_t fi = 0; fi < mod.functions.size(); fi++) {
        const auto& fn = mod.functions[fi];
        ss << "function " << fi << ": " << fn.name
           << " (params=" << fn.param_names.size()
           << ", locals=" << fn.num_locals << ")\n";
        for (size_t i = 0; i < fn.code.size(); i++) {
            const auto& ins = fn.code[i];
            ss << "  " << i << ": " << op_name(ins.op);
            if (ins.op == Op::PUSH_INT) ss << " #" << ins.arg << " (" << mod.int_pool[ins.arg] << ")";
            else if (ins.op == Op::PUSH_FLOAT) ss << " #" << ins.arg << " (" << mod.float_pool[ins.arg] << ")";
            else if (ins.op == Op::PUSH_STR || ins.op == Op::PUSH_SINK) ss << " #" << ins.arg << " (\"" << mod.str_pool[ins.arg] << "\")";
            else if (ins.op == Op::PUSH_BOOL) ss << " " << (ins.arg ? "true" : "false");
            else if (ins.op == Op::PUSH_FUNC) ss << " fn#" << ins.arg;
            else if (ins.op == Op::PUSH_BUILTIN) ss << " " << mod.builtin_names[ins.arg];
            else if (ins.op == Op::CALL_NATIVE) {
                // fib was pain.
                int builtin_idx = ins.arg / 1000;
                int nargs = ins.arg % 1000;
                if (builtin_idx >= 0 && builtin_idx < (int)mod.builtin_names.size()) {
                    ss << " " << mod.builtin_names[builtin_idx] << " args=" << nargs;
                } else {
                    ss << " <bad builtin index " << builtin_idx << ">";
                }
            }
            else if (ins.op == Op::LOAD_LOCAL || ins.op == Op::STORE_LOCAL) ss << " slot" << ins.arg;
            else if (ins.op == Op::LOAD_LOCAL_BOXED || ins.op == Op::STORE_LOCAL_BOXED) ss << " slot" << ins.arg << " (boxed)";
            else if (ins.op == Op::LOAD_UPVALUE || ins.op == Op::STORE_UPVALUE || ins.op == Op::LOAD_UPVALUE_BOX) ss << " up" << ins.arg;
            else if (ins.op == Op::LOAD_GLOBAL || ins.op == Op::STORE_GLOBAL) ss << " " << mod.global_names[ins.arg];
            else if (ins.op == Op::JMP || ins.op == Op::JMPF || ins.op == Op::JMPR) ss << " -> " << ins.arg;
            else if (ins.op == Op::TRY_ENTER) ss << " catch -> " << ins.arg;
            else if (ins.op == Op::CALL) ss << " args=" << ins.arg;
            else if (ins.op == Op::MAKE_VEC) ss << " count=" << ins.arg;
            else if (ins.op == Op::MAKE_CLOSURE) ss << " fn#" << ins.arg;
            ss << "    ; line " << ins.line << "\n";
        }
        ss << "\n";
    }
    return ss.str();
}

}
