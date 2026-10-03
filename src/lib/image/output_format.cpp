// src/lib/image/output_format.cpp
#include "fujinet/image/output_format.h"

#include "fujinet/image/ilbm_writer.h"

namespace fujinet::image {

namespace {

// The first entry is the default format.
constexpr OutputFormat kOutputFormats[] = {
    {
        .name = "ilbm",
        .defaultBits = 4,                   // Amiga OCS: 12-bit colour
        .maxPens = ilbm::kMaxPens,
        .allowsBase = true,
        .write = ilbm::write,
    },
};

} // namespace

const OutputFormat* find_output_format(std::string_view name)
{
    for (const OutputFormat& format : kOutputFormats) {
        if (format.name == name) {
            return &format;
        }
    }
    return nullptr;
}

const OutputFormat& default_output_format()
{
    return kOutputFormats[0];
}

} // namespace fujinet::image
