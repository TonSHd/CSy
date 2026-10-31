
#if defined(__x86_64__) && defined(__linux__)
#define CSY_HAVE_X64_BACKEND 1

static uint32_t x64_sig_code(const std::string& sig) {
    uint32_t code = 0;
    for (size_t i = 0; i < sig.size(); i++) {
        const uint32_t c = sig[i] == 'I' ? 1u : sig[i] == 'F' ? 2u : 3u;
        code |= c << (2 * i);
    }
    return code;
}

namespace x64 {

enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
           R8 = 8, R9, R10, R11, R12, R13, R14, R15 };
enum Cond { CC_O = 0x0, CC_B = 0x2, CC_AE = 0x3, CC_E = 0x4, CC_NE = 0x5, CC_BE = 0x6, CC_A = 0x7,
            CC_S = 0x8, CC_NS = 0x9, CC_P = 0xA, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

enum Helper {
    H_SIN, H_COS, H_TAN, H_ASIN, H_ACOS, H_ATAN, H_SQRT, H_EXP, H_LOG, H_LOG2, H_LOG10,
    H_FLOOR, H_CEIL, H_ROUND, H_FABS, H_SIGN, H_TANH, H_SIGMOID, H_RELU, H_SIGMOID_D, H_RELU_D, H_TANH_D,
    H_ATAN2, H_POW, H_FMAX, H_FMIN, H_CLAMP, H_BOUND_F,
    H_COUNT
};

extern "C" {
static double h_sin(double x) { return std::sin(x); }
static double h_cos(double x) { return std::cos(x); }
static double h_tan(double x) { return std::tan(x); }
static double h_asin(double x) { return std::asin(x); }
static double h_acos(double x) { return std::acos(x); }
static double h_atan(double x) { return std::atan(x); }
static double h_sqrt(double x) { return std::sqrt(x); }
static double h_exp(double x) { return std::exp(x); }
static double h_log(double x) { return std::log(x); }
static double h_log2(double x) { return std::log2(x); }
static double h_log10(double x) { return std::log10(x); }
static double h_floor(double x) { return std::floor(x); }
static double h_ceil(double x) { return std::ceil(x); }
static double h_round(double x) { return std::round(x); }
static double h_fabs(double x) { return std::fabs(x); }
static double h_sign(double x) { return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0); }
static double h_tanh(double x) { return std::tanh(x); }
static double h_sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }
static double h_relu(double x) { return x > 0.0 ? x : 0.0; }
static double h_sigmoid_d(double y) { return y * (1.0 - y); }
static double h_relu_d(double y) { return y > 0.0 ? 1.0 : 0.0; }
static double h_tanh_d(double y) { return 1.0 - y * y; }
static double h_atan2(double a, double b) { return std::atan2(a, b); }
static double h_pow(double a, double b) { return std::pow(a, b); }
static double h_fmax(double a, double b) { return std::fmax(a, b); }
static double h_fmin(double a, double b) { return std::fmin(a, b); }
static double h_clamp(double v, double lo, double hi) {
    if (std::isnan(v) || std::isnan(lo) || std::isnan(hi)) return std::numeric_limits<double>::quiet_NaN();
    return std::fmax(lo, std::fmin(v, hi));
}
static long long h_bound_f(double d) {
    if (!(d > 0.0)) return 0;
    if (d >= 9.2e18) return LLONG_MAX;
    return (long long)d;
}
}

inline void* helper_address(int id) {
    static void* const table[H_COUNT] = {
        (void*)&h_sin, (void*)&h_cos, (void*)&h_tan, (void*)&h_asin, (void*)&h_acos, (void*)&h_atan,
        (void*)&h_sqrt, (void*)&h_exp, (void*)&h_log, (void*)&h_log2, (void*)&h_log10,
        (void*)&h_floor, (void*)&h_ceil, (void*)&h_round, (void*)&h_fabs, (void*)&h_sign, (void*)&h_tanh,
        (void*)&h_sigmoid, (void*)&h_relu, (void*)&h_sigmoid_d, (void*)&h_relu_d, (void*)&h_tanh_d,
        (void*)&h_atan2, (void*)&h_pow, (void*)&h_fmax, (void*)&h_fmin, (void*)&h_clamp, (void*)&h_bound_f};
    return table[id];
}

struct DivMagic { int64_t multiplier; int shift; };
inline DivMagic div_magic_for(int64_t d) {
    const uint64_t two63 = 1ULL << 63;
    const uint64_t ad = (uint64_t)d;
    const uint64_t anc = two63 - 1 - two63 % ad;
    int p = 63;
    uint64_t q1 = two63 / anc, r1 = two63 - q1 * anc;
    uint64_t q2 = two63 / ad, r2 = two63 - q2 * ad;
    uint64_t delta;
    do {
        p++;
        q1 = 2 * q1; r1 = 2 * r1; if (r1 >= anc) { q1++; r1 -= anc; }
        q2 = 2 * q2; r2 = 2 * r2; if (r2 >= ad) { q2++; r2 -= ad; }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    return {(int64_t)(q2 + 1), p - 64};
}

class Asm {
public:
    std::vector<uint8_t> code;

    int new_label() { labels_.push_back({-1, {}}); return (int)labels_.size() - 1; }
    void bind(int l) {
        labels_[l].pos = (int)code.size();
        for (size_t at : labels_[l].fixups) patch32(at, (int32_t)(labels_[l].pos - (int)(at + 4)));
        labels_[l].fixups.clear();
    }
    bool all_bound() const { for (const auto& l : labels_) if (l.pos < 0 && !l.fixups.empty()) return false; return true; }
    size_t pos() const { return code.size(); }

    void b(uint8_t v) { code.push_back(v); }
    void d32(int32_t v) { for (int i = 0; i < 4; i++) code.push_back((uint8_t)((uint32_t)v >> (8 * i))); }
    void d64(uint64_t v) { for (int i = 0; i < 8; i++) code.push_back((uint8_t)(v >> (8 * i))); }
    void patch32(size_t at, int32_t v) { for (int i = 0; i < 4; i++) code[at + i] = (uint8_t)((uint32_t)v >> (8 * i)); }

    void rex(bool w, int reg, int rm, bool force = false) {
        const uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | (((reg >> 3) & 1) << 2) | ((rm >> 3) & 1));
        if (r != 0x40 || force) b(r);
    }
    void modrm_rr(int reg, int rm) { b((uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7))); }
    void modrm_mem(int reg, int base, int32_t disp) {
        b((uint8_t)(0x80 | ((reg & 7) << 3) | ((base & 7) == 4 ? 4 : (base & 7))));
        if ((base & 7) == 4) b(0x24);
        d32(disp);
    }

    void mov_rr(int dst, int src) { rex(true, src, dst); b(0x89); modrm_rr(src, dst); }
    void mov_ri(int dst, int64_t imm) {
        if (imm >= INT32_MIN && imm <= INT32_MAX) { rex(true, 0, dst); b(0xC7); modrm_rr(0, dst); d32((int32_t)imm); }
        else { rex(true, 0, dst); b((uint8_t)(0xB8 + (dst & 7))); d64((uint64_t)imm); }
    }
    void mov_rm(int dst, int base, int32_t disp) { rex(true, dst, base); b(0x8B); modrm_mem(dst, base, disp); }
    void mov_mr(int base, int32_t disp, int src) { rex(true, src, base); b(0x89); modrm_mem(src, base, disp); }
    void mov_m32_imm(int base, int32_t disp, int32_t imm) { rex(false, 0, base); b(0xC7); modrm_mem(0, base, disp); d32(imm); }
    void mov_m32r(int base, int32_t disp, int src) { rex(false, src, base); b(0x89); modrm_mem(src, base, disp); }
    void cmp_m32_imm8(int base, int32_t disp, int8_t imm) { rex(false, 0, base); b(0x83); modrm_mem(7, base, disp); b((uint8_t)imm); }
    void alu_rr(uint8_t opcode, int dst, int src) { rex(true, src, dst); b(opcode); modrm_rr(src, dst); }
    void alu_ri(int ext, int dst, int32_t imm) { rex(true, 0, dst); b(0x81); modrm_rr(ext, dst); d32(imm); }
    void cmp_rm(int reg, int base, int32_t disp) { rex(true, reg, base); b(0x3B); modrm_mem(reg, base, disp); }
    void imul_rr(int dst, int src) { rex(true, dst, src); b(0x0F); b(0xAF); modrm_rr(dst, src); }
    void imul_rri(int dst, int src, int32_t imm) { rex(true, dst, src); b(0x69); modrm_rr(dst, src); d32(imm); }
    void idiv_r(int r) { rex(true, 0, r); b(0xF7); modrm_rr(7, r); }
    void neg_r(int r) { rex(true, 0, r); b(0xF7); modrm_rr(3, r); }
    void cqo() { b(0x48); b(0x99); }
    void shift_ri(int ext, int r, uint8_t n) { rex(true, 0, r); b(0xC1); modrm_rr(ext, r); b(n); }
    void imul1(int r) { rex(true, 0, r); b(0xF7); modrm_rr(5, r); }
    void sqrtsd(int d, int s) { sse_rr(0xF2, 0x51, d, s); }
    void andpd(int d, int s) { sse_rr(0x66, 0x54, d, s); }
    void inc_m(int base, int32_t disp) { rex(true, 0, base); b(0xFF); modrm_mem(0, base, disp); }
    void inc_r(int r) { rex(true, 0, r); b(0xFF); modrm_rr(0, r); }
    void dec_r(int r) { rex(true, 0, r); b(0xFF); modrm_rr(1, r); }
    void lea(int dst, int base, int32_t disp) { rex(true, dst, base); b(0x8D); modrm_mem(dst, base, disp); }
    void push_r(int r) { if (r >= 8) b(0x41); b((uint8_t)(0x50 + (r & 7))); }
    void pop_r(int r) { if (r >= 8) b(0x41); b((uint8_t)(0x58 + (r & 7))); }
    void push_m(int base, int32_t disp) { rex(false, 0, base); b(0xFF); modrm_mem(6, base, disp); }
    void ret() { b(0xC3); }
    void leave() { b(0xC9); }
    void call_got(int slot, size_t got_base_pos) {
        b(0xFF); b(0x15);
        const int64_t target = (int64_t)(got_base_pos + (size_t)slot * 8);
        d32((int32_t)(target - (int64_t)(code.size() + 4)));
    }
    void jmp(int l) { b(0xE9); ref(l); }
    void jcc(int cc, int l) { b(0x0F); b((uint8_t)(0x80 | cc)); ref(l); }
    void call(int l) { b(0xE8); ref(l); }

    void sse_rr(uint8_t prefix, uint8_t op, int dst, int src) { b(prefix); rex(false, dst, src); b(0x0F); b(op); modrm_rr(dst, src); }
    void sse_rm(uint8_t prefix, uint8_t op, int xmm, int base, int32_t disp) { b(prefix); rex(false, xmm, base); b(0x0F); b(op); modrm_mem(xmm, base, disp); }
    void movsd_load(int xmm, int base, int32_t disp) { sse_rm(0xF2, 0x10, xmm, base, disp); }
    void movsd_store(int base, int32_t disp, int xmm) { sse_rm(0xF2, 0x11, xmm, base, disp); }
    void sse_idx(uint8_t op, int xmm, int base, int index) {
        b(0xF2);
        const uint8_t r = (uint8_t)(0x40 | (((xmm >> 3) & 1) << 2) | (((index >> 3) & 1) << 1) | ((base >> 3) & 1));
        if (r != 0x40) b(r);
        b(0x0F); b(op);
        b((uint8_t)(0x04 | ((xmm & 7) << 3)));
        b((uint8_t)(0xC0 | ((index & 7) << 3) | (base & 7)));
    }
    void sse_idx_disp(uint8_t op, int xmm, int base, int index, int32_t disp) {
        b(0x66);
        const uint8_t r = (uint8_t)(0x40 | (((xmm >> 3) & 1) << 2) | (((index >> 3) & 1) << 1) | ((base >> 3) & 1));
        if (r != 0x40) b(r);
        b(0x0F); b(op);
        b((uint8_t)(0x84 | ((xmm & 7) << 3)));
        b((uint8_t)(0xC0 | ((index & 7) << 3) | (base & 7)));
        d32(disp);
    }
    void movupd_load_idx(int xmm, int base, int index, int32_t disp) { sse_idx_disp(0x10, xmm, base, index, disp); }
    void movupd_store_idx(int base, int index, int32_t disp, int xmm) { sse_idx_disp(0x11, xmm, base, index, disp); }
    void addpd(int d, int s) { sse_rr(0x66, 0x58, d, s); }
    void subpd(int d, int s) { sse_rr(0x66, 0x5C, d, s); }
    void mulpd(int d, int s) { sse_rr(0x66, 0x59, d, s); }
    void unpcklpd(int d, int s) { sse_rr(0x66, 0x14, d, s); }
    void movsd_load_idx(int xmm, int base, int index) { sse_idx(0x10, xmm, base, index); }
    void movsd_store_idx(int base, int index, int xmm) { sse_idx(0x11, xmm, base, index); }
    void addsd(int d, int s) { sse_rr(0xF2, 0x58, d, s); }
    void subsd(int d, int s) { sse_rr(0xF2, 0x5C, d, s); }
    void mulsd(int d, int s) { sse_rr(0xF2, 0x59, d, s); }
    void divsd(int d, int s) { sse_rr(0xF2, 0x5E, d, s); }
    void ucomisd(int a, int bb) { sse_rr(0x66, 0x2E, a, bb); }
    void xorpd(int d, int s) { sse_rr(0x66, 0x57, d, s); }
    void movapd(int d, int s) { sse_rr(0x66, 0x28, d, s); }
    void movq_xr(int xmm, int r) { b(0x66); rex(true, xmm, r); b(0x0F); b(0x6E); modrm_rr(xmm, r); }
    void movq_rx(int r, int xmm) { b(0x66); rex(true, xmm, r); b(0x0F); b(0x7E); modrm_rr(xmm, r); }
    void cvtsi2sd(int xmm, int r) { xorpd(xmm, xmm); b(0xF2); rex(true, xmm, r); b(0x0F); b(0x2A); modrm_rr(xmm, r); }
    void cvttsd2si(int r, int xmm) { b(0xF2); rex(true, r, xmm); b(0x0F); b(0x2C); modrm_rr(r, xmm); }

private:
    struct Label { int pos; std::vector<size_t> fixups; };
    std::vector<Label> labels_;
    void ref(int l) {
        const size_t at = code.size();
        d32(0);
        if (labels_[l].pos >= 0) patch32(at, (int32_t)(labels_[l].pos - (int)(at + 4)));
        else labels_[l].fixups.push_back(at);
    }
};

struct Entry {
    std::string name;
    std::string sig;
    std::vector<std::string> global_names;
    uint8_t user_nparams = 0;
    uint32_t offset = 0;
    bool ret_float = false;
};
struct Image {
    std::string blob;
    std::vector<Entry> entries;
};

constexpr uint32_t kMaxNativeDepth = 20000;

}

class X64CodeGen {
public:
    explicit X64CodeGen(InstMap& insts) : fns_(insts) {}

    bool generate(const std::vector<std::string>& emit_order, const std::vector<std::string>& exported,
                  x64::Image& image, std::string& why) {
        using namespace x64;
        got_bytes_ = (size_t)H_COUNT * 8;
        for (size_t i = 0; i < got_bytes_; i++) a_.b(0);
        for (const auto& key : emit_order) fn_label_[key] = a_.new_label();
        for (const auto& key : emit_order) {
            if (!emit_function(fns_.at(key), key)) { why = why_; return false; }
        }
        for (const auto& key : exported) {
            const FnInfo& f = fns_.at(key);
            const size_t off = a_.pos();
            emit_entry(f, key);
            Entry e;
            e.name = f.base_name; e.sig = f.sig; e.offset = (uint32_t)off; e.ret_float = f.return_type == NType::Float;
            e.user_nparams = static_cast<uint8_t>(f.user_param_count);
            e.global_names = f.global_params;
            image.entries.push_back(std::move(e));
        }
        if (!a_.all_bound()) { why = "internal error: unbound label"; return false; }
        image.blob.assign(reinterpret_cast<const char*>(a_.code.data()), a_.code.size());
        return true;
    }

    struct LoopStat { int vector_loops = 0; int other_loops = 0; };
    const std::unordered_map<std::string, LoopStat>& loop_stats() const { return loop_stats_; }

private:
    using Asm = x64::Asm;
    InstMap& fns_;
    std::unordered_map<std::string, LoopStat> loop_stats_;
    LoopStat* cur_stat_ = nullptr;
    std::vector<bool> loop_has_vec_;
    void loop_enter() { loop_has_vec_.push_back(false); }
    void loop_leave(bool is_vec) {
        const bool vec = is_vec || (!loop_has_vec_.empty() && loop_has_vec_.back());
        if (!loop_has_vec_.empty()) loop_has_vec_.pop_back();
        if (vec) { if (!loop_has_vec_.empty()) loop_has_vec_.back() = true; }
        else if (cur_stat_) cur_stat_->other_loops++;
    }
    Asm a_;
    size_t got_bytes_ = 0;
    std::unordered_map<std::string, int> fn_label_;
    std::string why_;

    struct Loop { int cont; int brk; };
    struct Var { int slot = 0; int reg = -1; bool is_float = false; };
    static constexpr int kIntPool[8] = {x64::RBX, x64::R15, x64::RSI, x64::RDI, x64::R8, x64::R9, x64::R10, x64::R11};
    static int enc_reg(bool is_float, int r) { return r | (is_float ? 256 : 0); }
    static bool int_reg_clobbered_by_c(int r) { return r == x64::RSI || r == x64::RDI || (r >= 8 && r <= 11); }
    struct Ctx {
        FnInfo* fn = nullptr;
        std::unordered_map<std::string, Var> vars;
        std::unordered_map<std::string, int> tag_slots;
        std::unordered_map<const Expr*, int> call_tag_slots;
        std::unordered_map<const Stmt*, int> loop_ids;
        std::vector<int> used_regs;
        std::unordered_map<int, int> save_slot;
        int epilogue = -1;
        int inline_exit = -1;
        std::vector<const std::unordered_map<std::string, Var>*> outer_scopes;
        int inline_expansions = 0;
        int next_slot = 1;
        int depth = 0;
        int ret_zero = -1, err_div = -1, err_ovf = -1, err_idx = -1;
        std::vector<Loop> loops;
    } c_;

    bool fail(const std::string& why) { if (why_.empty()) why_ = why; return false; }
    int new_slot() { return -8 * (++c_.next_slot); }
    Var& var(const std::string& name) { return c_.vars.at(name); }
    bool has_var(const std::string& name) { return c_.vars.count(name) != 0; }
    void get_int(const std::string& n, int dst) {
        Var& v = var(n);
        if (v.reg >= 0) { if (v.reg != dst) a_.mov_rr(dst, v.reg); } else a_.mov_rm(dst, x64::RBP, v.slot);
    }
    void put_int(const std::string& n, int src) {
        Var& v = var(n);
        if (v.reg >= 0) { if (v.reg != src) a_.mov_rr(v.reg, src); } else a_.mov_mr(x64::RBP, v.slot, src);
    }
    bool has_dynamic_tag(const FnInfo& fn, const std::string& name) const {
        auto it = fn.seen.find(name);
        return it != fn.seen.end() && (it->second & 3) == 3;
    }
    void get_tag(const std::string& n, int dst = x64::RAX) {
        a_.mov_rm(dst, x64::RBP, c_.tag_slots.at(n));
    }
    void put_tag(const std::string& n, int src = x64::RAX) {
        a_.mov_mr(x64::RBP, c_.tag_slots.at(n), src);
    }
    void get_float(const std::string& n, int xdst) {
        Var& v = var(n);
        if (v.reg >= 0) { if (v.reg != xdst) a_.movapd(xdst, v.reg); } else a_.movsd_load(xdst, x64::RBP, v.slot);
    }
    void put_float(const std::string& n, int xsrc) {
        Var& v = var(n);
        if (v.reg >= 0) { if (v.reg != xsrc) a_.movapd(v.reg, xsrc); } else a_.movsd_store(x64::RBP, v.slot, xsrc);
    }

    NType type_of(const ExprPtr& e) { return infer_expr_type(fns_, *c_.fn, e); }
    NType local_type(const std::string& name) { return c_.fn->local_types.at(name); }

    static uint64_t bits(double v) { uint64_t u; std::memcpy(&u, &v, 8); return u; }

    void push_val(NType t) {
        if (t == NType::Float) a_.movq_rx(x64::RAX, 0);
        a_.push_r(x64::RAX);
        c_.depth += 8;
    }
    void pop_val(NType t) {
        a_.pop_r(x64::RAX);
        c_.depth -= 8;
        if (t == NType::Float) a_.movq_xr(0, x64::RAX);
    }
    void align_call_begin(bool& padded) {
        padded = (c_.depth % 16) != 0;
        if (padded) a_.alu_ri(5, x64::RSP, 8);
    }
    void align_call_end(bool padded) { if (padded) a_.alu_ri(0, x64::RSP, 8); }

    void err_check() { a_.cmp_m32_imm8(x64::R12, 0, 0); a_.jcc(x64::CC_NE, c_.ret_zero); }

    bool is_simple(const ExprPtr& e) {
        return e->kind == ExprKind::Int || e->kind == ExprKind::Float ||
               (e->kind == ExprKind::Ident && local_type_ok(e->name));
    }
    bool local_type_ok(const std::string& name) {
        auto it = c_.fn->local_types.find(name);
        return it != c_.fn->local_types.end() && it->second != NType::Array;
    }

    bool gen_to(const ExprPtr& e, NType want) {
        if (!gen_expr(e)) return false;
        const NType t = type_of(e);
        if (t == want) return true;
        if (t == NType::Int && want == NType::Float) { a_.cvtsi2sd(0, x64::RAX); return true; }
        return fail("float value used where an int is required");
    }

    bool load_simple(const ExprPtr& e, NType want, int idst = x64::RCX, int xdst = 1) {
        if (e->kind == ExprKind::Int) {
            if (want == NType::Float) { a_.mov_ri(idst, (int64_t)bits((double)e->int_val)); a_.movq_xr(xdst, idst); }
            else a_.mov_ri(idst, e->int_val);
            return true;
        }
        if (e->kind == ExprKind::Float) {
            if (want != NType::Float) return fail("float literal where an int is required");
            a_.mov_ri(idst, (int64_t)bits(e->float_val));
            a_.movq_xr(xdst, idst);
            return true;
        }
        const NType t = local_type(e->name);
        if (t == NType::Float) {
            if (want != NType::Float) return fail("float variable where an int is required");
            get_float(e->name, xdst);
        } else if (want == NType::Float) {
            const Var& v = var(e->name);
            if (v.reg >= 0) a_.cvtsi2sd(xdst, v.reg);
            else { a_.mov_rm(idst, x64::RBP, v.slot); a_.cvtsi2sd(xdst, idst); }
        } else {
            get_int(e->name, idst);
        }
        return true;
    }

    bool gen_operands(const ExprPtr& l, const ExprPtr& r, NType t) {
        if (is_simple(r)) {
            if (!gen_to(l, t)) return false;
            return load_simple(r, t);
        }
        if (is_simple(l)) {
            if (!gen_to(r, t)) return false;
            if (t == NType::Float) a_.movapd(1, 0); else a_.mov_rr(x64::RCX, x64::RAX);
            return load_simple(l, t, x64::RAX, 0);
        }
        if (!gen_to(l, t)) return false;
        push_val(t);
        if (!gen_to(r, t)) return false;
        if (t == NType::Float) a_.movapd(1, 0); else a_.mov_rr(x64::RCX, x64::RAX);
        pop_val(t);
        return true;
    }

    bool is_bool_expr(const ExprPtr& e) {
        if (e->kind == ExprKind::Unary && e->op != TokenType::MINUS) return true;
        if (e->kind != ExprKind::Binary) return false;
        switch (e->op) {
            case TokenType::LT: case TokenType::GT: case TokenType::LE: case TokenType::GE:
            case TokenType::EQ: case TokenType::NEQ: case TokenType::AND: case TokenType::OR: return true;
            default: return false;
        }
    }

    bool gen_expr(const ExprPtr& e) {
        using namespace x64;
        switch (e->kind) {
            case ExprKind::Int: a_.mov_ri(RAX, e->int_val); return true;
            case ExprKind::Float: a_.mov_ri(RAX, (int64_t)bits(e->float_val)); a_.movq_xr(0, RAX); return true;
            case ExprKind::Ident: {
                if (!local_type_ok(e->name)) return fail("array used as a value");
                if (local_type(e->name) == NType::Float) get_float(e->name, 0);
                else get_int(e->name, RAX);
                return true;
            }
            case ExprKind::Unary: {
                if (e->op == TokenType::MINUS) {
                    if (!gen_expr(e->operand)) return false;
                    if (type_of(e->operand) == NType::Int) { a_.neg_r(RAX); a_.jcc(CC_O, c_.err_ovf); }
                    else {
                        a_.mov_ri(RCX, (int64_t)0x8000000000000000ULL);
                        a_.movq_xr(1, RCX);
                        a_.xorpd(0, 1);
                    }
                    return true;
                }
                return gen_bool_value(e);
            }
            case ExprKind::Binary: return gen_binary(e);
            case ExprKind::Call: return gen_call(e);
            case ExprKind::Index: return gen_index_read(e);
            default: return fail("unsupported expression kind");
        }
    }

    bool gen_expr_tag(const ExprPtr& e) {
        using namespace x64;
        if (!e) { a_.mov_ri(RAX, 0); return true; }
        switch (e->kind) {
            case ExprKind::Int: a_.mov_ri(RAX, 0); return true;
            case ExprKind::Float: a_.mov_ri(RAX, 1); return true;
            case ExprKind::Ident: {
                auto local = c_.fn->local_types.find(e->name);
                if (local == c_.fn->local_types.end() || local->second != NType::Float) {
                    a_.mov_ri(RAX, 0);
                } else if (has_dynamic_tag(*c_.fn, e->name)) {
                    get_tag(e->name);
                } else {
                    a_.mov_ri(RAX, 1);
                }
                return true;
            }
            case ExprKind::Unary:
                if (e->op == TokenType::MINUS) return gen_expr_tag(e->operand);
                a_.mov_ri(RAX, 0);
                return true;
            case ExprKind::Binary:
                if (e->op != TokenType::PLUS && e->op != TokenType::MINUS &&
                    e->op != TokenType::STAR && e->op != TokenType::SLASH) {
                    a_.mov_ri(RAX, 0);
                    return true;
                }
                if (!gen_expr_tag(e->left)) return false;
                a_.push_r(RAX); c_.depth += 8;
                if (!gen_expr_tag(e->right)) return false;
                a_.mov_rr(RCX, RAX);
                a_.pop_r(RAX); c_.depth -= 8;
                a_.alu_rr(0x09, RAX, RCX);
                return true;
            case ExprKind::Index: a_.mov_ri(RAX, 1); return true;
            case ExprKind::Call: {
                auto math = math_whitelist().find(e->callee->name);
                if (math != math_whitelist().end()) {
                    if (math->second.kind != MathKind::Promote2) {
                        a_.mov_ri(RAX, 1);
                        return true;
                    }
                    if (!gen_expr_tag(e->args[0])) return false;
                    a_.push_r(RAX); c_.depth += 8;
                    if (!gen_expr_tag(e->args[1])) return false;
                    a_.mov_rr(RCX, RAX);
                    a_.pop_r(RAX); c_.depth -= 8;
                    a_.alu_rr(0x09, RAX, RCX);
                    return true;
                }
                std::string key;
                if (!callee_key(fns_, *c_.fn, e, key)) return fail("unresolved call tag");
                const FnInfo& callee = fns_.at(key);
                if (returns_maybe_int(callee)) {
                    auto tag = c_.call_tag_slots.find(e.get());
                    if (tag == c_.call_tag_slots.end()) return fail("missing dynamic call-result tag slot");
                    a_.mov_rm(RAX, RBP, tag->second);
                } else {
                    a_.mov_ri(RAX, callee.return_type == NType::Float ? 1 : 0);
                }
                return true;
            }
            default: a_.mov_ri(RAX, 0); return true;
        }
    }

    bool gen_bool_value(const ExprPtr& e) {
        const int t = a_.new_label(), f = a_.new_label(), end = a_.new_label();
        if (!gen_cond(e, t, f, t)) return false;
        a_.bind(t); a_.mov_ri(x64::RAX, 1); a_.jmp(end);
        a_.bind(f); a_.mov_ri(x64::RAX, 0);
        a_.bind(end);
        return true;
    }

    bool gen_binary(const ExprPtr& e) {
        using namespace x64;
        if (is_bool_expr(e)) return gen_bool_value(e);
        const NType t = type_of(e);
        if ((e->op == TokenType::SLASH || e->op == TokenType::PERCENT) && t == NType::Int &&
            e->right->kind == ExprKind::Int && e->right->int_val >= 2 &&
            type_of(e->left) == NType::Int) {
            if (!gen_expr(e->left)) return false;
            gen_div_const(e->right->int_val, e->op == TokenType::PERCENT);
            return true;
        }
        if (e->op == TokenType::SLASH) {
            if (t == NType::Float) {
                if (!gen_operands(e->left, e->right, NType::Float)) return false;
                const int ok = a_.new_label();
                a_.xorpd(2, 2);
                a_.ucomisd(1, 2);
                a_.jcc(CC_P, ok);
                a_.jcc(CC_NE, ok);
                a_.jmp(c_.err_div);
                a_.bind(ok);
                a_.divsd(0, 1);
                return true;
            }
            if (!gen_operands(e->left, e->right, NType::Int)) return false;
            const int normal = a_.new_label(), done = a_.new_label();
            a_.alu_rr(0x85, RCX, RCX);
            a_.jcc(CC_E, c_.err_div);
            a_.alu_ri(7, RCX, -1);
            a_.jcc(CC_NE, normal);
            a_.neg_r(RAX);
            a_.jcc(CC_O, c_.err_ovf);
            a_.jmp(done);
            a_.bind(normal);
            a_.cqo();
            a_.idiv_r(RCX);
            a_.bind(done);
            return true;
        }
        if (e->op == TokenType::PLUS || e->op == TokenType::MINUS || e->op == TokenType::STAR || e->op == TokenType::PERCENT) {
            if (t == NType::Int) {
                if (!gen_operands(e->left, e->right, NType::Int)) return false;
                switch (e->op) {
                    case TokenType::PLUS: a_.alu_rr(0x01, RAX, RCX); a_.jcc(CC_O, c_.err_ovf); break;
                    case TokenType::MINUS: a_.alu_rr(0x29, RAX, RCX); a_.jcc(CC_O, c_.err_ovf); break;
                    case TokenType::STAR: a_.imul_rr(RAX, RCX); a_.jcc(CC_O, c_.err_ovf); break;
                    default: return gen_mod();
                }
                return true;
            }
            if (e->op == TokenType::PERCENT) return fail("float modulo");
            if (!gen_operands(e->left, e->right, NType::Float)) return false;
            switch (e->op) {
                case TokenType::PLUS: a_.addsd(0, 1); break;
                case TokenType::MINUS: a_.subsd(0, 1); break;
                default: a_.mulsd(0, 1); break;
            }
            return true;
        }
        return fail("unsupported binary operator");
    }

    void gen_div_const(int64_t c, bool want_mod) {
        using namespace x64;
        a_.mov_rr(RCX, RAX);
        if ((c & (c - 1)) == 0) {
            const int k = __builtin_ctzll((uint64_t)c);
            a_.mov_rr(RDX, RAX);
            a_.shift_ri(7, RDX, 63);
            if (c - 1 <= INT32_MAX) a_.alu_ri(4, RDX, (int32_t)(c - 1));
            else { a_.mov_ri(RAX, c - 1); a_.alu_rr(0x21, RDX, RAX); }
            a_.mov_rr(RAX, RCX);
            a_.alu_rr(0x01, RAX, RDX);
            a_.shift_ri(7, RAX, (uint8_t)k);
            if (want_mod) {
                a_.shift_ri(4, RAX, (uint8_t)k);
                a_.alu_rr(0x29, RCX, RAX);
                a_.mov_rr(RAX, RCX);
            }
            return;
        }
        const DivMagic m = div_magic_for(c);
        a_.mov_ri(RAX, m.multiplier);
        a_.imul1(RCX);
        if (m.multiplier < 0) a_.alu_rr(0x01, RDX, RCX);
        if (m.shift > 0) a_.shift_ri(7, RDX, (uint8_t)m.shift);
        a_.mov_rr(RAX, RDX);
        a_.shift_ri(5, RAX, 63);
        a_.alu_rr(0x01, RDX, RAX);
        if (want_mod) {
            if (c <= INT32_MAX) a_.imul_rri(RDX, RDX, (int32_t)c);
            else { a_.mov_ri(RAX, c); a_.imul_rr(RDX, RAX); }
            a_.alu_rr(0x29, RCX, RDX);
            a_.mov_rr(RAX, RCX);
        } else {
            a_.mov_rr(RAX, RDX);
        }
    }

    bool gen_mod() {
        using namespace x64;
        const int ok = a_.new_label(), done = a_.new_label();
        a_.alu_rr(0x85, RCX, RCX);
        a_.jcc(CC_E, c_.err_div);
        a_.alu_ri(7, RCX, -1);
        a_.jcc(CC_NE, ok);
        a_.mov_ri(RAX, 0);
        a_.jmp(done);
        a_.bind(ok);
        a_.cqo();
        a_.idiv_r(RCX);
        a_.mov_rr(RAX, RDX);
        a_.bind(done);
        return true;
    }

    bool gen_call(const ExprPtr& e) {
        using namespace x64;
        const std::string& name = e->callee->name;
        auto mit = math_whitelist().find(name);
        if (mit != math_whitelist().end()) return gen_math(e, name, mit->second);
        std::string key;
        callee_key(fns_, *c_.fn, e, key);
        FnInfo& callee = fns_.at(key);
        if (c_.inline_exit < 0 && c_.inline_expansions < kMaxInlineExpansions && can_inline(key)) {
            c_.inline_expansions++;
            return gen_inline_call(e, callee);
        }
        int nslots = 0;
        for (size_t i = 0; i < callee.sig.size(); i++) nslots += callee.sig[i] == 'A' || callee.sig[i] == 'M' ? 2 : 1;
        int pad = (16 - ((c_.depth + nslots * 8) % 16)) % 16;
        if (pad) { a_.alu_ri(5, RSP, pad); c_.depth += pad; }
        for (size_t i = 0; i < e->args.size(); i++) {
            const ExprPtr& arg = e->args[i];
            switch (callee.sig[i]) {
                case 'A': {
                    if (arg->kind != ExprKind::Ident || !has_var(arg->name) || !has_var("len_" + arg->name))
                        return fail("array argument is not a plain array variable");
                    for (const std::string& part : {arg->name, "len_" + arg->name}) {
                        const Var& v = var(part);
                        if (v.reg >= 0) a_.push_r(v.reg); else a_.push_m(RBP, v.slot);
                        c_.depth += 8;
                    }
                    break;
                }
                case 'I':
                    if (!gen_expr(arg) || type_of(arg) != NType::Int) return fail("non-int argument for an int parameter");
                    a_.push_r(RAX); c_.depth += 8;
                    break;
                default:
                    if (!gen_to(arg, NType::Float)) return false;
                    a_.movq_rx(RAX, 0); a_.push_r(RAX); c_.depth += 8;
                    if (callee.sig[i] == 'M') {
                        if (!gen_expr_tag(arg)) return false;
                        a_.push_r(RAX); c_.depth += 8;
                    }
                break;
            }
        }
        for (size_t i = 0; i < callee.global_params.size(); i++) {
            const size_t p = callee.user_param_count + i;
            const std::string& global = callee.global_params[i];
            ExprPtr arg = std::make_shared<Expr>();
            arg->kind = ExprKind::Ident;
            arg->name = global;
            if (callee.sig[p] == 'I') {
                if (!gen_expr(arg) || type_of(arg) != NType::Int) return fail("non-int hidden module-global argument");
                a_.push_r(RAX); c_.depth += 8;
            } else {
                if (!gen_to(arg, NType::Float)) return false;
                a_.movq_rx(RAX, 0); a_.push_r(RAX); c_.depth += 8;
                if (callee.sig[p] == 'M') {
                    if (!gen_expr_tag(arg)) return false;
                    a_.push_r(RAX); c_.depth += 8;
                }
            }
        }
        a_.inc_r(R13);
        a_.call(fn_label_.at(key));
        if (returns_maybe_int(callee)) {
            auto tag = c_.call_tag_slots.find(e.get());
            if (tag == c_.call_tag_slots.end()) return fail("missing dynamic call-result tag slot");
            a_.mov_mr(RBP, tag->second, RDX);
        }
        a_.dec_r(R13);
        a_.alu_ri(0, RSP, nslots * 8 + pad);
        c_.depth -= nslots * 8 + pad;
        err_check();
        return true;
    }


    static constexpr int kMaxInlineExpansions = 32;
    static constexpr int kMaxInlineNodes = 56;
    std::unordered_map<std::string, int> inline_ok_;

    static int count_expr(const ExprPtr& e) {
        if (!e) return 0;
        int n = 1;
        n += count_expr(e->left) + count_expr(e->right) + count_expr(e->operand) + count_expr(e->target) +
             count_expr(e->value) + count_expr(e->index) + count_expr(e->object);
        for (const auto& a : e->args) n += count_expr(a);
        return n;
    }
    static bool leaf_expr_ok(const ExprPtr& e) {
        if (!e) return true;
        if (e->kind == ExprKind::Call) {
            if (!e->callee || e->callee->kind != ExprKind::Ident || !math_whitelist().count(e->callee->name)) return false;
        }
        if (!leaf_expr_ok(e->left) || !leaf_expr_ok(e->right) || !leaf_expr_ok(e->operand) || !leaf_expr_ok(e->target) ||
            !leaf_expr_ok(e->value) || !leaf_expr_ok(e->index) || !leaf_expr_ok(e->object)) return false;
        for (const auto& a : e->args) if (!leaf_expr_ok(a)) return false;
        return true;
    }
    static bool leaf_stmts_ok(const std::vector<StmtPtr>& body, int& nodes) {
        for (const auto& s : body) {
            if (!s) continue;
            nodes += 1;
            switch (s->kind) {
                case StmtKind::VarDecl:
                case StmtKind::ExprStmt:
                    if (!leaf_expr_ok(s->expr)) return false;
                    nodes += count_expr(s->expr);
                    break;
                case StmtKind::Return:
                    if (!leaf_expr_ok(s->ret_value)) return false;
                    nodes += count_expr(s->ret_value);
                    break;
                case StmtKind::If:
                    if (!leaf_expr_ok(s->cond)) return false;
                    nodes += count_expr(s->cond);
                    if (!leaf_stmts_ok(s->then_body, nodes) || !leaf_stmts_ok(s->else_body, nodes)) return false;
                    for (const auto& el : s->elifs) {
                        if (!leaf_expr_ok(el.first)) return false;
                        nodes += count_expr(el.first);
                        if (!leaf_stmts_ok(el.second, nodes)) return false;
                    }
                    break;
                default:
                    return false;
            }
            if (nodes > kMaxInlineNodes) return false;
        }
        return true;
    }
    bool can_inline(const std::string& key) {
        auto it = inline_ok_.find(key);
        if (it != inline_ok_.end()) return it->second != 0;
        int nodes = 0;
        const FnInfo& f = fns_.at(key);
        bool has_mixed_local = false;
        for (const auto& entry : f.seen) if ((entry.second & 3) == 3) has_mixed_local = true;
        bool ok = !has_mixed_local && f.sig.find('M') == std::string::npos && !returns_maybe_int(f) &&
                  leaf_stmts_ok(f.decl->body, nodes) && nodes <= kMaxInlineNodes;
        inline_ok_[key] = ok ? 1 : 0;
        return ok;
    }

    bool gen_inline_call(const ExprPtr& e, FnInfo& callee) {
        using namespace x64;
        std::unordered_map<std::string, Var> frame;
        std::vector<std::string> names;
        for (const auto& kv : callee.local_types) {
            names.push_back(kv.first);
            if (kv.second == NType::Array) names.push_back("len_" + kv.first);
        }
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            Var v;
            auto lt = callee.local_types.find(n);
            v.is_float = lt != callee.local_types.end() && lt->second == NType::Float;
            v.slot = new_slot();
            frame[n] = v;
        }
        for (size_t i = 0; i < e->args.size(); i++) {
            const ExprPtr& arg = e->args[i];
            const std::string& p = callee.param_names[i];
            const NType lt = callee.local_types.at(p);
            switch (callee.sig[i]) {
                case 'A':
                    if (arg->kind != ExprKind::Ident || !has_var(arg->name) || !has_var("len_" + arg->name))
                        return fail("array argument is not a plain array variable");
                    get_int(arg->name, RAX);
                    a_.mov_mr(RBP, frame.at(p).slot, RAX);
                    get_int("len_" + arg->name, RAX);
                    a_.mov_mr(RBP, frame.at("len_" + p).slot, RAX);
                    break;
                case 'I':
                    if (!gen_expr(arg) || type_of(arg) != NType::Int) return fail("non-int argument for an int parameter");
                    if (lt == NType::Float) { a_.cvtsi2sd(0, RAX); a_.movsd_store(RBP, frame.at(p).slot, 0); }
                    else a_.mov_mr(RBP, frame.at(p).slot, RAX);
                    break;
                default:
                    if (!gen_to(arg, NType::Float)) return false;
                    if (lt != NType::Float) return fail("float parameter with a non-float local");
                    a_.movsd_store(RBP, frame.at(p).slot, 0);
                break;
            }
        }
        for (size_t i = 0; i < callee.global_params.size(); i++) {
            const size_t p = callee.user_param_count + i;
            const std::string& global = callee.global_params[i];
            const NType lt = callee.local_types.at(global);
            if (callee.sig[p] == 'I') {
                get_int(global, RAX);
                if (lt == NType::Float) { a_.cvtsi2sd(0, RAX); a_.movsd_store(RBP, frame.at(global).slot, 0); }
                else a_.mov_mr(RBP, frame.at(global).slot, RAX);
            } else {
                get_float(global, 0);
                if (lt != NType::Float) return fail("float hidden module-global argument with a non-float local");
                a_.movsd_store(RBP, frame.at(global).slot, 0);
            }
        }
        {
            std::vector<std::string> zero;
            for (const auto& n : names) {
                if (n.compare(0, 4, "len_") == 0) continue;
                if (std::find(callee.param_names.begin(), callee.param_names.end(), n) != callee.param_names.end()) continue;
                if (callee.local_types.at(n) == NType::Array) return fail("array local in an inlined function");
                zero.push_back(n);
            }
            if (!zero.empty()) a_.mov_ri(RAX, 0);
            for (const auto& n : zero) a_.mov_mr(RBP, frame.at(n).slot, RAX);
        }
        FnInfo* saved_fn = c_.fn;
        std::unordered_map<std::string, Var> saved_vars = std::move(c_.vars);
        const int saved_exit = c_.inline_exit;
        std::vector<Loop> saved_loops = std::move(c_.loops);
        c_.outer_scopes.push_back(&saved_vars);
        c_.fn = &callee;
        c_.vars = std::move(frame);
        c_.inline_exit = a_.new_label();
        c_.loops.clear();
        const bool ok = gen_stmts(callee.decl->body);
        a_.mov_ri(RAX, 0);
        a_.xorpd(0, 0);
        a_.bind(c_.inline_exit);
        c_.vars = std::move(saved_vars);
        c_.outer_scopes.pop_back();
        c_.fn = saved_fn;
        c_.inline_exit = saved_exit;
        c_.loops = std::move(saved_loops);
        return ok;
    }

    int helper_for(const std::string& name) {
        static const std::unordered_map<std::string, int> m = {
            {"sin", x64::H_SIN}, {"cos", x64::H_COS}, {"tan", x64::H_TAN}, {"asin", x64::H_ASIN},
            {"acos", x64::H_ACOS}, {"atan", x64::H_ATAN}, {"sqrt", x64::H_SQRT}, {"exp", x64::H_EXP},
            {"log", x64::H_LOG}, {"log2", x64::H_LOG2}, {"log10", x64::H_LOG10}, {"floor", x64::H_FLOOR},
            {"ceil", x64::H_CEIL}, {"round", x64::H_ROUND}, {"abs", x64::H_FABS}, {"sign", x64::H_SIGN},
            {"tanh", x64::H_TANH}, {"sigmoid", x64::H_SIGMOID}, {"relu", x64::H_RELU},
            {"sigmoid_deriv", x64::H_SIGMOID_D}, {"relu_deriv", x64::H_RELU_D}, {"tanh_deriv", x64::H_TANH_D},
            {"atan2", x64::H_ATAN2}, {"pow", x64::H_POW}, {"clamp", x64::H_CLAMP}};
        auto it = m.find(name);
        return it == m.end() ? -1 : it->second;
    }

    void call_helper(int id) {
        std::vector<const Var*> spilled;
        auto collect = [&](const std::unordered_map<std::string, Var>& scope) {
            for (const auto& kv : scope) {
                const Var& v = kv.second;
                if (v.reg < 0) continue;
                if (v.is_float || int_reg_clobbered_by_c(v.reg)) spilled.push_back(&v);
            }
        };
        collect(c_.vars);
        for (const auto* scope : c_.outer_scopes) collect(*scope);
        std::sort(spilled.begin(), spilled.end(), [](const Var* a, const Var* b) { return a->slot < b->slot; });
        for (const Var* v : spilled) {
            if (v->is_float) a_.movsd_store(x64::RBP, v->slot, v->reg); else a_.mov_mr(x64::RBP, v->slot, v->reg);
        }
        bool padded;
        align_call_begin(padded);
        a_.call_got(id, 0);
        align_call_end(padded);
        for (const Var* v : spilled) {
            if (v->is_float) a_.movsd_load(v->reg, x64::RBP, v->slot); else a_.mov_rm(v->reg, x64::RBP, v->slot);
        }
    }

    bool gen_math(const ExprPtr& e, const std::string& name, const MathBuiltin& mb) {
        using namespace x64;
        if (mb.kind == MathKind::Promote2) {
            const NType ta = type_of(e->args[0]), tb = type_of(e->args[1]);
            if (ta == NType::Float || tb == NType::Float) {
                if (!gen_to(e->args[0], NType::Float)) return false;
                push_val(NType::Float);
                if (!gen_to(e->args[1], NType::Float)) return false;
                a_.movapd(1, 0);
                pop_val(NType::Float);
                call_helper(name == "max" ? H_FMAX : H_FMIN);
                return true;
            }
            if (!gen_operands(e->args[0], e->args[1], NType::Int)) return false;
            const int keep = a_.new_label();
            a_.alu_rr(0x39, RAX, RCX);
            a_.jcc(name == "max" ? CC_GE : CC_LE, keep);
            a_.mov_rr(RAX, RCX);
            a_.bind(keep);
            return true;
        }
        if (name == "sqrt" && e->args.size() == 1) {
            if (!gen_to(e->args[0], NType::Float)) return false;
            a_.sqrtsd(0, 0);
            return true;
        }
        if (name == "abs" && e->args.size() == 1) {
            if (!gen_to(e->args[0], NType::Float)) return false;
            a_.mov_ri(RAX, INT64_MAX);
            a_.movq_xr(1, RAX);
            a_.andpd(0, 1);
            return true;
        }
        const int id = helper_for(name);
        if (id < 0) return fail("math builtin '" + name + "' has no machine-code helper");
        const size_t n = e->args.size();
        for (size_t i = 0; i < n; i++) {
            if (!gen_to(e->args[i], NType::Float)) return false;
            push_val(NType::Float);
        }
        for (size_t k = n; k-- > 0;) {
            a_.pop_r(RAX); c_.depth -= 8;
            a_.movq_xr((int)k, RAX);
        }
        call_helper(id);
        return true;
    }

    bool gen_index_read(const ExprPtr& e) {
        using namespace x64;
        const std::string& name = e->object->name;
        if (!has_var("len_" + name)) return fail("index of a non-array");
        if (!gen_index_value(e->index)) return false;
        const int base = emit_bounds_and_base(name);
        a_.movsd_load_idx(0, base, RAX);
        return true;
    }

    bool gen_index_value(const ExprPtr& idx) {
        if (!gen_expr(idx)) return false;
        if (type_of(idx) == NType::Float) a_.cvttsd2si(x64::RAX, 0);
        return true;
    }

    int emit_bounds_and_base(const std::string& name) {
        using namespace x64;
        const int ok = a_.new_label();
        get_int("len_" + name, RDX);
        a_.alu_rr(0x39, RAX, RDX);
        a_.jcc(CC_B, ok);
        a_.alu_rr(0x85, RAX, RAX);
        a_.jcc(CC_NS, c_.err_idx);
        a_.alu_rr(0x01, RAX, RDX);
        a_.alu_rr(0x39, RAX, RDX);
        a_.jcc(CC_AE, c_.err_idx);
        a_.bind(ok);
        const Var& d = var(name);
        if (d.reg >= 0) return d.reg;
        a_.mov_rm(RDX, RBP, d.slot);
        return RDX;
    }

    static int invert_cc(int cc) {
        switch (cc) {
            case x64::CC_E: return x64::CC_NE;
            case x64::CC_NE: return x64::CC_E;
            case x64::CC_L: return x64::CC_GE;
            case x64::CC_GE: return x64::CC_L;
            case x64::CC_G: return x64::CC_LE;
            default: return x64::CC_G;
        }
    }
    static int int_cc(TokenType op) {
        switch (op) {
            case TokenType::LT: return x64::CC_L;
            case TokenType::GT: return x64::CC_G;
            case TokenType::LE: return x64::CC_LE;
            case TokenType::GE: return x64::CC_GE;
            case TokenType::EQ: return x64::CC_E;
            default: return x64::CC_NE;
        }
    }
    void branch_cc(int cc, int t, int f, int next) {
        if (next == t) { a_.jcc(invert_cc(cc), f); return; }
        a_.jcc(cc, t);
        if (next != f) a_.jmp(f);
    }
    void branch_float(TokenType op, int t, int f, int next) {
        using namespace x64;
        switch (op) {
            case TokenType::LT: case TokenType::LE: case TokenType::GT: case TokenType::GE: {
                const bool strict = op == TokenType::LT || op == TokenType::GT;
                const int true_cc = strict ? CC_A : CC_AE;
                const int false_cc = strict ? CC_BE : CC_B;
                if (next == t) { a_.jcc(false_cc, f); }
                else { a_.jcc(true_cc, t); if (next != f) a_.jmp(f); }
                return;
            }
            case TokenType::EQ:
                if (next == t) { a_.jcc(CC_P, f); a_.jcc(CC_NE, f); }
                else { a_.jcc(CC_P, f); a_.jcc(CC_E, t); if (next != f) a_.jmp(f); }
                return;
            default:
                if (next == t) { const int ok = a_.new_label(); a_.jcc(CC_P, ok); a_.jcc(CC_E, f); a_.bind(ok); }
                else { a_.jcc(CC_P, t); a_.jcc(CC_NE, t); if (next != f) a_.jmp(f); }
                return;
        }
    }

    bool gen_cond(const ExprPtr& e, int t, int f, int next = -1) {
        using namespace x64;
        if (e->kind == ExprKind::Binary && (e->op == TokenType::AND || e->op == TokenType::OR)) {
            const int mid = a_.new_label();
            if (e->op == TokenType::AND) { if (!gen_cond(e->left, mid, f, mid)) return false; }
            else { if (!gen_cond(e->left, t, mid, mid)) return false; }
            a_.bind(mid);
            return gen_cond(e->right, t, f, next);
        }
        if (e->kind == ExprKind::Unary && e->op != TokenType::MINUS) return gen_cond(e->operand, f, t, next);
        if (e->kind == ExprKind::Binary && (e->op == TokenType::LT || e->op == TokenType::GT || e->op == TokenType::LE ||
                                            e->op == TokenType::GE || e->op == TokenType::EQ || e->op == TokenType::NEQ)) {
            const bool fl = type_of(e->left) == NType::Float || type_of(e->right) == NType::Float;
            if (!fl && e->right->kind == ExprKind::Int && e->right->int_val >= INT32_MIN && e->right->int_val <= INT32_MAX) {
                if (!gen_to(e->left, NType::Int)) return false;
                if (e->right->int_val == 0) a_.alu_rr(0x85, RAX, RAX);
                else a_.alu_ri(7, RAX, (int32_t)e->right->int_val);
                branch_cc(int_cc(e->op), t, f, next);
                return true;
            }
            if (!gen_operands(e->left, e->right, fl ? NType::Float : NType::Int)) return false;
            if (!fl) {
                a_.alu_rr(0x39, RAX, RCX);
                branch_cc(int_cc(e->op), t, f, next);
                return true;
            }
            switch (e->op) {
                case TokenType::LT: case TokenType::LE: a_.ucomisd(1, 0); break;
                default: a_.ucomisd(0, 1); break;
            }
            branch_float(e->op, t, f, next);
            return true;
        }
        if (!gen_expr(e)) return false;
        if (type_of(e) == NType::Float) {
            a_.xorpd(1, 1);
            a_.ucomisd(0, 1);
            branch_float(TokenType::NEQ, t, f, next);
        } else {
            a_.alu_rr(0x85, RAX, RAX);
            branch_cc(CC_NE, t, f, next);
        }
        return true;
    }

    bool store_scalar(const std::string& name, NType value_type) {
        const NType tt = local_type(name);
        if (tt == NType::Array) return fail("assignment to an array variable");
        if (tt == NType::Float) {
            if (value_type == NType::Int) a_.cvtsi2sd(0, x64::RAX);
            put_float(name, 0);
        } else {
            if (value_type == NType::Float) return fail("float stored into an int variable");
            put_int(name, x64::RAX);
        }
        return true;
    }

    bool gen_stmts(const std::vector<StmtPtr>& body) {
        for (const auto& s : body) if (!gen_stmt(s)) return false;
        return true;
    }

    bool gen_array_store(const ExprPtr& target, const ExprPtr& value, int compound  ) {
        using namespace x64;
        const std::string& name = target->object->name;
        if (!has_var("len_" + name)) return fail("index of a non-array");
        if (!gen_index_value(target->index)) return false;
        if (is_simple(value)) {
            a_.mov_rr(RCX, RAX);
            if (!load_simple(value, NType::Float, RAX, 0)) return false;
            a_.mov_rr(RAX, RCX);
        } else {
            push_val(NType::Int);
            if (!gen_to(value, NType::Float)) return false;
            a_.pop_r(RAX); c_.depth -= 8;
        }
        const int base = emit_bounds_and_base(name);
        if (compound == 0) {
            a_.movsd_store_idx(base, RAX, 0);
        } else {
            a_.movsd_load_idx(1, base, RAX);
            if (compound == 1) a_.addsd(1, 0); else a_.subsd(1, 0);
            a_.movsd_store_idx(base, RAX, 1);
        }
        return true;
    }

    struct VecPlan {
        std::string target; int target_off = 0;
        std::map<std::string, int> reads;
        ExprPtr value;
    };
    static bool vec_enabled() {
        static const bool off = std::getenv("CSY_NO_VECTOR_LOOPS") != nullptr;
        return !off;
    }
    static bool vec_index(const ExprPtr& idx, const std::string& loop_var, int& off) {
        if (!idx) return false;
        if (idx->kind == ExprKind::Ident) { off = 0; return idx->name == loop_var; }
        if (idx->kind != ExprKind::Binary || idx->op != TokenType::PLUS) return false;
        const ExprPtr& l = idx->left; const ExprPtr& r = idx->right;
        const ExprPtr* var = nullptr; const ExprPtr* lit = nullptr;
        if (l && r && l->kind == ExprKind::Ident && r->kind == ExprKind::Int) { var = &l; lit = &r; }
        else if (l && r && r->kind == ExprKind::Ident && l->kind == ExprKind::Int) { var = &r; lit = &l; }
        if (!var || (*var)->name != loop_var || (*lit)->int_val < 0 || (*lit)->int_val > 4096) return false;
        off = (int)(*lit)->int_val;
        return true;
    }
    int vec_expr_ok(const ExprPtr& e, VecPlan& plan, const std::string& loop_var, bool int_leaf_ok, int& nodes) {
        if (!e || ++nodes > 48) return 0;
        switch (e->kind) {
            case ExprKind::Float: return 1;
            case ExprKind::Int: return int_leaf_ok ? 1 : 0;
            case ExprKind::Index: {
                if (!e->object || e->object->kind != ExprKind::Ident) return 0;
                const std::string& name = e->object->name;
                if (name == plan.target || !has_var("len_" + name) || !has_var(name)) return 0;
                int off = 0;
                if (!vec_index(e->index, loop_var, off)) return 0;
                auto it = plan.reads.find(name);
                if (it == plan.reads.end()) plan.reads[name] = off; else it->second = std::max(it->second, off);
                return 1;
            }
            case ExprKind::Binary: {
                if (e->op != TokenType::PLUS && e->op != TokenType::MINUS && e->op != TokenType::STAR) return 0;
                if (type_of(e) != NType::Float) return 0;
                const int l = vec_expr_ok(e->left, plan, loop_var, true, nodes);
                const int r = vec_expr_ok(e->right, plan, loop_var, true, nodes);
                if (!l || !r) return 0;
                return std::max(l, r + 1);
            }
            default: return 0;
        }
    }
    bool vec_plan(const StmtPtr& s, VecPlan& plan) {
        if (!vec_enabled() || s->body.size() != 1 || !s->body[0]) return false;
        const Stmt& st = *s->body[0];
        ExprPtr assign;
        if (st.kind == StmtKind::VarDecl) assign = st.expr;
        else if (st.kind == StmtKind::ExprStmt && st.expr && st.expr->kind == ExprKind::Assign) assign = st.expr;
        if (!assign || !assign->target || assign->target->kind != ExprKind::Index || !assign->value) return false;
        const ExprPtr& t = assign->target;
        if (!t->object || t->object->kind != ExprKind::Ident) return false;
        plan.target = t->object->name;
        if (!has_var(plan.target) || !has_var("len_" + plan.target)) return false;
        if (!vec_index(t->index, s->loop_var, plan.target_off)) return false;
        if (!has_var(s->loop_var) || local_type(s->loop_var) != NType::Int || has_dynamic_tag(*c_.fn, s->loop_var)) return false;
        if (type_of(assign->value) != NType::Float) return false;
        int nodes = 0;
        const int need = vec_expr_ok(assign->value, plan, s->loop_var, false, nodes);
        if (need == 0 || need > 8 || plan.reads.empty() || plan.reads.size() > 6) return false;
        plan.value = assign->value;
        return true;
    }
    void vec_gen(const ExprPtr& e, int k) {
        using namespace x64;
        switch (e->kind) {
            case ExprKind::Float:
            case ExprKind::Int: {
                const double v = e->kind == ExprKind::Float ? e->float_val : (double)e->int_val;
                a_.mov_ri(RCX, (int64_t)bits(v));
                a_.movq_xr(k, RCX);
                a_.unpcklpd(k, k);
                return;
            }
            case ExprKind::Index: {
                int off = 0;
                vec_index(e->index, vec_loop_var_, off);
                get_int(e->object->name, RCX);
                a_.movupd_load_idx(k, RCX, RAX, off * 8);
                return;
            }
            default: {
                vec_gen(e->left, k);
                vec_gen(e->right, k + 1);
                if (e->op == TokenType::PLUS) a_.addpd(k, k + 1);
                else if (e->op == TokenType::MINUS) a_.subpd(k, k + 1);
                else a_.mulpd(k, k + 1);
                return;
            }
        }
    }
    std::string vec_loop_var_;
    void vec_emit(const VecPlan& p, const StmtPtr& s, const std::string& cn, const std::string& bn, int top) {
        using namespace x64;
        vec_loop_var_ = s->loop_var;
        dbg("own x86-64 backend: element-wise loop at line " + std::to_string(s->tok.line) + " has a vector fast path");
        get_int(bn, RAX);
        a_.alu_ri(7, RAX, 4);
        a_.jcc(CC_L, top);
        std::vector<std::pair<std::string, int>> arrays(p.reads.begin(), p.reads.end());
        arrays.emplace_back(p.target, p.target_off);
        for (const auto& [name, cmax] : arrays) {
            get_int("len_" + name, RCX);
            a_.alu_ri(5, RCX, cmax);
            a_.alu_rr(0x39, RCX, RAX);
            a_.jcc(CC_L, top);
        }
        for (const auto& [name, cmax] : p.reads) {
            (void)cmax;
            const int ok = a_.new_label();
            get_int(name, RAX);
            get_int("len_" + name, RCX);
            a_.shift_ri(4, RCX, 3);
            a_.alu_rr(0x01, RCX, RAX);
            get_int(p.target, RDX);
            a_.alu_rr(0x39, RCX, RDX);
            a_.jcc(CC_BE, ok);
            get_int("len_" + p.target, RCX);
            a_.shift_ri(4, RCX, 3);
            a_.alu_rr(0x01, RCX, RDX);
            a_.alu_rr(0x39, RCX, RAX);
            a_.jcc(CC_A, top);
            a_.bind(ok);
        }
        get_int(bn, RDX);
        a_.dec_r(RDX);
        a_.mov_ri(RAX, 0);
        const int vtop = a_.new_label(), vdone = a_.new_label();
        a_.bind(vtop);
        a_.alu_rr(0x39, RAX, RDX);
        a_.jcc(CC_GE, vdone);
        vec_gen(p.value, 0);
        get_int(p.target, RCX);
        a_.movupd_store_idx(RCX, RAX, p.target_off * 8, 0);
        a_.alu_ri(0, RAX, 2);
        a_.jmp(vtop);
        a_.bind(vdone);
        put_int(cn, RAX);
        a_.alu_ri(5, RAX, 1);
        store_scalar(s->loop_var, NType::Int);
    }

    bool gen_stmt(const StmtPtr& s) {
        using namespace x64;
        if (!s) return true;
        switch (s->kind) {
            case StmtKind::VarDecl: {
                const ExprPtr& target = s->expr->target;
                if (target->kind == ExprKind::Index) return gen_array_store(target, s->expr->value, 0);
                if (!gen_expr(s->expr->value)) return false;
                if (!store_scalar(target->name, type_of(s->expr->value))) return false;
                if (has_dynamic_tag(*c_.fn, target->name)) {
                    if (!gen_expr_tag(s->expr->value)) return false;
                    put_tag(target->name);
                }
                return true;
            }
            case StmtKind::ExprStmt: {
                if (s->expr->kind == ExprKind::Assign) return gen_array_store(s->expr->target, s->expr->value, 0);
                if (s->expr->target->kind == ExprKind::Index)
                    return gen_array_store(s->expr->target, s->expr->value, s->expr->op == TokenType::MINUS_EQ ? 2 : 1);
                auto tmp = std::make_shared<Expr>();
                tmp->kind = ExprKind::Binary;
                tmp->op = s->expr->op == TokenType::PLUS_EQ ? TokenType::PLUS : TokenType::MINUS;
                tmp->left = s->expr->target;
                tmp->right = s->expr->value;
                if (local_type(s->expr->target->name) == NType::Int && type_of(s->expr->value) != NType::Int)
                    return fail("float added into an int variable");
                if (!gen_expr(tmp)) return false;
                if (!store_scalar(s->expr->target->name, type_of(tmp))) return false;
                if (has_dynamic_tag(*c_.fn, s->expr->target->name)) {
                    if (!gen_expr_tag(tmp)) return false;
                    put_tag(s->expr->target->name);
                }
                return true;
            }
            case StmtKind::If: {
                const int end = a_.new_label();
                int next = a_.new_label(), then_l = a_.new_label();
                if (!gen_cond(s->cond, then_l, next, then_l)) return false;
                a_.bind(then_l);
                if (!gen_stmts(s->then_body)) return false;
                if (!s->elifs.empty() || !s->else_body.empty()) a_.jmp(end);
                for (const auto& el : s->elifs) {
                    a_.bind(next);
                    next = a_.new_label();
                    const int body_l = a_.new_label();
                    if (!gen_cond(el.first, body_l, next, body_l)) return false;
                    a_.bind(body_l);
                    if (!gen_stmts(el.second)) return false;
                    if (&el != &s->elifs.back() || !s->else_body.empty()) a_.jmp(end);
                }
                a_.bind(next);
                if (!s->else_body.empty() && !gen_stmts(s->else_body)) return false;
                a_.bind(end);
                return true;
            }
            case StmtKind::While: {
                const int top = a_.new_label(), body = a_.new_label(), end = a_.new_label();
                a_.bind(top);
                if (!gen_cond(s->cond, body, end, body)) return false;
                a_.bind(body);
                c_.loops.push_back({top, end});
                loop_enter();
                const bool ok = gen_stmts(s->body);
                c_.loops.pop_back();
                if (!ok) return false;
                a_.jmp(top);
                a_.bind(end);
                loop_leave(false);
                return true;
            }
            case StmtKind::For: {
                const int id = c_.loop_ids.at(s.get());
                const std::string cn = "@c" + std::to_string(id), bn = "@b" + std::to_string(id);
                const ExprPtr& bexpr = s->iterable->args[0];
                if (!gen_expr(bexpr)) return false;
                if (type_of(bexpr) == NType::Float) call_helper(H_BOUND_F_ID());
                put_int(bn, RAX);
                a_.mov_ri(RAX, 0);
                put_int(cn, RAX);
                if (!has_var(s->loop_var) || local_type(s->loop_var) == NType::Array) return fail("loop variable");
                const int top = a_.new_label(), cont = a_.new_label(), end = a_.new_label();
                bool is_vec = false;
                {
                    VecPlan plan;
                    if (c_.inline_exit < 0 && vec_plan(s, plan)) { vec_emit(plan, s, cn, bn, top); is_vec = true; }
                }
                const bool profitable = is_vec && !(bexpr->kind == ExprKind::Int && bexpr->int_val < 256);
                if (profitable && cur_stat_) cur_stat_->vector_loops++;
                loop_enter();
                a_.bind(top);
                get_int(cn, RAX);
                const Var& bv = var(bn);
                if (bv.reg >= 0) a_.alu_rr(0x39, RAX, bv.reg); else a_.cmp_rm(RAX, RBP, bv.slot);
                a_.jcc(CC_GE, end);
                if (!store_scalar(s->loop_var, NType::Int)) return false;
                if (has_dynamic_tag(*c_.fn, s->loop_var)) {
                    a_.mov_ri(RAX, 0);
                    put_tag(s->loop_var);
                }
                c_.loops.push_back({cont, end});
                const bool ok = gen_stmts(s->body);
                c_.loops.pop_back();
                if (!ok) return false;
                a_.bind(cont);
                const Var& cv = var(cn);
                if (cv.reg >= 0) a_.inc_r(cv.reg); else a_.inc_m(RBP, cv.slot);
                a_.jmp(top);
                a_.bind(end);
                loop_leave(profitable);
                return true;
            }
            case StmtKind::Return: {
                const NType t = type_of(s->ret_value);
                if (!gen_expr(s->ret_value)) return false;
                if (c_.fn->return_type == NType::Float) {
                    if (t == NType::Int) a_.cvtsi2sd(0, RAX);
                } else if (t == NType::Float) {
                    return fail("float returned from an int-returning variant");
                }
                if (returns_maybe_int(*c_.fn)) {
                    if (!gen_expr_tag(s->ret_value)) return false;
                    a_.mov_rr(RDX, RAX);
                    if (c_.inline_exit >= 0) { a_.jmp(c_.inline_exit); return true; }
                } else {
                    if (c_.inline_exit >= 0) { a_.jmp(c_.inline_exit); return true; }
                    {
                        const int inner = a_.new_label();
                        a_.alu_rr(0x85, R13, R13);
                        a_.jcc(CC_NE, inner);
                        a_.mov_m32_imm(R14, 0, t == NType::Float ? 1 : 0);
                        a_.bind(inner);
                    }
                }
                a_.jmp(c_.epilogue);
                return true;
            }
            case StmtKind::Break:
                if (c_.loops.empty()) return fail("break outside a loop");
                a_.jmp(c_.loops.back().brk);
                return true;
            case StmtKind::Continue:
                if (c_.loops.empty()) return fail("continue outside a loop");
                a_.jmp(c_.loops.back().cont);
                return true;
            default:
                return true;
        }
    }
    static int H_BOUND_F_ID() { return x64::H_BOUND_F; }

    using Weights = std::unordered_map<std::string, long long>;
    bool is_array_name(const std::string& n) {
        auto it = c_.fn->local_types.find(n);
        return it != c_.fn->local_types.end() && it->second == NType::Array;
    }
    void weigh_expr(const ExprPtr& e, long long m, Weights& w) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Ident:
                if (c_.fn->local_types.count(e->name)) w[e->name] += m;
                return;
            case ExprKind::Index:
                if (e->object && e->object->kind == ExprKind::Ident) {
                    w[e->object->name] += m;
                    w["len_" + e->object->name] += m;
                }
                weigh_expr(e->index, m, w);
                return;
            case ExprKind::Call:
                for (const auto& arg : e->args) {
                    if (arg && arg->kind == ExprKind::Ident && is_array_name(arg->name)) { w[arg->name] += m; w["len_" + arg->name] += m; }
                    else weigh_expr(arg, m, w);
                }
                return;
            default:
                weigh_expr(e->left, m, w); weigh_expr(e->right, m, w); weigh_expr(e->operand, m, w);
                weigh_expr(e->target, m, w); weigh_expr(e->value, m, w);
                return;
        }
    }
    void weigh_stmts(const std::vector<StmtPtr>& body, long long m, Weights& w, int& next_loop) {
        const long long inner_factor = 8;
        for (const auto& s : body) {
            if (!s) continue;
            const long long inner = m >= (1 << 20) ? m : m * inner_factor;
            switch (s->kind) {
                case StmtKind::VarDecl:
                case StmtKind::ExprStmt:
                    weigh_expr(s->expr->target, m, w);
                    weigh_expr(s->expr->value, m, w);
                    break;
                case StmtKind::If:
                    weigh_expr(s->cond, m, w);
                    weigh_stmts(s->then_body, m, w, next_loop);
                    for (const auto& el : s->elifs) { weigh_expr(el.first, m, w); weigh_stmts(el.second, m, w, next_loop); }
                    weigh_stmts(s->else_body, m, w, next_loop);
                    break;
                case StmtKind::While:
                    weigh_expr(s->cond, inner, w);
                    weigh_stmts(s->body, inner, w, next_loop);
                    break;
                case StmtKind::For: {
                    const int id = next_loop++;
                    c_.loop_ids[s.get()] = id;
                    w["@c" + std::to_string(id)] += 3 * inner;
                    w["@b" + std::to_string(id)] += inner;
                    w[s->loop_var] += inner;
                    weigh_expr(s->iterable->args[0], m, w);
                    weigh_stmts(s->body, inner, w, next_loop);
                    break;
                }
                case StmtKind::Return:
                    weigh_expr(s->ret_value, m, w);
                    break;
                default:
                    break;
            }
        }
    }

    bool emit_function(FnInfo& fn, const std::string& key) {
        using namespace x64;
        c_ = Ctx();
        c_.fn = &fn;
        cur_stat_ = &loop_stats_[key];
        loop_has_vec_.clear();

        Weights w;
        int loops = 0;
        weigh_stmts(fn.decl->body, 1, w, loops);
        std::vector<std::string> names;
        for (const auto& kv : fn.local_types) {
            const bool is_param = std::find(fn.param_names.begin(), fn.param_names.end(), kv.first) != fn.param_names.end();
            if (kv.second == NType::Array) {
                if (!is_param) return fail("array local");
                names.push_back(kv.first);
                names.push_back("len_" + kv.first);
            } else {
                names.push_back(kv.first);
            }
        }
        for (int id = 0; id < loops; id++) { names.push_back("@c" + std::to_string(id)); names.push_back("@b" + std::to_string(id)); }
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            Var v;
            auto lt = fn.local_types.find(n);
            v.is_float = lt != fn.local_types.end() && lt->second == NType::Float;
            v.slot = new_slot();
            c_.vars[n] = v;
        }
        for (const auto& name : fn.param_names) {
            if (has_dynamic_tag(fn, name)) c_.tag_slots[name] = new_slot();
        }
        for (const auto& [name, type] : fn.local_types) {
            if (std::find(fn.param_names.begin(), fn.param_names.end(), name) == fn.param_names.end() &&
                type != NType::Array && has_dynamic_tag(fn, name)) c_.tag_slots[name] = new_slot();
        }
        all_stmt_exprs(fn.decl->body, [&](const ExprPtr& e) {
            if (e && e->kind == ExprKind::Call && e->callee && e->callee->kind == ExprKind::Ident &&
                !math_whitelist().count(e->callee->name)) {
                std::string callee;
                if (callee_key(fns_, fn, e, callee) && returns_maybe_int(fns_.at(callee)))
                    c_.call_tag_slots[e.get()] = new_slot();
            }
            return true;
        });
        {
            std::vector<std::string> ranked = names;
            std::stable_sort(ranked.begin(), ranked.end(), [&](const std::string& a, const std::string& b) {
                const long long wa = w.count(a) ? w[a] : 0, wb = w.count(b) ? w[b] : 0;
                return wa > wb;
            });
            int next_int = 0, next_float = 8;
            for (const auto& n : ranked) {
                const long long weight = w.count(n) ? w[n] : 0;
                if (weight < 6) continue;
                Var& v = c_.vars[n];
                if (v.is_float) { if (next_float < 16) v.reg = next_float++; }
                else if (next_int < 8) v.reg = kIntPool[next_int++];
            }
            for (const auto& n : names) {
                const Var& v = c_.vars[n];
                if (v.reg >= 0) { const int e = enc_reg(v.is_float, v.reg); c_.used_regs.push_back(e); c_.save_slot[e] = new_slot(); }
            }
        }

        a_.bind(fn_label_.at(key));
        a_.push_r(RBP);
        a_.mov_rr(RBP, RSP);
        a_.b(0x48); a_.b(0x81); a_.b(0xEC);
        const size_t frame_patch = a_.pos();
        a_.d32(0);
        c_.ret_zero = a_.new_label();
        c_.err_div = a_.new_label();
        c_.err_ovf = a_.new_label();
        c_.err_idx = a_.new_label();
        c_.epilogue = a_.new_label();

        for (int e : c_.used_regs) {
            if (e & 256) a_.movsd_store(RBP, c_.save_slot[e], e & 255);
            else a_.mov_mr(RBP, c_.save_slot[e], e & 255);
        }

        const int depth_ok = a_.new_label();
        a_.alu_ri(7, R13, (int32_t)kMaxNativeDepth);
        a_.jcc(CC_LE, depth_ok);
        a_.mov_m32_imm(R12, 0, 2);
        a_.jmp(c_.ret_zero);
        a_.bind(depth_ok);

        int param_slots = 0;
        for (size_t i = 0; i < fn.param_names.size(); i++)
            param_slots += fn.sig[i] == 'A' || fn.sig[i] == 'M' ? 2 : 1;
        int pos = 0;
        for (size_t i = 0; i < fn.param_names.size(); i++) {
            const std::string& p = fn.param_names[i];
            const NType lt = fn.local_types.at(p);
            if (fn.sig[i] == 'A') {
                if (lt != NType::Array) return fail("array parameter with a scalar local");
                const int data_src = 16 + 8 * (param_slots - 1 - pos), len_src = 16 + 8 * (param_slots - 2 - pos);
                for (const auto& part : {std::make_pair(p, data_src), std::make_pair("len_" + p, len_src)}) {
                    Var& v = var(part.first);
                    if (v.reg >= 0) a_.mov_rm(v.reg, RBP, part.second);
                    else v.slot = part.second;
                }
                pos += 2;
            } else {
                const int src = 16 + 8 * (param_slots - 1 - pos);
                Var& pv = var(p);
                if (fn.sig[i] == 'I') {
                    if (lt == NType::Float) { a_.mov_rm(RAX, RBP, src); a_.cvtsi2sd(0, RAX); put_float(p, 0); }
                    else if (lt == NType::Int) {
                        if (pv.reg >= 0) a_.mov_rm(pv.reg, RBP, src); else pv.slot = src;
                    }
                    else return fail("int parameter with an array local");
                    if (has_dynamic_tag(fn, p)) {
                        a_.mov_ri(RAX, 0);
                        put_tag(p, RAX);
                    }
                } else {
                    if (lt != NType::Float) return fail("float parameter with a non-float local");
                    if (pv.reg >= 0) a_.movsd_load(pv.reg, RBP, src); else pv.slot = src;
                    if (fn.sig[i] == 'M') {
                        const int tag_src = 16 + 8 * (param_slots - 2 - pos);
                        a_.mov_rm(RAX, RBP, tag_src);
                        put_tag(p, RAX);
                    } else if (has_dynamic_tag(fn, p)) {
                        a_.mov_ri(RAX, fn.sig[i] == 'F' ? 1 : 0);
                        put_tag(p, RAX);
                    }
                }
                pos += fn.sig[i] == 'M' ? 2 : 1;
            }
        }
        for (const auto& n : names) {
            if (n[0] == '@' || n.compare(0, 4, "len_") == 0) continue;
            if (std::find(fn.param_names.begin(), fn.param_names.end(), n) != fn.param_names.end()) continue;
            if (var(n).is_float) { a_.xorpd(0, 0); put_float(n, 0); }
            else { a_.mov_ri(RAX, 0); put_int(n, RAX); }
        }
        for (const auto& [name, slot] : c_.tag_slots) {
            if (std::find(fn.param_names.begin(), fn.param_names.end(), name) != fn.param_names.end()) continue;
            (void)slot;
            a_.mov_ri(RAX, 0);
            put_tag(name, RAX);
        }
        if (!gen_stmts(fn.decl->body)) return false;
        a_.jmp(c_.ret_zero);

        a_.bind(c_.err_div); emit_set_err(1);
        a_.bind(c_.err_idx); emit_set_err(3);
        a_.bind(c_.err_ovf); emit_set_err(4);
        a_.bind(c_.ret_zero);
        a_.mov_ri(RAX, 0);
        a_.xorpd(0, 0);
        a_.bind(c_.epilogue);
        for (int e : c_.used_regs) {
            if (e & 256) a_.movsd_load(e & 255, RBP, c_.save_slot[e]);
            else a_.mov_rm(e & 255, RBP, c_.save_slot[e]);
        }
        a_.leave();
        a_.ret();

        int frame = 8 * c_.next_slot;
        frame = (frame + 15) & ~15;
        a_.patch32(frame_patch, frame);
        return true;
    }
    void emit_set_err(int code) {
        using namespace x64;
        const int skip = a_.new_label();
        a_.cmp_m32_imm8(R12, 0, 0);
        a_.jcc(CC_NE, skip);
        a_.mov_m32_imm(R12, 0, code);
        a_.bind(skip);
        a_.jmp(c_.ret_zero);
    }

    void emit_entry(const FnInfo& fn, const std::string& key) {
        using namespace x64;
        a_.push_r(RBP);
        a_.mov_rr(RBP, RSP);
        a_.push_r(R12); a_.push_r(R13); a_.push_r(R14);
        a_.push_r(RCX);
        a_.mov_rr(R12, RSI);
        a_.mov_ri(R13, 0);
        a_.mov_rr(R14, RDX);
        const std::vector<size_t> guards = entry_guard_params(fn);
        int guard_fail = -1;
        if (!guards.empty()) {
            guard_fail = a_.new_label();
            for (size_t gi : guards) {
                a_.mov_rm(RAX, RDI, (int)(gi * sizeof(NativeArg) + offsetof(NativeArg, ival)));
                a_.mov_ri(RCX, (int64_t(1) << 53) - 1);
                a_.alu_rr(0x01, RAX, RCX);
                a_.mov_ri(RCX, (int64_t(1) << 54) - 2);
                a_.alu_rr(0x39, RAX, RCX);
                a_.jcc(CC_A, guard_fail);
            }
        }
        int nslots = 0;
        for (size_t i = 0; i < fn.param_names.size(); i++)
            nslots += fn.sig[i] == 'A' || fn.sig[i] == 'M' ? 2 : 1;
        const int pad = (nslots * 8) % 16 ? 8 : 0;
        if (pad) a_.alu_ri(5, RSP, pad);
        for (size_t i = 0; i < fn.param_names.size(); i++) {
            const int base = (int)(i * sizeof(NativeArg));
            switch (fn.sig[i]) {
                case 'A':
                    a_.push_m(RDI, base + (int)offsetof(NativeArg, data));
                    a_.push_m(RDI, base + (int)offsetof(NativeArg, length));
                    break;
                case 'I': a_.push_m(RDI, base + (int)offsetof(NativeArg, ival)); break;
                default: a_.push_m(RDI, base + (int)offsetof(NativeArg, number)); break;
            }
        }
        a_.call(fn_label_.at(key));
        a_.alu_ri(0, RSP, nslots * 8 + pad);
        a_.pop_r(RCX);
        if (fn.return_type == NType::Float) a_.movsd_store(RCX, 0, 0); else a_.mov_mr(RCX, 0, RAX);
        if (returns_maybe_int(fn)) a_.mov_m32r(R14, 0, RDX);
        a_.pop_r(R14); a_.pop_r(R13); a_.pop_r(R12);
        a_.pop_r(RBP);
        a_.ret();
        if (guard_fail >= 0) {
            a_.bind(guard_fail);
            a_.mov_m32_imm(R12, 0, 5);
            a_.pop_r(RCX);
            a_.pop_r(R14); a_.pop_r(R13); a_.pop_r(R12);
            a_.pop_r(RBP);
            a_.ret();
        }
    }
};


static void x64_put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xFF)); }

std::string x64_serialize(const x64::Image& img) {
    std::string s = "CSYX64\x02";
    x64_put32(s, (uint32_t)img.entries.size());
    for (const auto& e : img.entries) {
        x64_put32(s, e.offset);
        s.push_back(e.ret_float ? 1 : 0);
        x64_put32(s, (uint32_t)e.name.size()); s += e.name;
        x64_put32(s, (uint32_t)e.sig.size()); s += e.sig;
        x64_put32(s, e.user_nparams);
        x64_put32(s, (uint32_t)e.global_names.size());
        for (const auto& name : e.global_names) {
            x64_put32(s, (uint32_t)name.size()); s += name;
        }
    }
    x64_put32(s, (uint32_t)img.blob.size());
    s += img.blob;
    return s;
}

bool x64_parse(const std::string& s, x64::Image& img) {
    size_t p = 0;
    auto get32 = [&](uint32_t& v) {
        if (s.size() - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t)(unsigned char)s[p + i] << (8 * i);
        p += 4;
        return true;
    };
    if (s.size() < 7 || s.compare(0, 6, "CSYX64") != 0 ||
        (s[6] != 1 && s[6] != 2)) return false;
    const uint8_t version = static_cast<uint8_t>(s[6]);
    p = 7;
    uint32_t n = 0;
    if (!get32(n) || n > 100000) return false;
    for (uint32_t i = 0; i < n; i++) {
        x64::Entry e;
        uint32_t len = 0;
        if (!get32(e.offset) || s.size() - p < 1) return false;
        e.ret_float = s[p++] != 0;
        if (!get32(len) || len > 4096 || s.size() - p < len) return false;
        e.name.assign(s, p, len); p += len;
        if (!get32(len) || len > 64 || s.size() - p < len) return false;
        e.sig.assign(s, p, len); p += len;
        if (e.sig.size() > kMaxNativeParams) return false;
        if (version >= 2) {
            uint32_t users = 0, globals = 0;
            if (!get32(users) || users > e.sig.size() || !get32(globals) ||
                globals > kMaxNativeParams || users + globals != e.sig.size()) return false;
            e.user_nparams = static_cast<uint8_t>(users);
            for (uint32_t g = 0; g < globals; g++) {
                if (!get32(len) || len > 4096 || s.size() - p < len) return false;
                e.global_names.emplace_back(s, p, len); p += len;
            }
        } else {
            e.user_nparams = static_cast<uint8_t>(e.sig.size());
        }
        img.entries.push_back(std::move(e));
    }
    uint32_t blob = 0;
    if (!get32(blob) || s.size() - p != blob) return false;
    img.blob.assign(s, p, blob);
    for (const auto& e : img.entries)
        if (e.offset < (size_t)x64::H_COUNT * 8 || e.offset >= img.blob.size()) return false;
    return img.blob.size() > (size_t)x64::H_COUNT * 8;
}

bool x64_load(const x64::Image& img, IRModule& ir, int& loaded, std::string& why, bool only_missing = false) {
    std::unordered_map<std::string, size_t> ir_index;
    for (size_t i = 0; i < ir.functions.size(); i++) ir_index.emplace(ir.functions[i].name, i);
    auto covered = [&](const x64::Entry& e) {
        auto it = ir_index.find(e.name);
        if (it == ir_index.end()) return true;
        const auto code = x64_sig_code(e.sig);
        for (const auto& v : ir.functions[it->second].native_variants) if (v.sig == code) return true;
        return false;
    };
    if (only_missing) {
        bool any = false;
        for (const auto& e : img.entries) if (!covered(e)) { any = true; break; }
        if (!any) { loaded = 0; return true; }
    }
    const size_t len = (img.blob.size() + 4095) & ~size_t(4095);
    void* mem = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { why = "cannot map memory for machine code"; return false; }
    std::memcpy(mem, img.blob.data(), img.blob.size());
    void** table = static_cast<void**>(mem);
    for (int i = 0; i < x64::H_COUNT; i++) table[i] = x64::helper_address(i);
    if (::mprotect(mem, len, PROT_READ | PROT_EXEC) != 0) {
        ::munmap(mem, len);
        why = "cannot make machine code executable";
        return false;
    }
    loaded = 0;
    for (const auto& e : img.entries) {
        auto it = ir_index.find(e.name);
        if (it == ir_index.end()) continue;
        if (only_missing && covered(e)) continue;
        if (e.user_nparams + e.global_names.size() != e.sig.size()) continue;
        NativeVariant v;
        v.entry = reinterpret_cast<NativeEntry>(static_cast<char*>(mem) + e.offset);
        v.sig = x64_sig_code(e.sig);
        v.nparams = e.user_nparams;
        bool globals_found = true;
        for (const auto& name : e.global_names) {
            auto gi = ir.global_index.find(name);
            if (gi == ir.global_index.end()) { globals_found = false; break; }
            v.global_indices.push_back(gi->second);
        }
        if (!globals_found) continue;
        v.ret_float = e.ret_float;
        ir.functions[it->second].native_variants.push_back(v);
        loaded++;
    }
    return true;
}

#endif

