#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ui::assets {

struct Asset {
  const char* path;               // Request path (e.g., "/html/config.html")
  const std::uint8_t* data;       // Pointer to embedded bytes
  std::size_t size;               // Size of the embedded asset
  const char* content_type;       // MIME type for HTTP response
  bool compressed;                // True if gzip-compressed at build time
};

// Returns pointer to asset metadata or nullptr when not found.
const Asset* Find(std::string_view path);
const Asset* List();
std::size_t Count();

}  // namespace ui::assets
