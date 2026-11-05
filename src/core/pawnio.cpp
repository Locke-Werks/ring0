// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/pawnio.hpp"

#include <array>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace ring0::pawnio {
namespace {

using OpenFn    = HRESULT(STDAPICALLTYPE*)(PHANDLE);
using LoadFn    = HRESULT(STDAPICALLTYPE*)(HANDLE, const UCHAR*, SIZE_T);
using ExecuteFn = HRESULT(STDAPICALLTYPE*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64,
                                           SIZE_T, PSIZE_T);
using CloseFn   = HRESULT(STDAPICALLTYPE*)(HANDLE);

struct Lib {
    HMODULE dll = nullptr;
    OpenFn open = nullptr;
    LoadFn load = nullptr;
    ExecuteFn execute = nullptr;
    CloseFn close = nullptr;
};

template <class F>
F resolve(HMODULE m, const char* name) noexcept {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// Only absolute paths are tried. A bare "PawnIOLib.dll" would walk the DLL search order,
// and this process is elevated, so a copy planted beside it or in the cwd would run as admin.
HMODULE load_from_install() noexcept {
    std::array<wchar_t, MAX_PATH> dir{};
    DWORD bytes = static_cast<DWORD>(dir.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
                     L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                     dir.data(), &bytes) == ERROR_SUCCESS && dir[0] != L'\0') {
        std::wstring p(dir.data());
        if (p.back() != L'\\') p += L'\\';
        p += L"PawnIOLib.dll";
        if (HMODULE m = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return m;
    }

    std::array<wchar_t, MAX_PATH> def{};
    const DWORD n = ExpandEnvironmentStringsW(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll",
                                              def.data(), static_cast<DWORD>(def.size()));
    if (n == 0 || n > def.size()) return nullptr;
    return LoadLibraryExW(def.data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

const Lib& lib() noexcept {
    static const Lib l = [] {
        Lib r;
        r.dll = load_from_install();
        if (!r.dll) return r;
        r.open = resolve<OpenFn>(r.dll, "pawnio_open");
        r.load = resolve<LoadFn>(r.dll, "pawnio_load");
        r.execute = resolve<ExecuteFn>(r.dll, "pawnio_execute");
        r.close = resolve<CloseFn>(r.dll, "pawnio_close");
        if (!r.open || !r.load || !r.execute || !r.close) {
            FreeLibrary(r.dll);
            r = Lib{};
        }
        return r;
    }();
    return l;
}

} // namespace

bool available() noexcept { return lib().dll != nullptr; }

Module& Module::operator=(Module&& o) noexcept {
    if (this != &o) {
        close();
        handle_ = o.handle_;
        o.handle_ = nullptr;
    }
    return *this;
}

void Module::close() noexcept {
    if (handle_ && lib().close) lib().close(handle_);
    handle_ = nullptr;
}

Result<Module> Module::load(std::span<const std::byte> blob) noexcept {
    const Lib& l = lib();
    if (!l.dll) return std::unexpected(Error{HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), "PawnIOLib.dll"});
    if (blob.empty()) return std::unexpected(Error{E_INVALIDARG, "empty module"});

    HANDLE h = nullptr;
    if (HRESULT hr = l.open(&h); FAILED(hr) || !h)
        return std::unexpected(Error{FAILED(hr) ? hr : E_HANDLE, "pawnio_open"});

    Module m(h);
    if (HRESULT hr = l.load(h, reinterpret_cast<const UCHAR*>(blob.data()), blob.size()); FAILED(hr))
        return std::unexpected(Error{hr, "pawnio_load"});
    return m;
}

Result<Module> Module::load_file(const std::filesystem::path& path) noexcept {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return std::unexpected(Error{HRESULT_FROM_WIN32(GetLastError()), "open module file"});

    LARGE_INTEGER size{};
    // Modules are a few KiB; the cap keeps a corrupt or wrong file from becoming a huge allocation.
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
        CloseHandle(f);
        return std::unexpected(Error{HRESULT_FROM_WIN32(ERROR_BAD_FORMAT), "module file size"});
    }

    std::vector<std::byte> buf;
    try {
        buf.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (...) {
        CloseHandle(f);
        return std::unexpected(Error{E_OUTOFMEMORY, "module buffer"});
    }

    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(f);
    if (!ok || got != buf.size())
        return std::unexpected(Error{ok ? HRESULT_FROM_WIN32(ERROR_HANDLE_EOF) : HRESULT_FROM_WIN32(err),
                                     "read module file"});
    return load(buf);
}

Result<std::size_t> Module::execute(const char* function, std::span<const std::uint64_t> in,
                                    std::span<std::uint64_t> out) const noexcept {
    if (!handle_) return std::unexpected(Error{E_HANDLE, "module not loaded"});
    SIZE_T written = 0;
    const HRESULT hr = lib().execute(handle_, function,
                                     reinterpret_cast<const ULONG64*>(in.data()), in.size(),
                                     reinterpret_cast<PULONG64>(out.data()), out.size(), &written);
    if (FAILED(hr)) return std::unexpected(Error{hr, function});
    return static_cast<std::size_t>(written);
}

Result<std::uint64_t> Module::call1(const char* function, std::uint64_t arg) const noexcept {
    std::array<std::uint64_t, 1> in{arg};
    std::array<std::uint64_t, 1> out{};
    auto r = execute(function, in, out);
    if (!r) return std::unexpected(r.error());
    if (*r != 1) return std::unexpected(Error{HRESULT_FROM_WIN32(ERROR_INVALID_DATA), function});
    return out[0];
}

} // namespace ring0::pawnio
