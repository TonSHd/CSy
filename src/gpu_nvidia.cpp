// NVIDIA GPU support for CSy, wired in dynamically, at runtime.
// This file was written by Claude, CSy maintainer.

#include "gpu_nvidia.hpp"
#include "value.hpp"
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/wait.h>
#include <cerrno>
#include <unistd.h>
#include <climits>

namespace csy {

extern "C" {
typedef int (*fn_cudaGetDeviceCount)(int*);
typedef int (*fn_cudaRuntimeGetVersion)(int*);
typedef int (*fn_cudaMalloc)(void**, size_t);
typedef int (*fn_cudaFree)(void*);
typedef int (*fn_cudaMemcpy)(void*, const void*, size_t, int);

typedef int (*fn_cublasCreate)(void**);
typedef int (*fn_cublasDestroy)(void*);
typedef int (*fn_cublasSgemm)(void*, int, int, int, int, int,
                               const float*, const float*, int,
                               const float*, int,
                               const float*, float*, int);
}

// Stable public ABI constants (unchanged since CUDA 1.0 / cuBLAS 1.0).
static const int CSY_cudaMemcpyHostToDevice = 1;
static const int CSY_cudaMemcpyDeviceToHost = 2;
static const int CSY_cudaSuccess = 0;
static const int CSY_cublasSuccess = 0;
static const int CSY_CUBLAS_OP_N = 0;

struct CudaBinding {
    bool attempted = false;
    bool loaded = false;
    fn_cudaGetDeviceCount cudaGetDeviceCount = nullptr;
    fn_cudaRuntimeGetVersion cudaRuntimeGetVersion = nullptr;
    fn_cudaMalloc cudaMalloc = nullptr;
    fn_cudaFree cudaFree = nullptr;
    fn_cudaMemcpy cudaMemcpy = nullptr;
    fn_cublasCreate cublasCreate = nullptr;
    fn_cublasDestroy cublasDestroy = nullptr;
    fn_cublasSgemm cublasSgemm = nullptr;
};

extern "C" {
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef int (*fn_nvmlInit)(void);
typedef int (*fn_nvmlShutdown)(void);
typedef int (*fn_nvmlDeviceGetCount)(unsigned int*);
typedef int (*fn_nvmlDeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
typedef int (*fn_nvmlDeviceGetName)(nvmlDevice_t, char*, unsigned int);
struct NvmlMemory { unsigned long long total; unsigned long long free; unsigned long long used; };
typedef int (*fn_nvmlDeviceGetMemoryInfo)(nvmlDevice_t, NvmlMemory*);
typedef int (*fn_nvmlDeviceGetTemperature)(nvmlDevice_t, int, unsigned int*);
struct NvmlUtilization { unsigned int gpu; unsigned int memory; };
typedef int (*fn_nvmlDeviceGetUtilizationRates)(nvmlDevice_t, NvmlUtilization*);
typedef int (*fn_nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int*);
typedef int (*fn_nvmlDeviceGetCudaComputeCapability)(nvmlDevice_t, int*, int*);
typedef int (*fn_nvmlSystemGetDriverVersion)(char*, unsigned int);
}
static const int CSY_NVML_SUCCESS = 0;
static const int CSY_NVML_TEMPERATURE_GPU = 0;

struct NvmlBinding {
    bool attempted = false;
    bool loaded = false;
    fn_nvmlDeviceGetCount nvmlDeviceGetCount = nullptr;
    fn_nvmlDeviceGetHandleByIndex nvmlDeviceGetHandleByIndex = nullptr;
    fn_nvmlDeviceGetName nvmlDeviceGetName = nullptr;
    fn_nvmlDeviceGetMemoryInfo nvmlDeviceGetMemoryInfo = nullptr;
    fn_nvmlDeviceGetTemperature nvmlDeviceGetTemperature = nullptr;
    fn_nvmlDeviceGetUtilizationRates nvmlDeviceGetUtilizationRates = nullptr;
    fn_nvmlDeviceGetPowerUsage nvmlDeviceGetPowerUsage = nullptr;
    fn_nvmlDeviceGetCudaComputeCapability nvmlDeviceGetCudaComputeCapability = nullptr;
    fn_nvmlSystemGetDriverVersion nvmlSystemGetDriverVersion = nullptr;
};

static std::string exe_dir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string path(buf);
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? "" : path.substr(0, pos);
}

static std::string cwd_dir() {
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return "";
    return std::string(buf);
}

static bool path_exists(const std::string& p) {
    return access(p.c_str(), F_OK) == 0;
}

static std::string find_repo_root(bool debug) {
    std::vector<std::string> candidates;
    std::string ed = exe_dir();
    if (!ed.empty()) {
        candidates.push_back(ed);
        candidates.push_back(ed + "/..");
    }
    const char* allow_cwd = std::getenv("CSY_ALLOW_CWD_LIBS");
    std::string cd = (allow_cwd && allow_cwd[0] == '1') ? cwd_dir() : std::string();
    if (!cd.empty()) {
        candidates.push_back(cd);
        candidates.push_back(cd + "/..");
    }
    for (const auto& root : candidates) {
        bool has_script = path_exists(root + "/scripts/setup_cuda.sh");
        bool has_libs = path_exists(root + "/third_party/cuda/nvidia/cuda_runtime/lib/libcudart.so.12");
        if (debug) {
            fprintf(stderr, "[csy][cuda-debug] checking '%s' (script=%s, libs=%s)\n",
                    root.c_str(), has_script ? "yes" : "no", has_libs ? "yes" : "no");
        }
        if (has_script || has_libs) return root;
    }
    return ed;
}

static bool try_dlopen_pair(const std::string& rt_path, const std::string& blas_path,
                             CudaBinding& b, bool debug) {
    void* rt = dlopen(rt_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!rt) {
        if (debug) fprintf(stderr, "[csy][cuda-debug] dlopen('%s') failed: %s\n",
                            rt_path.c_str(), dlerror());
        return false;
    }
    void* blas = dlopen(blas_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!blas) {
        if (debug) fprintf(stderr, "[csy][cuda-debug] dlopen('%s') failed: %s\n",
                            blas_path.c_str(), dlerror());
        dlclose(rt);
        return false;
    }

    b.cudaGetDeviceCount = (fn_cudaGetDeviceCount)dlsym(rt, "cudaGetDeviceCount");
    b.cudaRuntimeGetVersion = (fn_cudaRuntimeGetVersion)dlsym(rt, "cudaRuntimeGetVersion");
    b.cudaMalloc = (fn_cudaMalloc)dlsym(rt, "cudaMalloc");
    b.cudaFree = (fn_cudaFree)dlsym(rt, "cudaFree");
    b.cudaMemcpy = (fn_cudaMemcpy)dlsym(rt, "cudaMemcpy");
    b.cublasCreate = (fn_cublasCreate)dlsym(blas, "cublasCreate_v2");
    b.cublasDestroy = (fn_cublasDestroy)dlsym(blas, "cublasDestroy_v2");
    b.cublasSgemm = (fn_cublasSgemm)dlsym(blas, "cublasSgemm_v2");

    if (!b.cudaGetDeviceCount || !b.cudaRuntimeGetVersion || !b.cublasCreate || !b.cublasSgemm) {
        if (debug) fprintf(stderr, "[csy][cuda-debug] '%s' loaded but a required symbol was missing\n",
                            rt_path.c_str());
        b = CudaBinding{};
        return false;
    }
    b.loaded = true;
    if (debug) fprintf(stderr, "[csy][cuda-debug] loaded '%s' + '%s' successfully\n",
                        rt_path.c_str(), blas_path.c_str());
    return true;
}

static CudaBinding& binding() {
    static CudaBinding b;
    if (b.attempted) return b;
    b.attempted = true;
    bool debug = std::getenv("CSY_CUDA_DEBUG") != nullptr;

    // library search path.
    if (try_dlopen_pair("libcudart.so.12", "libcublas.so.12", b, debug)) return b;

    std::string root = find_repo_root(debug);
    if (!root.empty()) {
        std::string rt = root + "/third_party/cuda/nvidia/cuda_runtime/lib/libcudart.so.12";
        std::string bl = root + "/third_party/cuda/nvidia/cublas/lib/libcublas.so.12";
        if (try_dlopen_pair(rt, bl, b, debug)) return b;

        const char* allow_setup = std::getenv("CSY_AUTO_CUDA_SETUP");
        if (allow_setup && std::string(allow_setup) == "1") {
            std::string script = root + "/scripts/setup_cuda.sh";
            if (access(script.c_str(), F_OK) == 0) {
                fprintf(stderr, "[csy] #inc cuda used, no GPU toolkit found yet\n");
                fprintf(stderr, "[csy] fetching real NVIDIA CUDA support (one time, ~600MB)...\n");
                int rc = -1;
                const pid_t child = ::fork();
                if (child == 0) {
                    ::execl("/bin/bash", "bash", script.c_str(), static_cast<char*>(nullptr));
                    _exit(127);
                }
                if (child > 0) {
                    int status = 0;
                    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                    if (WIFEXITED(status)) rc = WEXITSTATUS(status);
                }
                if (rc == 0) {
                    try_dlopen_pair(rt, bl, b, debug);
                } else {
                    fprintf(stderr, "[csy] CUDA setup failed, continuing on CPU\n");
                }
            } else if (debug) {
                fprintf(stderr, "[csy][cuda-debug] no scripts/setup_cuda.sh found under '%s' either\n",
                        root.c_str());
            }
        }
    }
    return b;
}

static int nv_device_count_impl() {
    auto& b = binding();
    if (!b.loaded) return 0;
    int c = 0;
    if (b.cudaGetDeviceCount(&c) != CSY_cudaSuccess) return 0;
    return c;
}

static bool nv_is_available() {
    return nv_device_count_impl() > 0;
}

static std::string nv_driver_version_impl() {
    auto& b = binding();
    if (!b.loaded || nv_device_count_impl() == 0) return "n/a";
    int ver = 0;
    if (b.cudaRuntimeGetVersion(&ver) != CSY_cudaSuccess) return "n/a";
    std::ostringstream ss;
    ss << (ver / 1000) << "." << ((ver % 1000) / 10);
    return ss.str();
}

static NvmlBinding& nvml_binding() {
    static NvmlBinding b;
    if (b.attempted) return b;
    b.attempted = true;
    bool debug = std::getenv("CSY_CUDA_DEBUG") != nullptr;

    void* lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) {
        if (debug) fprintf(stderr, "[csy][cuda-debug] dlopen('libnvidia-ml.so.1') failed: %s\n", dlerror());
        lib = dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) {
            if (debug) fprintf(stderr, "[csy][cuda-debug] dlopen('libnvidia-ml.so') failed: %s\n", dlerror());
            return b;
        }
    }

    auto nvmlInit = (fn_nvmlInit)dlsym(lib, "nvmlInit_v2");
    b.nvmlDeviceGetCount = (fn_nvmlDeviceGetCount)dlsym(lib, "nvmlDeviceGetCount_v2");
    b.nvmlDeviceGetHandleByIndex = (fn_nvmlDeviceGetHandleByIndex)dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    b.nvmlDeviceGetName = (fn_nvmlDeviceGetName)dlsym(lib, "nvmlDeviceGetName");
    b.nvmlDeviceGetMemoryInfo = (fn_nvmlDeviceGetMemoryInfo)dlsym(lib, "nvmlDeviceGetMemoryInfo");
    b.nvmlDeviceGetTemperature = (fn_nvmlDeviceGetTemperature)dlsym(lib, "nvmlDeviceGetTemperature");
    b.nvmlDeviceGetUtilizationRates = (fn_nvmlDeviceGetUtilizationRates)dlsym(lib, "nvmlDeviceGetUtilizationRates");
    b.nvmlDeviceGetPowerUsage = (fn_nvmlDeviceGetPowerUsage)dlsym(lib, "nvmlDeviceGetPowerUsage");
    b.nvmlDeviceGetCudaComputeCapability = (fn_nvmlDeviceGetCudaComputeCapability)dlsym(lib, "nvmlDeviceGetCudaComputeCapability");
    b.nvmlSystemGetDriverVersion = (fn_nvmlSystemGetDriverVersion)dlsym(lib, "nvmlSystemGetDriverVersion");

    if (!nvmlInit || !b.nvmlDeviceGetCount || !b.nvmlDeviceGetHandleByIndex || !b.nvmlDeviceGetName) {
        if (debug) fprintf(stderr, "[csy][cuda-debug] libnvidia-ml loaded but required symbols were missing\n");
        b = NvmlBinding{};
        return b;
    }
    if (nvmlInit() != CSY_NVML_SUCCESS) {
        if (debug) fprintf(stderr, "[csy][cuda-debug] nvmlInit_v2() failed\n");
        b = NvmlBinding{};
        return b;
    }
    b.loaded = true;
    if (debug) fprintf(stderr, "[csy][cuda-debug] NVML loaded successfully\n");
    return b;
}

static bool nvml_get_handle(unsigned int index, nvmlDevice_t& out) {
    auto& b = nvml_binding();
    if (!b.loaded) return false;
    unsigned int count = 0;
    if (b.nvmlDeviceGetCount(&count) != CSY_NVML_SUCCESS || index >= count) return false;
    return b.nvmlDeviceGetHandleByIndex(index, &out) == CSY_NVML_SUCCESS;
}

static std::string nvml_name(unsigned int index) {
    nvmlDevice_t dev;
    if (!nvml_get_handle(index, dev)) return "n/a";
    char buf[256];
    std::memset(buf, 0, sizeof(buf));
    if (nvml_binding().nvmlDeviceGetName(dev, buf, sizeof(buf) - 1) != CSY_NVML_SUCCESS) return "n/a";
    return std::string(buf);
}

static bool nvml_memory(unsigned int index, unsigned long long& total, unsigned long long& free) {
    nvmlDevice_t dev;
    if (!nvml_get_handle(index, dev)) return false;
    auto& b = nvml_binding();
    if (!b.nvmlDeviceGetMemoryInfo) return false;
    NvmlMemory mem;
    if (b.nvmlDeviceGetMemoryInfo(dev, &mem) != CSY_NVML_SUCCESS) return false;
    total = mem.total;
    free = mem.free;
    return true;
}

static int nvml_temperature(unsigned int index) {
    nvmlDevice_t dev;
    if (!nvml_get_handle(index, dev)) return -1;
    auto& b = nvml_binding();
    if (!b.nvmlDeviceGetTemperature) return -1;
    unsigned int t = 0;
    if (b.nvmlDeviceGetTemperature(dev, CSY_NVML_TEMPERATURE_GPU, &t) != CSY_NVML_SUCCESS) return -1;
    return (int)t;
}

static int nvml_utilization_percent(unsigned int index) {
    nvmlDevice_t dev;
    if (!nvml_get_handle(index, dev)) return -1;
    auto& b = nvml_binding();
    if (!b.nvmlDeviceGetUtilizationRates) return -1;
    NvmlUtilization u;
    if (b.nvmlDeviceGetUtilizationRates(dev, &u) != CSY_NVML_SUCCESS) return -1;
    return (int)u.gpu;
}

static double nvml_power_watts(unsigned int index) {
    nvmlDevice_t dev;
    if (!nvml_get_handle(index, dev)) return -1.0;
    auto& b = nvml_binding();
    if (!b.nvmlDeviceGetPowerUsage) return -1.0;
    unsigned int mw = 0;
    if (b.nvmlDeviceGetPowerUsage(dev, &mw) != CSY_NVML_SUCCESS) return -1.0;
    return (double)mw / 1000.0;
}

static bool nvml_compute_capability(unsigned int index, int& major, int& minor) {
    nvmlDevice_t dev;
    if (!nvml_get_handle(index, dev)) return false;
    auto& b = nvml_binding();
    if (!b.nvmlDeviceGetCudaComputeCapability) return false;
    return b.nvmlDeviceGetCudaComputeCapability(dev, &major, &minor) == CSY_NVML_SUCCESS;
}

static std::string nvml_driver_version() {
    auto& b = nvml_binding();
    if (!b.loaded || !b.nvmlSystemGetDriverVersion) return "n/a";
    char buf[128];
    std::memset(buf, 0, sizeof(buf));
    if (b.nvmlSystemGetDriverVersion(buf, sizeof(buf) - 1) != CSY_NVML_SUCCESS) return "n/a";
    return std::string(buf);
}

static bool cuda_matmul(const std::vector<float>& A, const std::vector<float>& B,
                         std::vector<float>& C, int M, int K, int N) {
    auto& b = binding();
    void* handle = nullptr;
    if (b.cublasCreate(&handle) != CSY_cublasSuccess) return false;

    void *dA = nullptr, *dB = nullptr, *dC = nullptr;
    bool ok = b.cudaMalloc(&dA, sizeof(float) * A.size()) == CSY_cudaSuccess;
    ok = ok && b.cudaMalloc(&dB, sizeof(float) * B.size()) == CSY_cudaSuccess;
    ok = ok && b.cudaMalloc(&dC, sizeof(float) * C.size()) == CSY_cudaSuccess;

    if (ok) {
        b.cudaMemcpy(dA, A.data(), sizeof(float) * A.size(), CSY_cudaMemcpyHostToDevice);
        b.cudaMemcpy(dB, B.data(), sizeof(float) * B.size(), CSY_cudaMemcpyHostToDevice);
        float alpha = 1.0f, beta = 0.0f;
        int st = b.cublasSgemm(handle, CSY_CUBLAS_OP_N, CSY_CUBLAS_OP_N,
                                N, M, K, &alpha, (const float*)dB, N,
                                (const float*)dA, K, &beta, (float*)dC, N);
        if (st == CSY_cublasSuccess) {
            b.cudaMemcpy(C.data(), dC, sizeof(float) * C.size(), CSY_cudaMemcpyDeviceToHost);
        } else {
            ok = false;
        }
    }
    if (dA) b.cudaFree(dA);
    if (dB) b.cudaFree(dB);
    if (dC) b.cudaFree(dC);
    b.cublasDestroy(handle);
    return ok;
}

static bool is_tensor_2d_local(const Value& v) {
    if (v.type != Value::Vec || v.vec_val->empty()) return false;
    for (const auto& row : *v.vec_val) {
        if (row.type != Value::Vec) return false;
    }
    return true;
}

static double to_double_local(const Value& v) {
    if (v.type == Value::Int) return (double)v.int_val;
    if (v.type == Value::Float) return v.float_val;
    if (v.type == Value::Bool) return v.bool_val ? 1.0 : 0.0;
    return 0.0;
}

void register_nvidia_builtins(VM& vm) {
    auto& builtins = vm.builtins_table();

    auto device_index_arg = [](std::vector<Value>& args) -> unsigned int {
        if (args.empty()) return 0;
        long long i = args[0].type == Value::Int ? args[0].int_val
                     : (long long)to_double_local(args[0]);
        return i < 0 ? 0 : (unsigned int)i;
    };

    builtins["nv_available"] = [](VM&, std::vector<Value>&) -> Value {
        return Value::make_bool(nv_is_available());
    };

    builtins["nv_device_count"] = [](VM&, std::vector<Value>&) -> Value {
        return Value::make_int(nv_device_count_impl());
    };

    builtins["nv_name"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        unsigned int idx = device_index_arg(args);
        std::string name = nvml_name(idx);
        if (name != "n/a") return Value::make_str(name);
        if (nv_is_available()) return Value::make_str("cuda device " + std::to_string(idx));
        return Value::make_str("none");
    };

    builtins["nv_driver_version"] = [](VM&, std::vector<Value>&) -> Value {
        std::string real = nvml_driver_version();
        if (real != "n/a") return Value::make_str(real);
        return Value::make_str(nv_driver_version_impl());
    };

    builtins["nv_memory_total"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        unsigned long long total = 0, free = 0;
        if (!nvml_memory(device_index_arg(args), total, free)) return Value::make_int(-1);
        return Value::make_int((long long)total);
    };
    builtins["nv_memory_free"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        unsigned long long total = 0, free = 0;
        if (!nvml_memory(device_index_arg(args), total, free)) return Value::make_int(-1);
        return Value::make_int((long long)free);
    };
    builtins["nv_memory_used"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        unsigned long long total = 0, free = 0;
        if (!nvml_memory(device_index_arg(args), total, free)) return Value::make_int(-1);
        return Value::make_int((long long)(total - free));
    };

    builtins["nv_temperature"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        return Value::make_int(nvml_temperature(device_index_arg(args)));
    };
    builtins["nv_utilization"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        return Value::make_int(nvml_utilization_percent(device_index_arg(args)));
    };
    builtins["nv_power"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        return Value::make_float(nvml_power_watts(device_index_arg(args)));
    };

    builtins["nv_compute_capability"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        int major = 0, minor = 0;
        if (!nvml_compute_capability(device_index_arg(args), major, minor)) {
            return Value::make_str("n/a");
        }
        return Value::make_str(std::to_string(major) + "." + std::to_string(minor));
    };

    builtins["nv_info"] = [device_index_arg](VM&, std::vector<Value>& args) -> Value {
        unsigned int idx = device_index_arg(args);
        std::unordered_map<std::string, Value> info;
        info["available"] = Value::make_bool(nv_is_available());
        info["device_count"] = Value::make_int(nv_device_count_impl());
        info["index"] = Value::make_int((long long)idx);
        std::string name = nvml_name(idx);
        info["name"] = Value::make_str(name != "n/a" ? name :
            (nv_is_available() ? "cuda device " + std::to_string(idx) : "none"));
        std::string drv = nvml_driver_version();
        info["driver_version"] = Value::make_str(drv != "n/a" ? drv : nv_driver_version_impl());
        int major = 0, minor = 0;
        info["compute_capability"] = nvml_compute_capability(idx, major, minor)
            ? Value::make_str(std::to_string(major) + "." + std::to_string(minor))
            : Value::make_str("n/a");
        unsigned long long total = 0, free = 0;
        bool have_mem = nvml_memory(idx, total, free);
        info["memory_total_bytes"] = Value::make_int(have_mem ? (long long)total : -1);
        info["memory_free_bytes"] = Value::make_int(have_mem ? (long long)free : -1);
        info["memory_used_bytes"] = Value::make_int(have_mem ? (long long)(total - free) : -1);
        info["temperature_c"] = Value::make_int(nvml_temperature(idx));
        info["utilization_pct"] = Value::make_int(nvml_utilization_percent(idx));
        info["power_watts"] = Value::make_float(nvml_power_watts(idx));
        return Value::make_dict(std::move(info));
    };

    builtins["nv_matmul"] = [](VM& vm2, std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::make_none();
        if (!is_tensor_2d_local(args[0]) || !is_tensor_2d_local(args[1])) {
            throw std::runtime_error("nv::matmul requires two 2D tensors");
        }
        if (nv_is_available()) {
            const auto& Amat = *args[0].vec_val;
            const auto& Bmat = *args[1].vec_val;
            int M = (int)Amat.size();
            int K = (int)Amat[0].vec_val->size();
            int K2 = (int)Bmat.size();
            int N = (int)Bmat[0].vec_val->size();
            if (K == K2 && K > 0 && N > 0) {
                std::vector<float> A((size_t)M * K), B((size_t)K * N), C((size_t)M * N);
                for (int i = 0; i < M; i++)
                    for (int p = 0; p < K; p++)
                        A[(size_t)i * K + p] = (float)to_double_local((*Amat[i].vec_val)[p]);
                for (int p = 0; p < K; p++)
                    for (int j = 0; j < N; j++)
                        B[(size_t)p * N + j] = (float)to_double_local((*Bmat[p].vec_val)[j]);
                if (cuda_matmul(A, B, C, M, K, N)) {
                    std::vector<Value> result;
                    result.reserve(M);
                    for (int i = 0; i < M; i++) {
                        std::vector<Value> row;
                        row.reserve(N);
                        for (int j = 0; j < N; j++) row.push_back(Value::make_float(C[(size_t)i * N + j]));
                        result.push_back(Value::make_vec(std::move(row)));
                    }
                    return Value::make_vec(std::move(result));
                }
                // GPU path failed for any reason - fall through to CPU.
            }
        }
        // CPU fallback: always available, always correct, the exact same
        // implementation bc::matmul uses.
        auto it = vm2.builtins_table().find("matmul");
        if (it != vm2.builtins_table().end()) {
            return it->second(vm2, args);
        }
        return Value::make_none();
    };
}

}
