// include/fujinet/io/devices/image_writer_ilbm.h
#pragma once

#include "fujinet/io/devices/image_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// The `ilbm` writer: an IndexedImage as an IFF FORM ILBM with BMHD, CMAP and a
// ByteRun1-compressed BODY.
namespace fujinet::io::image {

// Bitplanes needed for pens 0..base+colors-1.
int planes_for(const Options& o);

// ByteRun1-compress one row.
std::vector<std::uint8_t> byterun1(const std::uint8_t* row, std::size_t n);

std::vector<std::uint8_t> write_ilbm(const IndexedImage& image, const Options& o);

} // namespace fujinet::io::image
