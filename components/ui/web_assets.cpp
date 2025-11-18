#include "ui/web_assets.hpp"

#include <algorithm>
#include <cstddef>

#include "web_assets_data.inc"

namespace ui::assets {

const Asset* Find(std::string_view path) {
  const auto begin = generated::kAssets;
  const auto end = generated::kAssets + generated::kAssetCount;
  const auto it = std::find_if(begin, end, [&](const Asset& asset) { return path == asset.path; });
  return it == end ? nullptr : it;
}

const Asset* List() {
  return generated::kAssets;
}

std::size_t Count() {
  return generated::kAssetCount;
}

}  // namespace ui::assets
