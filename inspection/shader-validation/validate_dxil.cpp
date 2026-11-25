#include <windows.h>
#include <wrl/client.h>
#include <dxcapi.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <type_traits>

using Microsoft::WRL::ComPtr;

namespace
{
    struct FreeModule
    {
        void operator()(HMODULE module) const noexcept
        {
            FreeLibrary(module);
        }
    };

    using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, FreeModule>;

    Module LoadSdkModule(const std::filesystem::path& path)
    {
        Module module(LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
        if (!module)
        {
            const DWORD error = GetLastError();
            std::fprintf(stderr, "Could not load %s (Win32 error %lu).\n",
                path.u8string().c_str(), error);
        }
        return module;
    }

    bool Check(HRESULT result, const char* operation)
    {
        if (SUCCEEDED(result))
            return true;

        std::fprintf(stderr, "%s failed (HRESULT 0x%08lX).\n",
            operation, static_cast<unsigned long>(result));
        return false;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        std::fputs("Usage: validate_dxil.exe <path-to-dxcompiler.dll> <path-to-shader.dxil>\n", stderr);
        return 2;
    }

    std::error_code pathError;
    const auto compilerPath = std::filesystem::absolute(argv[1], pathError);
    if (pathError)
    {
        std::fprintf(stderr, "Could not resolve the DXC compiler path (%s).\n",
            pathError.message().c_str());
        return 2;
    }

    // DXC can load the validator by name, so preload its matching SDK copy.
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        std::fprintf(stderr, "Could not set the DLL search policy (Win32 error %lu).\n", GetLastError());
        return 2;
    }
    const auto dxil = LoadSdkModule(compilerPath.parent_path() / L"dxil.dll");
    if (!dxil)
        return 2;
    const auto compiler = LoadSdkModule(compilerPath);
    if (!compiler)
        return 2;

    const FARPROC entryPoint = GetProcAddress(compiler.get(), "DxcCreateInstance");
    if (!entryPoint)
    {
        std::fputs("The DXC library does not export DxcCreateInstance.\n", stderr);
        return 2;
    }
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(entryPoint);

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcValidator> validator;
    if (!Check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Create DXC utilities") ||
        !Check(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator)), "Create DXIL validator"))
    {
        return 2;
    }

    ComPtr<IDxcBlobEncoding> shader;
    if (!Check(utils->LoadFile(argv[2], nullptr, &shader), "Read DXIL file"))
        return 2;

    ComPtr<IDxcOperationResult> result;
    if (!Check(validator->Validate(shader.Get(), DxcValidatorFlags_Default, &result), "Run DXIL validator"))
        return 2;

    ComPtr<IDxcBlobEncoding> diagnostics;
    ComPtr<IDxcBlobUtf8> utf8;
    if (SUCCEEDED(result->GetErrorBuffer(&diagnostics)) && diagnostics &&
        SUCCEEDED(utils->GetBlobAsUtf8(diagnostics.Get(), &utf8)) && utf8 && utf8->GetStringLength() != 0)
    {
        std::fwrite(utf8->GetStringPointer(), 1, utf8->GetStringLength(), stderr);
    }

    HRESULT status = E_FAIL;
    if (!Check(result->GetStatus(&status), "Read DXIL validation result"))
        return 2;
    if (!Check(status, "DXIL validation"))
        return 1;

    std::puts("DXIL validation passed.");
    return 0;
}
