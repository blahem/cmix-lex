#ifndef R1_REORDER_TRANSFORM_H
#define R1_REORDER_TRANSFORM_H

#include <string>

namespace r1_reorder {

// Sorts PHDA9 metadata records by the same payload_lex key used in the tail
// experiments while keeping the page/body bytes in Kaido's article order. The
// raw side stream is written separately so it can be appended at ready-stream
// EOF and stripped before line-based decompression splitting.
bool ReorderPhda9MainFile(const std::string& path,
    const std::string& dictionary_path, const std::string& side_path);

// Applies the validated payload_lex permutation to regime 1 of the already
// dictionary-encoded cmix stream. This is deliberately post-WRT: doing it on
// .main_phda9prepr changes WRT's byte output and no longer matches the lpaq
// experiment.
bool ReorderEncodedTailFile(const std::string& path,
    const std::string& side_path);

bool AppendSideToFile(const std::string& path, const std::string& side_path);

bool ExtractSideFromFile(const std::string& path, const std::string& side_path);

// Restores the post-WRT tail permutation after arithmetic decode and before
// dictionary decode.
bool RestoreEncodedTailFile(const std::string& path,
    const std::string& side_path);

// Restores the PHDA9 file to its ordinary layout before phda9_resto() runs.
// This is required because PHDA9 restore expects metadata/lang chunks in body
// order, not payload_lex order.
bool RestorePhda9MainFile(const std::string& path,
    const std::string& side_path);

}  // namespace r1_reorder

#endif
