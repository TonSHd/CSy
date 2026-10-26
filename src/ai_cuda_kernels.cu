#include <cublas_v2.h>
#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {
enum Activation { Linear = 0, Relu = 1, Sigmoid = 2, Tanh = 3 };

__device__ float activate(float x, int activation) {
    switch (activation) {
        case Linear: return x;
        case Relu: return x > 0.0f ? x : 0.0f;
        case Sigmoid: return 1.0f / (1.0f + expf(-x));
        case Tanh: return tanhf(x);
        default: return x;
    }
}

__device__ float derivative(float y, int activation) {
    switch (activation) {
        case Linear: return 1.0f;
        case Relu: return y > 0.0f ? 1.0f : 0.0f;
        case Sigmoid: return y * (1.0f - y);
        case Tanh: return 1.0f - y * y;
        default: return 1.0f;
    }
}

__global__ void add_bias_activate(float* values, const float* bias,
                                  size_t batch, size_t width, int activation) {
    const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const size_t count = batch * width;
    if (index < count) values[index] = activate(values[index] + bias[index % width], activation);
}

__global__ void output_gradient(const float* output, const float* target,
                                float* delta, size_t count, int activation,
                                float* loss) {
    const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= count) return;
    const float error = output[index] - target[index];
    atomicAdd(loss, 0.5f * error * error);
    delta[index] = error * derivative(output[index], activation);
}

__global__ void hidden_gradient(float* delta, const float* hidden,
                                size_t count, int activation) {
    const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index < count) delta[index] *= derivative(hidden[index], activation);
}

__global__ void update_weights(float* weights, const float* gradient,
                               size_t count, float learning_rate) {
    const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index < count) weights[index] -= learning_rate * gradient[index];
}

__global__ void update_bias(float* bias, const float* delta,
                            size_t batch, size_t width, float learning_rate) {
    const size_t unit = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (unit >= width) return;
    float sum = 0.0f;
    for (size_t row = 0; row < batch; ++row) sum += delta[row * width + unit];
    bias[unit] -= learning_rate * sum;
}

void set_error(char* destination, size_t capacity, const char* text) {
    if (!destination || capacity == 0) return;
    std::snprintf(destination, capacity, "%s", text ? text : "unknown CUDA error");
}

bool cuda_ok(cudaError_t status, const char* action, char* error, size_t capacity) {
    if (status == cudaSuccess) return true;
    char message[256];
    std::snprintf(message, sizeof(message), "%s: %s", action, cudaGetErrorString(status));
    set_error(error, capacity, message);
    return false;
}

bool cublas_ok(cublasStatus_t status, const char* action, char* error, size_t capacity) {
    if (status == CUBLAS_STATUS_SUCCESS) return true;
    char message[128];
    std::snprintf(message, sizeof(message), "%s: cuBLAS status %d",
                  action, static_cast<int>(status));
    set_error(error, capacity, message);
    return false;
}

struct DeviceFloats {
    float* data = nullptr;
    ~DeviceFloats() { if (data) cudaFree(data); }
    bool allocate(size_t count, char* error, size_t capacity) {
        if (count == 0) return true;
        return cuda_ok(cudaMalloc(reinterpret_cast<void**>(&data), count * sizeof(float)),
                       "cudaMalloc", error, capacity);
    }
};

bool copy_to_device(float* destination, const std::vector<float>& source,
                    char* error, size_t capacity) {
    return cuda_ok(cudaMemcpy(destination, source.data(), source.size() * sizeof(float),
                              cudaMemcpyHostToDevice),
                   "copying data to GPU", error, capacity);
}

bool launch_ok(const char* action, char* error, size_t capacity) {
    return cuda_ok(cudaGetLastError(), action, error, capacity);
}
} // namespace

extern "C" int csy_ai_cuda_mlp_train(
    const double* input, const double* target,
    double* w1, double* b1, double* w2, double* b2,
    size_t batch, size_t input_width, size_t hidden_width, size_t output_width,
    size_t epochs, double learning_rate, int activation,
    double* final_loss, char* error, size_t error_capacity) {
    if (!input || !target || !w1 || !b1 || !w2 || !b2 || !final_loss ||
        batch == 0 || input_width == 0 || hidden_width == 0 ||
        output_width == 0 || epochs == 0 || activation < Linear || activation > Tanh) {
        set_error(error, error_capacity, "invalid CUDA MLP training arguments");
        return 1;
    }
    try {
        const size_t input_count = batch * input_width;
        const size_t hidden_count = batch * hidden_width;
        const size_t output_count = batch * output_width;
        const size_t w1_count = input_width * hidden_width;
        const size_t w2_count = hidden_width * output_width;
        std::vector<float> h_input(input_count), h_target(output_count);
        std::vector<float> h_w1(w1_count), h_b1(hidden_width);
        std::vector<float> h_w2(w2_count), h_b2(output_width);
        for (size_t i = 0; i < input_count; ++i) h_input[i] = static_cast<float>(input[i]);
        for (size_t i = 0; i < output_count; ++i) h_target[i] = static_cast<float>(target[i]);
        for (size_t i = 0; i < w1_count; ++i) h_w1[i] = static_cast<float>(w1[i]);
        for (size_t i = 0; i < hidden_width; ++i) h_b1[i] = static_cast<float>(b1[i]);
        for (size_t i = 0; i < w2_count; ++i) h_w2[i] = static_cast<float>(w2[i]);
        for (size_t i = 0; i < output_width; ++i) h_b2[i] = static_cast<float>(b2[i]);

        DeviceFloats d_input, d_target, d_w1, d_b1, d_w2, d_b2;
        DeviceFloats d_hidden, d_output, d_delta_hidden, d_delta_output;
        DeviceFloats d_grad_w1, d_grad_w2, d_loss;
        if (!d_input.allocate(input_count, error, error_capacity) ||
            !d_target.allocate(output_count, error, error_capacity) ||
            !d_w1.allocate(w1_count, error, error_capacity) ||
            !d_b1.allocate(hidden_width, error, error_capacity) ||
            !d_w2.allocate(w2_count, error, error_capacity) ||
            !d_b2.allocate(output_width, error, error_capacity) ||
            !d_hidden.allocate(hidden_count, error, error_capacity) ||
            !d_output.allocate(output_count, error, error_capacity) ||
            !d_delta_hidden.allocate(hidden_count, error, error_capacity) ||
            !d_delta_output.allocate(output_count, error, error_capacity) ||
            !d_grad_w1.allocate(w1_count, error, error_capacity) ||
            !d_grad_w2.allocate(w2_count, error, error_capacity) ||
            !d_loss.allocate(1, error, error_capacity)) return 2;

        if (!copy_to_device(d_input.data, h_input, error, error_capacity) ||
            !copy_to_device(d_target.data, h_target, error, error_capacity) ||
            !copy_to_device(d_w1.data, h_w1, error, error_capacity) ||
            !copy_to_device(d_b1.data, h_b1, error, error_capacity) ||
            !copy_to_device(d_w2.data, h_w2, error, error_capacity) ||
            !copy_to_device(d_b2.data, h_b2, error, error_capacity)) return 3;

        cublasHandle_t handle = nullptr;
        if (!cublas_ok(cublasCreate(&handle), "creating cuBLAS handle", error, error_capacity)) return 4;
        struct HandleGuard {
            cublasHandle_t value;
            ~HandleGuard() { if (value) cublasDestroy(value); }
        } guard{handle};

        const float alpha = 1.0f, beta = 0.0f;
        constexpr int threads = 256;
        const int hidden_blocks = static_cast<int>((hidden_count + threads - 1) / threads);
        const int output_blocks = static_cast<int>((output_count + threads - 1) / threads);
        const int w1_blocks = static_cast<int>((w1_count + threads - 1) / threads);
        const int w2_blocks = static_cast<int>((w2_count + threads - 1) / threads);
        for (size_t epoch = 0; epoch < epochs; ++epoch) {
            // Row-major Y=X*W is column-major Y^T=W^T*X^T in the same buffers.
            if (!cublas_ok(cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N,
                    static_cast<int>(hidden_width), static_cast<int>(batch),
                    static_cast<int>(input_width), &alpha, d_w1.data,
                    static_cast<int>(hidden_width), d_input.data,
                    static_cast<int>(input_width), &beta, d_hidden.data,
                    static_cast<int>(hidden_width)),
                    "first-layer cuBLAS multiply", error, error_capacity)) return 5;
            add_bias_activate<<<hidden_blocks, threads>>>(d_hidden.data, d_b1.data,
                batch, hidden_width, activation);
            if (!launch_ok("first-layer activation", error, error_capacity)) return 6;

            if (!cublas_ok(cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N,
                    static_cast<int>(output_width), static_cast<int>(batch),
                    static_cast<int>(hidden_width), &alpha, d_w2.data,
                    static_cast<int>(output_width), d_hidden.data,
                    static_cast<int>(hidden_width), &beta, d_output.data,
                    static_cast<int>(output_width)),
                    "output-layer cuBLAS multiply", error, error_capacity)) return 7;
            add_bias_activate<<<output_blocks, threads>>>(d_output.data, d_b2.data,
                batch, output_width, activation);
            if (!launch_ok("output-layer activation", error, error_capacity)) return 8;

            if (!cuda_ok(cudaMemset(d_loss.data, 0, sizeof(float)),
                         "resetting loss", error, error_capacity)) return 9;
            output_gradient<<<output_blocks, threads>>>(d_output.data, d_target.data,
                d_delta_output.data, output_count, activation, d_loss.data);
            if (!launch_ok("output gradient", error, error_capacity)) return 10;

            if (!cublas_ok(cublasSgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N,
                    static_cast<int>(hidden_width), static_cast<int>(batch),
                    static_cast<int>(output_width), &alpha, d_w2.data,
                    static_cast<int>(output_width), d_delta_output.data,
                    static_cast<int>(output_width), &beta, d_delta_hidden.data,
                    static_cast<int>(hidden_width)),
                    "hidden-gradient cuBLAS multiply", error, error_capacity)) return 11;
            hidden_gradient<<<hidden_blocks, threads>>>(d_delta_hidden.data, d_hidden.data,
                hidden_count, activation);
            if (!launch_ok("hidden gradient", error, error_capacity)) return 12;

            if (!cublas_ok(cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_T,
                    static_cast<int>(hidden_width), static_cast<int>(input_width),
                    static_cast<int>(batch), &alpha, d_delta_hidden.data,
                    static_cast<int>(hidden_width), d_input.data,
                    static_cast<int>(input_width), &beta, d_grad_w1.data,
                    static_cast<int>(hidden_width)),
                    "first-layer gradient cuBLAS multiply", error, error_capacity)) return 13;
            if (!cublas_ok(cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_T,
                    static_cast<int>(output_width), static_cast<int>(hidden_width),
                    static_cast<int>(batch), &alpha, d_delta_output.data,
                    static_cast<int>(output_width), d_hidden.data,
                    static_cast<int>(hidden_width), &beta, d_grad_w2.data,
                    static_cast<int>(output_width)),
                    "output-layer gradient cuBLAS multiply", error, error_capacity)) return 14;

            update_weights<<<w1_blocks, threads>>>(d_w1.data, d_grad_w1.data,
                w1_count, static_cast<float>(learning_rate));
            update_weights<<<w2_blocks, threads>>>(d_w2.data, d_grad_w2.data,
                w2_count, static_cast<float>(learning_rate));
            update_bias<<<static_cast<int>((hidden_width + threads - 1) / threads), threads>>>(
                d_b1.data, d_delta_hidden.data, batch, hidden_width,
                static_cast<float>(learning_rate));
            update_bias<<<static_cast<int>((output_width + threads - 1) / threads), threads>>>(
                d_b2.data, d_delta_output.data, batch, output_width,
                static_cast<float>(learning_rate));
            if (!launch_ok("parameter update", error, error_capacity)) return 15;
        }

        float loss_value = 0.0f;
        if (!cuda_ok(cudaMemcpy(&loss_value, d_loss.data, sizeof(float),
                                cudaMemcpyDeviceToHost),
                     "copying loss from GPU", error, error_capacity)) return 16;
        std::vector<float> updated_w1(w1_count), updated_b1(hidden_width);
        std::vector<float> updated_w2(w2_count), updated_b2(output_width);
        if (!cuda_ok(cudaMemcpy(updated_w1.data(), d_w1.data, w1_count * sizeof(float),
                                cudaMemcpyDeviceToHost), "copying W1 from GPU", error, error_capacity) ||
            !cuda_ok(cudaMemcpy(updated_b1.data(), d_b1.data, hidden_width * sizeof(float),
                                cudaMemcpyDeviceToHost), "copying b1 from GPU", error, error_capacity) ||
            !cuda_ok(cudaMemcpy(updated_w2.data(), d_w2.data, w2_count * sizeof(float),
                                cudaMemcpyDeviceToHost), "copying W2 from GPU", error, error_capacity) ||
            !cuda_ok(cudaMemcpy(updated_b2.data(), d_b2.data, output_width * sizeof(float),
                                cudaMemcpyDeviceToHost), "copying b2 from GPU", error, error_capacity)) return 17;
        for (size_t i = 0; i < w1_count; ++i) w1[i] = updated_w1[i];
        for (size_t i = 0; i < hidden_width; ++i) b1[i] = updated_b1[i];
        for (size_t i = 0; i < w2_count; ++i) w2[i] = updated_w2[i];
        for (size_t i = 0; i < output_width; ++i) b2[i] = updated_b2[i];
        *final_loss = static_cast<double>(loss_value / static_cast<float>(batch));
        return 0;
    } catch (const std::exception& exception) {
        set_error(error, error_capacity, exception.what());
        return 18;
    } catch (...) {
        set_error(error, error_capacity, "unknown CUDA training failure");
        return 19;
    }
}
