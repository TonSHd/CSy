#include "ai_ops.hpp"
#include "vm.hpp"
#include "value.hpp"
#include "resource.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace csy {

//runtime threading for the ai kernels
static std::atomic<unsigned> g_run_threads{0};
static constexpr std::size_t kParallelMinWork = std::size_t(1) << 21;

void set_ai_run_threads(unsigned threads) { g_run_threads.store(threads, std::memory_order_relaxed); }

static unsigned ai_threads() {
    unsigned n = g_run_threads.load(std::memory_order_relaxed);
    if (n == 0) {
        if (const char* env = std::getenv("CSY_RUN_THREADS")) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(env, &end, 10);
            if (end != env && *end == '\0' && v <= 256) n = static_cast<unsigned>(v);
        }
    }
    if (n == 0) n = res::hardware_threads();
    return n < 1 ? 1 : n;
}

template <class F>
static void run_blocks(std::size_t blocks, std::size_t work, F&& fn) {
    if (blocks < 2 || work < kParallelMinWork) {
        for (std::size_t b = 0; b < blocks; ++b) fn(b);
        return;
    }
    const unsigned threads = ai_threads();
    if (threads <= 1) {
        for (std::size_t b = 0; b < blocks; ++b) fn(b);
        return;
    }
    res::parallel_for(blocks, threads, [&](std::size_t b) { fn(b); });
}

static void matmul_row_block(const double* __restrict__ a, const double* __restrict__ b,
                             double* __restrict__ c, std::size_t i0, std::size_t i1,
                             std::size_t inner, std::size_t cols) {
    constexpr std::size_t BP = 64, BJ = 256;
    for (std::size_t p0 = 0; p0 < inner; p0 += BP) {
        const std::size_t p1 = std::min(p0 + BP, inner);
        for (std::size_t j0 = 0; j0 < cols; j0 += BJ) {
            const std::size_t j1 = std::min(j0 + BJ, cols);
            for (std::size_t i = i0; i < i1; ++i) {
                double* __restrict__ c_row = c + i * cols;
                const double* __restrict__ a_row = a + i * inner;
                for (std::size_t p = p0; p < p1; ++p) {
                    const double scale = a_row[p];
                    const double* __restrict__ b_row = b + p * cols;
                    for (std::size_t j = j0; j < j1; ++j) c_row[j] += scale * b_row[j];
                }
            }
        }
    }
}

static void matmul_kernel(const double* __restrict__ a, const double* __restrict__ b,
                          double* __restrict__ c, std::size_t rows, std::size_t inner, std::size_t cols) {
    constexpr std::size_t BI = 32;
    const std::size_t row_blocks = (rows + BI - 1) / BI;
    run_blocks(row_blocks, rows * inner * cols, [=](std::size_t rb) {
        matmul_row_block(a, b, c, rb * BI, std::min(rb * BI + BI, rows), inner, cols);
    });
}

namespace {

const std::vector<double>& require_f64(const std::vector<Value>& args,
                                       std::size_t index,
                                       const char* operation,
                                       const char* parameter) {
    if (index >= args.size() || args[index].type != Value::F64Array ||
        !args[index].f64_array_val) {
        throw std::runtime_error(std::string("ai::") + operation + " expects " +
                                 parameter + " to be a f64_array");
    }
    return *args[index].f64_array_val;
}

std::size_t require_dim(const std::vector<Value>& args, std::size_t index,
                        const char* operation, const char* name,
                        bool allow_zero = false) {
    if (index >= args.size() || args[index].type != Value::Int ||
        args[index].int_val < (allow_zero ? 0 : 1)) {
        throw std::runtime_error(std::string("ai::") + operation + " expects " +
                                 name + " to be a " +
                                 (allow_zero ? "non-negative" : "positive") + " integer");
    }
    return static_cast<std::size_t>(args[index].int_val);
}

std::size_t checked_product(std::size_t a, std::size_t b, const char* operation) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::runtime_error(std::string("ai::") + operation + " shape is too large");
    }
    const std::size_t product = a * b;
    if (product > std::vector<double>().max_size()) {
        throw std::runtime_error(std::string("ai::") + operation + " shape is too large");
    }
    return product;
}

void require_size(const std::vector<double>& values, std::size_t expected,
                  const char* operation, const char* parameter) {
    if (values.size() != expected) {
        throw std::runtime_error(std::string("ai::") + operation + " shape mismatch: " +
                                 parameter + " has " + std::to_string(values.size()) +
                                 " values, expected " + std::to_string(expected));
    }
}

enum class Activation { Linear, Relu, Sigmoid, Tanh };

using CudaTrainFn = int (*)(const double*, const double*, double*, double*, double*, double*,
                            std::size_t, std::size_t, std::size_t, std::size_t,
                            std::size_t, double, int, double*, char*, std::size_t);

std::string executable_directory() {
    char path[4096];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return {};
    path[length] = '\0';
    std::string result(path);
    const std::size_t slash = result.find_last_of('/');
    return slash == std::string::npos ? std::string{} : result.substr(0, slash);
}

void* load_cuda_library() {
    std::vector<std::string> paths;
    if (const char* configured = std::getenv("CSY_AI_CUDA_LIBRARY")) {
        if (*configured) paths.emplace_back(configured);
    }
    if (const char* allow = std::getenv("CSY_ALLOW_CWD_LIBS")) {
        if (allow[0] == '1') paths.emplace_back("third_party/csy_ai_cuda/libcsy_ai_cuda.so");
    }
    const std::string exe_dir = executable_directory();
    if (!exe_dir.empty()) {
        paths.emplace_back(exe_dir + "/third_party/csy_ai_cuda/libcsy_ai_cuda.so");
        paths.emplace_back(exe_dir + "/../third_party/csy_ai_cuda/libcsy_ai_cuda.so");
    }
    for (const std::string& path : paths) {
        if (void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) return handle;
    }
    return nullptr;
}

template <Activation A>
inline double activate(double x) {
    if constexpr (A == Activation::Linear) return x;
    if constexpr (A == Activation::Relu) return x > 0.0 ? x : 0.0;
    if constexpr (A == Activation::Tanh) return std::tanh(x);
    return 1.0 / (1.0 + std::exp(-x)); // sigmoid
}

template <Activation A>
inline double activation_derivative(double y) {
    if constexpr (A == Activation::Linear) return 1.0;
    if constexpr (A == Activation::Relu) return y > 0.0 ? 1.0 : 0.0;
    if constexpr (A == Activation::Tanh) return 1.0 - y * y;
    return y * (1.0 - y); // sigmoid
}

template <Activation A>
void dense_kernel(const std::vector<double>& input,
                  const std::vector<double>& weights,
                  const std::vector<double>& bias,
                  std::vector<double>& output,
                  std::size_t batch, std::size_t input_width,
                  std::size_t output_width) {
    const double* __restrict__ input_data = input.data();
    const double* __restrict__ weight_data = weights.data();
    const double* __restrict__ bias_data = bias.data();
    double* __restrict__ output_data = output.data();
    constexpr std::size_t kRowsPerBlock = 8;
    const std::size_t blocks = (batch + kRowsPerBlock - 1) / kRowsPerBlock;
    run_blocks(blocks, batch * input_width * output_width, [&](std::size_t blk) {
      const std::size_t row_end = std::min(batch, (blk + 1) * kRowsPerBlock);
      for (std::size_t row = blk * kRowsPerBlock; row < row_end; ++row) {
        double* out_row = output_data + row * output_width;
        const double* input_row = input_data + row * input_width;
        for (std::size_t j = 0; j < output_width; ++j) out_row[j] = bias_data[j];
        for (std::size_t i = 0; i < input_width; ++i) {
            const double scale = input_row[i];
            const double* weight_row = weight_data + i * output_width;
            for (std::size_t j = 0; j < output_width; ++j) {
                out_row[j] += scale * weight_row[j];
            }
        }
        for (std::size_t j = 0; j < output_width; ++j) {
            out_row[j] = activate<A>(out_row[j]);
        }
      }
    });
}

template <Activation A>
void dense_backward_kernel(const std::vector<double>& input,
                           const std::vector<double>& weights,
                           const std::vector<double>& output,
                           const std::vector<double>& grad_output,
                           std::vector<double>& grad_weights,
                           std::vector<double>& grad_bias,
                           std::vector<double>& grad_input,
                           std::size_t batch, std::size_t input_width,
                           std::size_t output_width) {
    std::vector<double> delta(batch * output_width);
    for (std::size_t i = 0; i < delta.size(); ++i) {
        delta[i] = grad_output[i] * activation_derivative<A>(output[i]);
    }
    for (std::size_t row = 0; row < batch; ++row) {
        const double* delta_row = delta.data() + row * output_width;
        for (std::size_t j = 0; j < output_width; ++j) {
            grad_bias[j] += delta_row[j];
        }
    }
    constexpr std::size_t kInputsPerBlock = 16;
    const std::size_t blocks = (input_width + kInputsPerBlock - 1) / kInputsPerBlock;
    run_blocks(blocks, batch * input_width * output_width * 2, [&](std::size_t blk) {
        const std::size_t i_end = std::min(input_width, (blk + 1) * kInputsPerBlock);
        for (std::size_t row = 0; row < batch; ++row) {
            const double* input_row = input.data() + row * input_width;
            const double* delta_row = delta.data() + row * output_width;
            double* input_grad_row = grad_input.data() + row * input_width;
            for (std::size_t i = blk * kInputsPerBlock; i < i_end; ++i) {
                double* weight_grad_row = grad_weights.data() + i * output_width;
                const double scale = input_row[i];
                for (std::size_t j = 0; j < output_width; ++j) {
                    weight_grad_row[j] += scale * delta_row[j];
                }
                const double* weight_row = weights.data() + i * output_width;
                double sum = 0.0;
                for (std::size_t j = 0; j < output_width; ++j) {
                    sum += delta_row[j] * weight_row[j];
                }
                input_grad_row[i] = sum;
            }
        }
    });
}

template <Activation A, bool ScalarOutput>
double mlp_train_kernel(const std::vector<double>& input,
                        const std::vector<double>& target,
                        std::vector<double>& w1,
                        std::vector<double>& b1,
                        std::vector<double>& w2,
                        std::vector<double>& b2,
                        std::size_t batch, std::size_t input_width,
                        std::size_t hidden_width, std::size_t output_width,
                        std::size_t epochs, double learning_rate) {
    const std::size_t hidden_size = batch * hidden_width;
    const std::size_t output_size = batch * output_width;
    std::vector<double> hidden(hidden_size), output(output_size);
    std::vector<double> delta_hidden(hidden_size), delta_output(output_size);
    std::vector<double> grad_w1(w1.size()), grad_b1(b1.size());
    std::vector<double> grad_w2(w2.size()), grad_b2(b2.size());
    const double* __restrict__ input_data = input.data();
    const double* __restrict__ target_data = target.data();
    double* __restrict__ w1_data = w1.data();
    double* __restrict__ b1_data = b1.data();
    double* __restrict__ w2_data = w2.data();
    double* __restrict__ b2_data = b2.data();
    double* __restrict__ hidden_data = hidden.data();
    double* __restrict__ output_data = output.data();
    double* __restrict__ delta_hidden_data = delta_hidden.data();
    double* __restrict__ delta_output_data = delta_output.data();
    double* __restrict__ grad_w1_data = grad_w1.data();
    double* __restrict__ grad_b1_data = grad_b1.data();
    double* __restrict__ grad_w2_data = grad_w2.data();
    double* __restrict__ grad_b2_data = grad_b2.data();
    double final_loss = 0.0;

    for (std::size_t epoch = 0; epoch < epochs; ++epoch) {
        for (std::size_t row = 0; row < batch; ++row) {
            const double* x_row = input_data + row * input_width;
            double* h_row = hidden_data + row * hidden_width;
            std::copy(b1_data, b1_data + hidden_width, h_row);
            for (std::size_t i = 0; i < input_width; ++i) {
                const double scale = x_row[i];
                const double* weight_row = w1_data + i * hidden_width;
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    h_row[j] += scale * weight_row[j];
                }
            }
            for (std::size_t j = 0; j < hidden_width; ++j) {
                h_row[j] = activate<A>(h_row[j]);
            }
            double* y_row = output_data + row * output_width;
            if constexpr (ScalarOutput) {
                double z = b2_data[0];
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    z += h_row[j] * w2_data[j];
                }
                y_row[0] = activate<A>(z);
            } else {
                std::copy(b2_data, b2_data + output_width, y_row);
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    const double scale = h_row[j];
                    const double* weight_row = w2_data + j * output_width;
                    for (std::size_t k = 0; k < output_width; ++k) {
                        y_row[k] += scale * weight_row[k];
                    }
                }
                for (std::size_t k = 0; k < output_width; ++k) {
                    y_row[k] = activate<A>(y_row[k]);
                }
            }
        }

        final_loss = 0.0;
        for (std::size_t i = 0; i < output_size; ++i) {
            const double error = output_data[i] - target_data[i];
            final_loss += 0.5 * error * error;
            delta_output_data[i] = error * activation_derivative<A>(output_data[i]);
        }
        for (std::size_t row = 0; row < batch; ++row) {
            const double* dy_row = delta_output_data + row * output_width;
            double* dh_row = delta_hidden_data + row * hidden_width;
            const double* h_row = hidden_data + row * hidden_width;
            if constexpr (ScalarOutput) {
                const double delta = dy_row[0];
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    dh_row[j] = (delta * w2_data[j]) *
                                activation_derivative<A>(h_row[j]);
                }
            } else {
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    double sum = 0.0;
                    const double* weight_row = w2_data + j * output_width;
                    for (std::size_t k = 0; k < output_width; ++k) {
                        sum += dy_row[k] * weight_row[k];
                    }
                    dh_row[j] = sum * activation_derivative<A>(h_row[j]);
                }
            }
        }

        std::fill(grad_w1.begin(), grad_w1.end(), 0.0);
        std::fill(grad_b1.begin(), grad_b1.end(), 0.0);
        std::fill(grad_w2.begin(), grad_w2.end(), 0.0);
        std::fill(grad_b2.begin(), grad_b2.end(), 0.0);
        for (std::size_t row = 0; row < batch; ++row) {
            const double* x_row = input_data + row * input_width;
            const double* h_row = hidden_data + row * hidden_width;
            const double* dh_row = delta_hidden_data + row * hidden_width;
            const double* dy_row = delta_output_data + row * output_width;
            for (std::size_t i = 0; i < input_width; ++i) {
                double* grad_row = grad_w1_data + i * hidden_width;
                const double scale = x_row[i];
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    grad_row[j] += scale * dh_row[j];
                }
            }
            if constexpr (ScalarOutput) {
                const double delta = dy_row[0];
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    grad_b1_data[j] += dh_row[j];
                    grad_w2_data[j] += h_row[j] * delta;
                }
                grad_b2_data[0] += delta;
            } else {
                for (std::size_t j = 0; j < hidden_width; ++j) {
                    grad_b1_data[j] += dh_row[j];
                    double* grad_row = grad_w2_data + j * output_width;
                    const double scale = h_row[j];
                    for (std::size_t k = 0; k < output_width; ++k) {
                        grad_row[k] += scale * dy_row[k];
                    }
                }
                for (std::size_t k = 0; k < output_width; ++k) {
                    grad_b2_data[k] += dy_row[k];
                }
            }
        }
        for (std::size_t i = 0; i < w1.size(); ++i) w1_data[i] -= learning_rate * grad_w1_data[i];
        for (std::size_t i = 0; i < b1.size(); ++i) b1_data[i] -= learning_rate * grad_b1_data[i];
        for (std::size_t i = 0; i < w2.size(); ++i) w2_data[i] -= learning_rate * grad_w2_data[i];
        for (std::size_t i = 0; i < b2.size(); ++i) b2_data[i] -= learning_rate * grad_b2_data[i];
    }
    return final_loss / static_cast<double>(batch);
}

template <Activation A>
double dispatch_mlp_train(const std::vector<double>& input,
                          const std::vector<double>& target,
                          std::vector<double>& w1,
                          std::vector<double>& b1,
                          std::vector<double>& w2,
                          std::vector<double>& b2,
                          std::size_t batch, std::size_t input_width,
                          std::size_t hidden_width, std::size_t output_width,
                          std::size_t epochs, double learning_rate) {
    if (output_width == 1) {
        return mlp_train_kernel<A, true>(input, target, w1, b1, w2, b2, batch,
            input_width, hidden_width, output_width, epochs, learning_rate);
    }
    return mlp_train_kernel<A, false>(input, target, w1, b1, w2, b2, batch,
        input_width, hidden_width, output_width, epochs, learning_rate);
}

}

void register_ai_builtins(VM& vm) {
    auto& builtins = vm.builtins_table();

    // Row-major packed matrix multiplication.
    builtins["ai_matmul"] = [](VM&, std::vector<Value>& args) -> Value {
        constexpr const char* op = "matmul";
        if (args.size() != 5) {
            throw std::runtime_error(
                "ai::matmul expects (a, b, rows, inner, cols); matrices use row-major f64_array buffers");
        }
        const auto& a = require_f64(args, 0, op, "a");
        const auto& b = require_f64(args, 1, op, "b");
        const std::size_t rows = require_dim(args, 2, op, "rows", true);
        const std::size_t inner = require_dim(args, 3, op, "inner");
        const std::size_t cols = require_dim(args, 4, op, "cols");
        const std::size_t a_size = checked_product(rows, inner, op);
        const std::size_t b_size = checked_product(inner, cols, op);
        const std::size_t c_size = checked_product(rows, cols, op);
        require_size(a, a_size, op, "a");
        require_size(b, b_size, op, "b");

        Value result = Value::make_f64_array(c_size);
        matmul_kernel(a.data(), b.data(), result.f64_array_val->data(), rows, inner, cols);
        return result;
    };

    // Fused affine and activation layer.
    builtins["ai_dense"] = [](VM&, std::vector<Value>& args) -> Value {
        constexpr const char* op = "dense";
        if (args.size() != 7 || args[6].type != Value::Str) {
            throw std::runtime_error(
                "ai::dense expects (input, weights, bias, batch, input_width, output_width, activation)");
        }
        const auto& input = require_f64(args, 0, op, "input");
        const auto& weights = require_f64(args, 1, op, "weights");
        const auto& bias = require_f64(args, 2, op, "bias");
        const std::size_t batch = require_dim(args, 3, op, "batch", true);
        const std::size_t input_width = require_dim(args, 4, op, "input_width");
        const std::size_t output_width = require_dim(args, 5, op, "output_width");
        const std::string& activation = *args[6].str_val;
        Activation kind;
        if (activation == "linear") kind = Activation::Linear;
        else if (activation == "relu") kind = Activation::Relu;
        else if (activation == "sigmoid") kind = Activation::Sigmoid;
        else if (activation == "tanh") kind = Activation::Tanh;
        else {
            throw std::runtime_error(
                "ai::dense activation must be 'linear', 'relu', 'sigmoid', or 'tanh'");
        }
        const std::size_t input_size = checked_product(batch, input_width, op);
        const std::size_t weight_size = checked_product(input_width, output_width, op);
        const std::size_t output_size = checked_product(batch, output_width, op);
        require_size(input, input_size, op, "input");
        require_size(weights, weight_size, op, "weights");
        require_size(bias, output_width, op, "bias");

        Value result = Value::make_f64_array(output_size);
        auto& output = *result.f64_array_val;
        switch (kind) {
            case Activation::Linear:
                dense_kernel<Activation::Linear>(input, weights, bias, output,
                                                 batch, input_width, output_width);
                break;
            case Activation::Relu:
                dense_kernel<Activation::Relu>(input, weights, bias, output,
                                               batch, input_width, output_width);
                break;
            case Activation::Sigmoid:
                dense_kernel<Activation::Sigmoid>(input, weights, bias, output,
                                                  batch, input_width, output_width);
                break;
            case Activation::Tanh:
                dense_kernel<Activation::Tanh>(input, weights, bias, output,
                                               batch, input_width, output_width);
                break;
        }
        return result;
    };

    // gradients reduced across the batch for W and bias.
    builtins["ai_dense_backward"] = [](VM&, std::vector<Value>& args) -> Value {
        constexpr const char* op = "dense_backward";
        if (args.size() != 8 || args[7].type != Value::Str) {
            throw std::runtime_error(
                "ai::dense_backward expects (input, weights, output, grad_output, batch, input_width, output_width, activation)");
        }
        const auto& input = require_f64(args, 0, op, "input");
        const auto& weights = require_f64(args, 1, op, "weights");
        const auto& output = require_f64(args, 2, op, "output");
        const auto& grad_output = require_f64(args, 3, op, "grad_output");
        const std::size_t batch = require_dim(args, 4, op, "batch", true);
        const std::size_t input_width = require_dim(args, 5, op, "input_width");
        const std::size_t output_width = require_dim(args, 6, op, "output_width");
        const std::string& activation = *args[7].str_val;
        Activation kind;
        if (activation == "linear") kind = Activation::Linear;
        else if (activation == "relu") kind = Activation::Relu;
        else if (activation == "sigmoid") kind = Activation::Sigmoid;
        else if (activation == "tanh") kind = Activation::Tanh;
        else {
            throw std::runtime_error(
                "ai::dense_backward activation must be 'linear', 'relu', 'sigmoid', or 'tanh'");
        }
        const std::size_t input_size = checked_product(batch, input_width, op);
        const std::size_t output_size = checked_product(batch, output_width, op);
        const std::size_t weight_size = checked_product(input_width, output_width, op);
        require_size(input, input_size, op, "input");
        require_size(weights, weight_size, op, "weights");
        require_size(output, output_size, op, "output");
        require_size(grad_output, output_size, op, "grad_output");

        Value grad_weights = Value::make_f64_array(weight_size);
        Value grad_bias = Value::make_f64_array(output_width);
        Value grad_input = Value::make_f64_array(input_size);
        switch (kind) {
            case Activation::Linear:
                dense_backward_kernel<Activation::Linear>(input, weights, output, grad_output,
                    *grad_weights.f64_array_val, *grad_bias.f64_array_val,
                    *grad_input.f64_array_val, batch, input_width, output_width);
                break;
            case Activation::Relu:
                dense_backward_kernel<Activation::Relu>(input, weights, output, grad_output,
                    *grad_weights.f64_array_val, *grad_bias.f64_array_val,
                    *grad_input.f64_array_val, batch, input_width, output_width);
                break;
            case Activation::Sigmoid:
                dense_backward_kernel<Activation::Sigmoid>(input, weights, output, grad_output,
                    *grad_weights.f64_array_val, *grad_bias.f64_array_val,
                    *grad_input.f64_array_val, batch, input_width, output_width);
                break;
            case Activation::Tanh:
                dense_backward_kernel<Activation::Tanh>(input, weights, output, grad_output,
                    *grad_weights.f64_array_val, *grad_bias.f64_array_val,
                    *grad_input.f64_array_val, batch, input_width, output_width);
                break;
        }
        std::vector<Value> result;
        result.reserve(3);
        result.push_back(std::move(grad_weights));
        result.push_back(std::move(grad_bias));
        result.push_back(std::move(grad_input));
        return Value::make_vec(std::move(result));
    };

    // In place SGD update to avoid allocating a temporary scaled gradient :3
    builtins["ai_sgd_step"] = [](VM&, std::vector<Value>& args) -> Value {
        constexpr const char* op = "sgd_step";
        if (args.size() != 3 || (args[2].type != Value::Float && args[2].type != Value::Int)) {
            throw std::runtime_error("ai::sgd_step expects (parameters, gradients, learning_rate)");
        }
        const auto& gradients = require_f64(args, 1, op, "gradients");
        if (args[0].type != Value::F64Array || !args[0].f64_array_val) {
            throw std::runtime_error("ai::sgd_step expects parameters to be a f64_array");
        }
        auto& parameters = *args[0].f64_array_val;
        require_size(gradients, parameters.size(), op, "gradients");
        const double learning_rate = args[2].type == Value::Int
            ? static_cast<double>(args[2].int_val) : args[2].float_val;
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            parameters[i] -= learning_rate * gradients[i];
        }
        return Value::make_none();
    };

    builtins["ai_mlp_train"] = [](VM&, std::vector<Value>& args) -> Value {
        constexpr const char* op = "mlp_train";
        if (args.size() != 13 || args[12].type != Value::Str ||
            (args[11].type != Value::Float && args[11].type != Value::Int)) {
            throw std::runtime_error(
                "ai::mlp_train expects (input, target, W1, b1, W2, b2, batch, input_width, hidden_width, output_width, epochs, learning_rate, activation)");
        }
        const auto& input = require_f64(args, 0, op, "input");
        const auto& target = require_f64(args, 1, op, "target");
        if (args[2].type != Value::F64Array || !args[2].f64_array_val ||
            args[3].type != Value::F64Array || !args[3].f64_array_val ||
            args[4].type != Value::F64Array || !args[4].f64_array_val ||
            args[5].type != Value::F64Array || !args[5].f64_array_val) {
            throw std::runtime_error("ai::mlp_train expects W1, b1, W2, and b2 to be f64_array buffers");
        }
        auto& w1 = *args[2].f64_array_val;
        auto& b1 = *args[3].f64_array_val;
        auto& w2 = *args[4].f64_array_val;
        auto& b2 = *args[5].f64_array_val;
        const Rc<std::vector<double>>& w1_ref = args[2].f64_array_val;
        const Rc<std::vector<double>>& b1_ref = args[3].f64_array_val;
        const Rc<std::vector<double>>& w2_ref = args[4].f64_array_val;
        const Rc<std::vector<double>>& b2_ref = args[5].f64_array_val;
        if (w1_ref == b1_ref || w1_ref == w2_ref || w1_ref == b2_ref ||
            b1_ref == w2_ref || b1_ref == b2_ref || w2_ref == b2_ref ||
            w1_ref == args[0].f64_array_val || w1_ref == args[1].f64_array_val ||
            b1_ref == args[0].f64_array_val || b1_ref == args[1].f64_array_val ||
            w2_ref == args[0].f64_array_val || w2_ref == args[1].f64_array_val ||
            b2_ref == args[0].f64_array_val || b2_ref == args[1].f64_array_val) {
            throw std::runtime_error(
                "ai::mlp_train expects inputs, targets, and each parameter buffer to have independent storage");
        }
        const std::size_t batch = require_dim(args, 6, op, "batch");
        const std::size_t input_width = require_dim(args, 7, op, "input_width");
        const std::size_t hidden_width = require_dim(args, 8, op, "hidden_width");
        const std::size_t output_width = require_dim(args, 9, op, "output_width");
        const std::size_t epochs = require_dim(args, 10, op, "epochs");
        const double learning_rate = args[11].type == Value::Int
            ? static_cast<double>(args[11].int_val) : args[11].float_val;
        const std::string& activation = *args[12].str_val;
        Activation kind;
        if (activation == "linear") kind = Activation::Linear;
        else if (activation == "relu") kind = Activation::Relu;
        else if (activation == "sigmoid") kind = Activation::Sigmoid;
        else if (activation == "tanh") kind = Activation::Tanh;
        else {
            throw std::runtime_error(
                "ai::mlp_train activation must be 'linear', 'relu', 'sigmoid', or 'tanh'");
        }
        const std::size_t input_size = checked_product(batch, input_width, op);
        checked_product(batch, hidden_width, op);
        const std::size_t target_size = checked_product(batch, output_width, op);
        const std::size_t w1_size = checked_product(input_width, hidden_width, op);
        const std::size_t w2_size = checked_product(hidden_width, output_width, op);
        require_size(input, input_size, op, "input");
        require_size(target, target_size, op, "target");
        require_size(w1, w1_size, op, "W1");
        require_size(b1, hidden_width, op, "b1");
        require_size(w2, w2_size, op, "W2");
        require_size(b2, output_width, op, "b2");

        double loss = 0.0;
        switch (kind) {
            case Activation::Linear:
                loss = dispatch_mlp_train<Activation::Linear>(input, target, w1, b1, w2, b2,
                    batch, input_width, hidden_width, output_width, epochs, learning_rate);
                break;
            case Activation::Relu:
                loss = dispatch_mlp_train<Activation::Relu>(input, target, w1, b1, w2, b2,
                    batch, input_width, hidden_width, output_width, epochs, learning_rate);
                break;
            case Activation::Sigmoid:
                loss = dispatch_mlp_train<Activation::Sigmoid>(input, target, w1, b1, w2, b2,
                    batch, input_width, hidden_width, output_width, epochs, learning_rate);
                break;
            case Activation::Tanh:
                loss = dispatch_mlp_train<Activation::Tanh>(input, target, w1, b1, w2, b2,
                    batch, input_width, hidden_width, output_width, epochs, learning_rate);
                break;
        }
        return Value::make_float(loss);
    };

    builtins["ai_mlp_train_gpu"] = [](VM&, std::vector<Value>& args) -> Value {
        constexpr const char* op = "mlp_train_gpu";
        if (args.size() != 13 || args[12].type != Value::Str) {
            throw std::runtime_error(
                "ai::mlp_train_gpu expects (input, target, W1, b1, W2, b2, batch, input_width, hidden_width, output_width, epochs, learning_rate, activation)");
        }
        const auto& input = require_f64(args, 0, op, "input");
        const auto& target = require_f64(args, 1, op, "target");
        for (std::size_t i = 2; i <= 5; ++i) {
            if (args[i].type != Value::F64Array || !args[i].f64_array_val) {
                throw std::runtime_error("ai::mlp_train_gpu expects W1, b1, W2, and b2 to be f64_array buffers");
            }
        }
        auto& w1 = *args[2].f64_array_val;
        auto& b1 = *args[3].f64_array_val;
        auto& w2 = *args[4].f64_array_val;
        auto& b2 = *args[5].f64_array_val;
        const Rc<std::vector<double>>& wr1 = args[2].f64_array_val;
        const Rc<std::vector<double>>& br1 = args[3].f64_array_val;
        const Rc<std::vector<double>>& wr2 = args[4].f64_array_val;
        const Rc<std::vector<double>>& br2 = args[5].f64_array_val;
        if (wr1 == br1 || wr1 == wr2 || wr1 == br2 || br1 == wr2 || br1 == br2 || wr2 == br2 ||
            wr1 == args[0].f64_array_val || wr1 == args[1].f64_array_val ||
            br1 == args[0].f64_array_val || br1 == args[1].f64_array_val ||
            wr2 == args[0].f64_array_val || wr2 == args[1].f64_array_val ||
            br2 == args[0].f64_array_val || br2 == args[1].f64_array_val) {
            throw std::runtime_error("ai::mlp_train_gpu expects inputs, targets, and each parameter buffer to have independent storage");
        }
        const std::size_t batch = require_dim(args, 6, op, "batch");
        const std::size_t input_width = require_dim(args, 7, op, "input_width");
        const std::size_t hidden_width = require_dim(args, 8, op, "hidden_width");
        const std::size_t output_width = require_dim(args, 9, op, "output_width");
        const std::size_t epochs = require_dim(args, 10, op, "epochs");
        const double learning_rate = args[11].type == Value::Int
            ? static_cast<double>(args[11].int_val) : args[11].float_val;
        const std::string& activation = *args[12].str_val;
        int activation_id;
        if (activation == "linear") activation_id = 0;
        else if (activation == "relu") activation_id = 1;
        else if (activation == "sigmoid") activation_id = 2;
        else if (activation == "tanh") activation_id = 3;
        else throw std::runtime_error("ai::mlp_train_gpu activation must be 'linear', 'relu', 'sigmoid', or 'tanh'");

        const std::size_t cublas_limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
        if (batch > cublas_limit || input_width > cublas_limit ||
            hidden_width > cublas_limit || output_width > cublas_limit) {
            throw std::runtime_error("ai::mlp_train_gpu dimensions exceed the cuBLAS integer limit");
        }

        require_size(input, checked_product(batch, input_width, op), op, "input");
        require_size(target, checked_product(batch, output_width, op), op, "target");
        require_size(w1, checked_product(input_width, hidden_width, op), op, "W1");
        require_size(b1, hidden_width, op, "b1");
        require_size(w2, checked_product(hidden_width, output_width, op), op, "W2");
        require_size(b2, output_width, op, "b2");

        static void* cuda_handle = load_cuda_library();
        if (!cuda_handle) {
            throw std::runtime_error("ai::mlp_train_gpu CUDA backend is unavailable; build it with 'make cuda' and ensure an NVIDIA CUDA runtime is installed");
        }
        auto train = reinterpret_cast<CudaTrainFn>(dlsym(cuda_handle, "csy_ai_cuda_mlp_train"));
        if (!train) throw std::runtime_error("ai::mlp_train_gpu CUDA plugin is missing csy_ai_cuda_mlp_train");
        double loss = 0.0;
        char error[512] = {};
        const int status = train(input.data(), target.data(), w1.data(), b1.data(), w2.data(), b2.data(),
            batch, input_width, hidden_width, output_width, epochs, learning_rate,
            activation_id, &loss, error, sizeof(error));
        if (status != 0) {
            throw std::runtime_error(std::string("ai::mlp_train_gpu failed: ") +
                                     (error[0] ? error : "CUDA backend returned an unspecified error"));
        }
        return Value::make_float(loss);
    };
}

}
