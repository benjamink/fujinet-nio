// include/fujinet/image/output_format.h
#pragma once

#include "fujinet/image/image_pipeline.h"

#include <cstdint>
#include <string_view>
#include <vector>

// The output formats the Image translator can write, one entry per `fmt=`
// value. Adding a format means adding a writer and its entry in
// output_format.cpp; the selector parser and the translator need no change.
namespace fujinet::image {

struct OutputFormat {
    std::string_view name;      // the `fmt=` value
    int defaultBits;            // palette bits per channel when `bits` is absent
    int maxPens;                // highest base+colors the format can hold
    bool allowsBase;            // whether `base` (first pen) means anything
    std::vector<std::uint8_t> (*write)(const IndexedImage& image, const Options& o);
};

// The entry for `name`, or nullptr when there is none.
const OutputFormat* find_output_format(std::string_view name);

// The format used when the selector has no `fmt` key.
const OutputFormat& default_output_format();

} // namespace fujinet::image
