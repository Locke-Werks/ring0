// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Thin wrapper over PawnIOLib.dll, the user-mode half of the PawnIO driver.
// The DLL is resolved at runtime from the PawnIO install so ring0 still starts on a
// machine without it; every failure comes back as an Error value, nothing throws.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

namespace ring0::pawnio {

struct Error {
    HRESULT hr = E_FAIL;
    const char* where = "";
};

template <class T>
using Result = std::expected<T, Error>;

// One open executor with one module loaded into it. The driver binds a module to the
// handle it was loaded through, so the two have the same lifetime.
class Module {
public:
    Module() noexcept = default;
    Module(Module&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    Module& operator=(Module&& o) noexcept;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    ~Module() { close(); }

    static Result<Module> load(std::span<const std::byte> blob) noexcept;
    static Result<Module> load_file(const std::filesystem::path& path) noexcept;

    // Returns the number of output cells the module wrote.
    Result<std::size_t> execute(const char* function, std::span<const std::uint64_t> in,
                                std::span<std::uint64_t> out) const noexcept;

    // Every register read the modules expose takes one cell and returns one, so this keeps
    // call sites from spelling out the spans each time.
    Result<std::uint64_t> call1(const char* function, std::uint64_t arg) const noexcept;

    explicit operator bool() const noexcept { return handle_ != nullptr; }
    void close() noexcept;

private:
    explicit Module(HANDLE h) noexcept : handle_(h) {}
    HANDLE handle_ = nullptr;
};

// True when PawnIOLib.dll was found and exports everything this wrapper uses.
bool available() noexcept;

} // namespace ring0::pawnio
