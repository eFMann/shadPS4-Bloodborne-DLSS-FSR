// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: MIT
//
// shadps4_dlss5_nvngx.dll: DLSS 5's model refuses calls unless the library
// making them has "nvngx.dll" in its path. The bridge passes each model
// function here with its arguments, and this library makes the call. Each
// result is kept in a volatile first: returning the call directly lets the
// compiler jump to the model instead, which then sees the bridge as its caller.

#include <cstdint>

using InitFn = int32_t (*)(unsigned long long, const wchar_t*, void*, void*, void*, int32_t,
                           const void*);
using CreateFn = int32_t (*)(void*, int32_t, void*, void**);
using EvaluateFn = int32_t (*)(void*, const void*, const void*, void*);
using ReleaseFn = int32_t (*)(void*);

extern "C" {

__declspec(dllexport) int32_t ShadNvngxInit(void* function, unsigned long long application,
                                            const wchar_t* data_path, void* instance,
                                            void* physical, void* device, int32_t version,
                                            const void* parameters) {
    volatile int32_t result = reinterpret_cast<InitFn>(function)(
        application, data_path, instance, physical, device, version, parameters);
    return result;
}

__declspec(dllexport) int32_t ShadNvngxCreate(void* function, void* command, int32_t feature,
                                              void* parameters, void** handle) {
    volatile int32_t result =
        reinterpret_cast<CreateFn>(function)(command, feature, parameters, handle);
    return result;
}

__declspec(dllexport) int32_t ShadNvngxEvaluate(void* function, void* command, const void* handle,
                                                const void* parameters) {
    volatile int32_t result =
        reinterpret_cast<EvaluateFn>(function)(command, handle, parameters, nullptr);
    return result;
}

__declspec(dllexport) int32_t ShadNvngxRelease(void* function, void* handle) {
    volatile int32_t result = reinterpret_cast<ReleaseFn>(function)(handle);
    return result;
}

} // extern "C"
