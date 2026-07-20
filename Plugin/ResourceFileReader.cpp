#include "ResourceFileReader.h"

#include "RE/Skyrim.h"

#include <format>

namespace ResourceFileReader
{
    bool Read(const std::string& resourcePath, std::string& output, std::string& failureReason,
              std::size_t maximumBytes)
    {
        output.clear();
        failureReason.clear();

        if (resourcePath.empty()) {
            failureReason = "resource path is empty";
            return false;
        }

        RE::BSResourceNiBinaryStream resource(resourcePath);
        if (!resource.good() || !resource.stream) {
            failureReason = "resource stream could not be opened";
            return false;
        }

        const auto size = static_cast<std::size_t>(resource.stream->totalSize);
        if (size == 0) {
            failureReason = "resource stream is empty";
            return false;
        }
        if (size > maximumBytes) {
            failureReason = std::format("resource is too large ({} bytes, maximum {})", size, maximumBytes);
            return false;
        }

        output.resize(size);
        std::uint64_t bytesRead = 0;
        const auto error = resource.stream->DoRead(output.data(), size, bytesRead);
        if (error != RE::BSResource::ErrorCode::kNone) {
            failureReason = std::format("resource read failed with error {}", static_cast<int>(error));
            output.clear();
            return false;
        }
        if (bytesRead != size) {
            failureReason = std::format("resource read was incomplete ({} of {} bytes)", bytesRead, size);
            output.clear();
            return false;
        }

        return true;
    }
}
