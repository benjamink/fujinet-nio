// include/fujinet/image/ilbm_writer.h
#pragma once

#include "fujinet/image/image_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// The `ilbm` output format: an IndexedImage as an IFF FORM ILBM with BMHD,
// CMAP and a ByteRun1-compressed BODY.
namespace fujinet::image::ilbm {

// Pens the writer supports: five bitplanes, the Amiga OCS low-resolution limit.
constexpr int kMaxPens = 32;

// Bitplanes needed for pens 0..base+colors-1.
int planes_for(const Options& o);

// ByteRun1-compress one row.
std::vector<std::uint8_t> byterun1(const std::uint8_t* row, std::size_t n);

std::vector<std::uint8_t> write(const IndexedImage& image, const Options& o);

} // namespace fujinet::image::ilbm
