// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Turns a weights file path into the name its store knows it by. Ollama keeps
// content-addressed blobs, so its names come from the manifests; Hugging Face and
// LM Studio encode the repository in the directory layout.

namespace ring0 {

class WeightStores {
public:
    std::wstring friendly_name(const std::wstring& path);

private:
    void refresh_ollama();

    std::unordered_map<std::wstring, std::wstring> ollama_;   // lowercase hex digest -> "name:tag"
    std::uint64_t ollama_tick_ = 0;
};

} // namespace ring0
