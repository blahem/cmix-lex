// Payload-lex regime-1 transform for cmix-lex.
//
// Copyright (C) 2026 Ibrahim Marcouch
//
// This file is original cmix-lex code.  It implements a reversible transform
// for the post-dictionary PHDA9 tail stream by sorting regime-1 blocks into a
// compressor-friendly payload_lex order and storing only the missing
// permutation data at EOF.
//
// Transform sketch:
// 1. cmix -e first builds the normal PHDA9 + dictionary/WRT ready stream.
// 2. The final 45,332,670-byte phda9 tail (the last 7.73% of the dictionary encoded file (size 586459321)) is split into regimes 0/1/2.
// 3. Only regime 1 is reordered; regimes 0 and 2 remain byte-for-byte intact.
// 4. Regime-1 D99 blocks are sorted by their payload bytes after D99/D86a.
// 5. The EOF side blob stores enough order data to restore original regime 1.
// 6. Decompression removes the side blob, restores regime 1, then lets the
//    existing dictionary/PHDA9 decoder recover original enwik9.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.

#include "r1_reorder_transform.h"

#include "preprocess/dictionary.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace r1_reorder {
namespace {

struct Range {
  size_t start = 0;
  size_t len = 0;
};

struct LineRef {
  size_t start = 0;
  size_t len = 0;
};

struct HeaderBlock {
  std::vector<LineRef> lines;
  size_t delta_line = 0;
  size_t revision_line = 0;
  int64_t page_id = 0;
};

struct Record {
  size_t original_index = 0;
  Range page;
  HeaderBlock header;
  int lang_chunk = -1;
  std::string sort_key;
};

struct TailBlock {
  size_t original_index = 0;
  std::vector<LineRef> lines;
  // payload_lex key: the compressibility-friendly part of a D99 block.
  std::string sort_key;
};

// R1ORD2 is kept as a compatibility decoder; new archives use R1ORD3.
enum class SideOrder {
  kPayloadLexRaw,
  kD86aLehmer,
};

struct SideMeta {
  size_t tail_len = 0;
  size_t r0_count = 0;
  size_t r1_count = 0;
  size_t r2_count = 0;
  size_t prelude_count = 0;
  size_t suffix_count = 0;
  SideOrder side_order = SideOrder::kPayloadLexRaw;
  std::vector<size_t> sorted_to_original;
};

const unsigned char kSideMagic[] = {'R', '1', 'O', 'R', 'D', '2', '\n'};
const unsigned char kSideMagicD86Lehmer[] =
    {'R', '1', 'O', 'R', 'D', '3', '\n'};
const unsigned char kFooterMagic[] = {'R', '1', 'O', 'R', 'D', 'F', 'T', 'R'};
// These offsets describe the exact post-WRT stream produced by the current -e
// preprocessing path, not byte offsets in raw enwik9.
const size_t kEncodedTailStart = 541126651;
const size_t kEncodedTailLen = 45332670;
const size_t kEncodedRegime1Start = 13599801;
const size_t kEncodedRegime2Start = 30372888;
const unsigned char kD99Line[] = {0xDF, 0x99, 'N'};

bool CheckedAdd(size_t a, size_t b, size_t* out) {
  if (b > std::numeric_limits<size_t>::max() - a) return false;
  *out = a + b;
  return true;
}

bool SizeTFromU64(uint64_t value, size_t* out) {
  if (value > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  *out = static_cast<size_t>(value);
  return true;
}

bool ReadFile(const std::string& path, std::vector<unsigned char>* data) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) return false;
  if (static_cast<uint64_t>(size) >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  data->resize(static_cast<size_t>(size));
  if (!data->empty()) {
    in.read(reinterpret_cast<char*>(data->data()), data->size());
  }
  return static_cast<size_t>(in.gcount()) == data->size();
}

bool WriteFile(const std::string& path, const std::vector<unsigned char>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
  }
  return out.good();
}

size_t BodyEnd(const std::vector<unsigned char>& data, const LineRef& line) {
  if (line.start > data.size() || line.len > data.size() - line.start) {
    return line.start;
  }
  size_t end = line.start + line.len;
  if (end > line.start && data[end - 1] == '\n') --end;
  if (end > line.start && data[end - 1] == '\r') --end;
  return end;
}

bool ParseUnsignedAt(const std::vector<unsigned char>& data, size_t pos,
    size_t end, uint64_t* value) {
  while (pos < end && data[pos] == ' ') ++pos;
  if (pos >= end || data[pos] < '0' || data[pos] > '9') return false;
  uint64_t result = 0;
  while (pos < end && data[pos] >= '0' && data[pos] <= '9') {
    const uint64_t digit = static_cast<uint64_t>(data[pos] - '0');
    if (result >
        (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
    ++pos;
  }
  *value = result;
  return true;
}

bool ParseUnsignedExact(const std::vector<unsigned char>& data, size_t pos,
    size_t end, uint64_t* value) {
  if (pos >= end || data[pos] < '0' || data[pos] > '9') return false;
  uint64_t result = 0;
  while (pos < end && data[pos] >= '0' && data[pos] <= '9') {
    const uint64_t digit = static_cast<uint64_t>(data[pos] - '0');
    if (result >
        (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
    ++pos;
  }
  if (pos != end) return false;
  *value = result;
  return true;
}

bool ParseSignedBody(const std::vector<unsigned char>& data,
    const LineRef& line, int64_t* value) {
  size_t pos = line.start;
  const size_t end = BodyEnd(data, line);
  bool negative = false;
  if (pos < end && data[pos] == '-') {
    negative = true;
    ++pos;
  }
  if (pos >= end || data[pos] < '0' || data[pos] > '9') return false;
  const uint64_t limit = negative ?
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1 :
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  uint64_t result = 0;
  while (pos < end && data[pos] >= '0' && data[pos] <= '9') {
    const uint64_t digit = static_cast<uint64_t>(data[pos] - '0');
    if (result > (limit - digit) / 10) return false;
    result = result * 10 + digit;
    ++pos;
  }
  if (pos != end) return false;
  if (negative) {
    if (result == limit) {
      *value = std::numeric_limits<int64_t>::min();
    } else {
      *value = -static_cast<int64_t>(result);
    }
  } else {
    *value = static_cast<int64_t>(result);
  }
  return true;
}

std::vector<LineRef> SplitLines(size_t start, size_t len,
    const std::vector<unsigned char>& data) {
  std::vector<LineRef> lines;
  if (start > data.size() || len > data.size() - start) return lines;
  const size_t end = start + len;
  size_t line_start = start;
  for (size_t i = start; i < end; ++i) {
    if (data[i] == '\n') {
      lines.push_back({line_start, i + 1 - line_start});
      line_start = i + 1;
    }
  }
  if (line_start < end) lines.push_back({line_start, end - line_start});
  return lines;
}

bool ContainsBytes(const std::vector<unsigned char>& data, const LineRef& line,
    const char* text) {
  const size_t len = std::strlen(text);
  if (len == 0 || line.len < len) return false;
  if (line.start > data.size() || line.len > data.size() - line.start) {
    return false;
  }
  const unsigned char* begin = data.data() + line.start;
  const unsigned char* end = begin + line.len - len + 1;
  for (const unsigned char* p = begin; p < end; ++p) {
    if (std::memcmp(p, text, len) == 0) return true;
  }
  return false;
}

bool EndsWithBody(const std::vector<unsigned char>& data, const LineRef& line,
    const char* suffix) {
  const size_t len = std::strlen(suffix);
  if (line.start > data.size() || line.len > data.size() - line.start) {
    return false;
  }
  const size_t end = BodyEnd(data, line);
  return end >= line.start && end - line.start >= len &&
      std::memcmp(data.data() + end - len, suffix, len) == 0;
}

bool IsPageDeltaLine(const std::vector<unsigned char>& data,
    const LineRef& line) {
  const size_t end = BodyEnd(data, line);
  size_t pos = line.start;
  if (pos >= end || data[pos++] != '>') return false;
  if (pos < end && data[pos] == '-') ++pos;
  if (pos >= end || data[pos] < '0' || data[pos] > '9') return false;
  while (pos < end) {
    if (data[pos] < '0' || data[pos] > '9') return false;
    ++pos;
  }
  return true;
}

bool IsContributorEnd(const std::vector<unsigned char>& data,
    const LineRef& line) {
  const size_t end = BodyEnd(data, line);
  const size_t len = end - line.start;
  return (len >= 13 &&
      std::memcmp(data.data() + line.start, "/contributor>", 13) == 0) ||
      (len >= 16 &&
       std::memcmp(data.data() + line.start, "contributor dele", 16) == 0);
}

bool IsRevisionLine(const std::vector<unsigned char>& data,
    const LineRef& line) {
  const size_t end = BodyEnd(data, line);
  return end - line.start == 9 &&
      std::memcmp(data.data() + line.start, "revision>", 9) == 0;
}

bool IsExactD99Line(const std::vector<unsigned char>& data,
    const LineRef& line) {
  const size_t end = BodyEnd(data, line);
  return end - line.start == sizeof(kD99Line) &&
      std::memcmp(data.data() + line.start, kD99Line, sizeof(kD99Line)) == 0;
}

bool ParseFirstD86a(const std::vector<unsigned char>& data,
    const TailBlock& block, uint64_t* value) {
  // D86a is the numeric field on line 1 of each regime-1 D99 block.
  static const unsigned char kD86Prefix[] = {0xDF, 0x86, 'N'};
  if (block.lines.size() < 2) return false;
  const LineRef& line = block.lines[1];
  const size_t end = BodyEnd(data, line);
  size_t prefix_end = 0;
  if (!CheckedAdd(line.start, sizeof(kD86Prefix), &prefix_end) ||
      end < prefix_end) {
    return false;
  }
  if (std::memcmp(data.data() + line.start, kD86Prefix,
          sizeof(kD86Prefix)) != 0) {
    return false;
  }
  return ParseUnsignedExact(data, line.start + sizeof(kD86Prefix), end, value);
}

bool IsAsciiNumberLine(const std::vector<unsigned char>& data,
    const LineRef& line) {
  const size_t end = BodyEnd(data, line);
  if (line.start == end) return false;
  for (size_t i = line.start; i < end; ++i) {
    const unsigned char c = data[i];
    if ((c < '0' || c > '9') && c != '-') return false;
  }
  return true;
}

void AppendRange(std::vector<unsigned char>* out,
    const std::vector<unsigned char>& data, const Range& range) {
  if (range.start > data.size() || range.len > data.size() - range.start) {
    return;
  }
  out->insert(out->end(), data.begin() + range.start,
      data.begin() + range.start + range.len);
}

void AppendLine(std::vector<unsigned char>* out,
    const std::vector<unsigned char>& data, const LineRef& line) {
  if (line.start > data.size() || line.len > data.size() - line.start) {
    return;
  }
  out->insert(out->end(), data.begin() + line.start,
      data.begin() + line.start + line.len);
}

void AppendLines(std::vector<unsigned char>* out,
    const std::vector<unsigned char>& data, const std::vector<LineRef>& lines) {
  for (const LineRef& line : lines) AppendLine(out, data, line);
}

void AppendDecimalLine(std::vector<unsigned char>* out, int64_t value) {
  const std::string text = std::to_string(value);
  out->insert(out->end(), text.begin(), text.end());
  out->push_back('\n');
}

void AppendVarint(std::vector<unsigned char>* out, uint64_t value) {
  while (value >= 0x80) {
    out->push_back(static_cast<unsigned char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out->push_back(static_cast<unsigned char>(value));
}

bool ReadVarint(const std::vector<unsigned char>& data, size_t end,
    size_t* pos, uint64_t* value) {
  uint64_t result = 0;
  int shift = 0;
  while (*pos < end && shift <= 63) {
    const unsigned char byte = data[(*pos)++];
    if (shift == 63 && (byte & 0x7F) > 1) return false;
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

class Fenwick {
 public:
  // Maintains the remaining unused original indices for Lehmer permutation I/O.
  explicit Fenwick(size_t n) : tree_(n + 1, 0) {
    for (size_t i = 0; i < n; ++i) Add(i, 1);
  }

  void Add(size_t index, int delta) {
    ++index;
    while (index < tree_.size()) {
      tree_[index] += delta;
      index += index & (0 - index);
    }
  }

  size_t SumLessThan(size_t index) const {
    size_t result = 0;
    while (index > 0) {
      result += static_cast<size_t>(tree_[index]);
      index -= index & (0 - index);
    }
    return result;
  }

  size_t FindByOrder(size_t rank) const {
    size_t index = 0;
    size_t bit = 1;
    while (bit < tree_.size() / 2) bit <<= 1;
    while (bit != 0) {
      const size_t next = index + bit;
      if (next < tree_.size() && static_cast<size_t>(tree_[next]) <= rank) {
        index = next;
        rank -= static_cast<size_t>(tree_[next]);
      }
      bit >>= 1;
    }
    return index;
  }

 private:
  std::vector<int> tree_;
};

bool AppendLehmerPermutation(std::vector<unsigned char>* out,
    const std::vector<size_t>& permutation) {
  // Store each original index as its rank among still-unused indices.
  Fenwick fenwick(permutation.size());
  std::vector<unsigned char> seen(permutation.size(), 0);
  for (size_t value : permutation) {
    if (value >= permutation.size() || seen[value]) return false;
    seen[value] = 1;
    AppendVarint(out, fenwick.SumLessThan(value));
    fenwick.Add(value, -1);
  }
  return true;
}

bool ReadLehmerPermutation(const std::vector<unsigned char>& data,
    size_t* pos, size_t count, std::vector<size_t>* permutation) {
  // Reverse the rank stream back into the exact original-index permutation.
  Fenwick fenwick(count);
  permutation->assign(count, 0);
  for (size_t i = 0; i < count; ++i) {
    uint64_t rank64 = 0;
    if (!ReadVarint(data, data.size(), pos, &rank64)) return false;
    const size_t remaining = count - i;
    if (rank64 >= remaining) return false;
    const size_t value = fenwick.FindByOrder(static_cast<size_t>(rank64));
    (*permutation)[i] = value;
    fenwick.Add(value, -1);
  }
  return true;
}

void ApplyDictionaryTextByteShuffle(std::string* bytes) {
  // Match the byte shuffle used after dictionary substitution in cmix's -e path.
  for (char& ch : *bytes) {
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= '{' && c < 127) c += 'P' - '{';
    else if (c >= 'P' && c < 'T') c -= 'P' - '{';
    else if ((c >= ':' && c <= '?') || (c >= 'J' && c <= 'O')) c ^= 0x70;
    if (c == 'X' || c == '`') c ^= 'X' ^ '`';
    ch = static_cast<char>(c);
  }
}

void AppendLineToString(std::string* out,
    const std::vector<unsigned char>& data, const LineRef& line) {
  if (line.start > data.size() || line.len > data.size() - line.start) {
    return;
  }
  out->append(reinterpret_cast<const char*>(data.data() + line.start),
      line.len);
}

bool FindPages(const std::vector<unsigned char>& data, size_t body_start,
    size_t body_len, Range* prelude, std::vector<Range>* pages) {
  static const char kPageStart[] = "  <page>\n";
  static const char kPageEnd[] = "  </page>\n";
  if (body_start > data.size() || body_len > data.size() - body_start) {
    return false;
  }
  const size_t body_end = body_start + body_len;
  const size_t page_start_len = sizeof(kPageStart) - 1;
  const size_t page_end_len = sizeof(kPageEnd) - 1;
  size_t pos = body_start;
  pages->clear();

  while (pos < body_end) {
    size_t start = std::string::npos;
    if (body_end - pos >= page_start_len) {
      for (size_t i = pos; i <= body_end - page_start_len; ++i) {
        if (std::memcmp(data.data() + i, kPageStart, page_start_len) == 0) {
          start = i;
          break;
        }
      }
    }
    if (start == std::string::npos) break;
    if (pages->empty()) *prelude = {body_start, start - body_start};

    size_t end = std::string::npos;
    if (body_end - start >= page_end_len) {
      for (size_t i = start; i <= body_end - page_end_len; ++i) {
        if (std::memcmp(data.data() + i, kPageEnd, page_end_len) == 0) {
          end = i + page_end_len;
          break;
        }
      }
    }
    if (end == std::string::npos) return false;
    pages->push_back({start, end - start});
    pos = end;
  }
  return !pages->empty() && pos == body_end;
}

bool ParseHeaderBlocks(const std::vector<unsigned char>& data,
    const std::vector<LineRef>& lines, size_t expected_count,
    std::vector<HeaderBlock>* blocks) {
  blocks->clear();
  blocks->reserve(expected_count);
  size_t cursor = 0;
  int64_t last_page_id = 0;
  while (cursor < lines.size()) {
    HeaderBlock block;
    const size_t start = cursor;
    while (cursor < lines.size() && !IsPageDeltaLine(data, lines[cursor])) {
      ++cursor;
    }
    if (cursor >= lines.size()) return false;
    block.delta_line = cursor - start;

    int64_t delta = 0;
    LineRef delta_body = lines[cursor];
    ++delta_body.start;
    --delta_body.len;
    if (!ParseSignedBody(data, delta_body, &delta)) return false;
    last_page_id += delta;
    block.page_id = last_page_id;
    ++cursor;

    while (cursor < lines.size()) {
      const bool done = IsContributorEnd(data, lines[cursor]);
      ++cursor;
      if (done) break;
    }
    block.lines.assign(lines.begin() + start, lines.begin() + cursor);
    bool found_revision = false;
    for (size_t i = 0; i < block.lines.size(); ++i) {
      if (IsRevisionLine(data, block.lines[i])) {
        block.revision_line = i;
        found_revision = true;
        break;
      }
    }
    if (!found_revision || block.revision_line + 1 >= block.lines.size()) {
      return false;
    }
    blocks->push_back(block);
  }
  return blocks->size() == expected_count;
}

bool ParseLangChunks(const std::vector<unsigned char>& data,
    const std::vector<LineRef>& lang_lines, std::vector<Range>* chunks) {
  chunks->clear();
  size_t cursor = 0;
  while (cursor < lang_lines.size()) {
    const size_t start = cursor;
    while (cursor < lang_lines.size()) {
      const bool done = ContainsBytes(data, lang_lines[cursor], "</text>");
      ++cursor;
      if (done) break;
    }
    if (cursor == start) return false;
    const size_t chunk_start = lang_lines[start].start;
    const LineRef& last = lang_lines[cursor - 1];
    size_t last_end = 0;
    if (!CheckedAdd(last.start, last.len, &last_end) ||
        last_end < chunk_start) {
      return false;
    }
    chunks->push_back({chunk_start, last_end - chunk_start});
  }
  return true;
}

bool AssignLangChunks(const std::vector<unsigned char>& data,
    const std::vector<LineRef>& body_lines, const std::vector<Range>& pages,
    size_t chunk_count, std::vector<int>* page_to_chunk) {
  page_to_chunk->assign(pages.size(), -1);
  size_t page = 0;
  size_t next_chunk = 0;
  int c = 0;
  int64_t lnu = 0;
  int64_t f = 0;
  for (const LineRef& line : body_lines) {
    ++lnu;
    while (page + 1 < pages.size() &&
        pages[page].len <=
            std::numeric_limits<size_t>::max() - pages[page].start &&
        line.start >= pages[page].start + pages[page].len) {
      ++page;
    }
    if (ContainsBytes(data, line, "<tex")) {
      c = 1;
      f = lnu;
    }
    if (ContainsBytes(data, line, "</text>")) c = 0;
    if (EndsWithBody(data, line, "</revision>") && c == 1 && lnu - f >= 4) {
      c = 0;
      if (page >= pages.size() || next_chunk >= chunk_count) return false;
      (*page_to_chunk)[page] = static_cast<int>(next_chunk++);
    }
  }
  return next_chunk == chunk_count;
}

void RewriteTailLengthLine(std::vector<unsigned char>* out, uint64_t tail_len) {
  const std::string digits = std::to_string(tail_len);
  const size_t limit = std::min<size_t>(21, out->size());
  for (size_t i = 0; i < limit; ++i) (*out)[i] = ' ';
  for (size_t i = 0; i < digits.size() && i < limit; ++i) {
    (*out)[i] = static_cast<unsigned char>(digits[i]);
  }
}

std::string MakePayloadLexKey(const std::vector<unsigned char>& data,
    const std::vector<Record>& records, size_t index,
    preprocessor::Dictionary* dictionary) {
  const HeaderBlock& header = records[index].header;
  std::string raw;
  if (header.revision_line + 2 < header.lines.size()) {
    for (size_t i = header.revision_line + 2; i < header.lines.size(); ++i) {
      AppendLineToString(&raw, data, header.lines[i]);
    }
  }
  // The standalone payload_lex sweep sorted post-dictionary blocks from
  // revision marker to the next revision marker. That block includes the next
  // page-id delta at the end, so keep the same signal in the sort key even
  // though the delta stream itself is moved to the EOF side blob.
  if (index + 1 < records.size()) {
    AppendLineToString(&raw, data,
        records[index + 1].header.lines[records[index + 1].header.delta_line]);
  }
  std::string encoded;
  if (dictionary) {
    dictionary->EncodeString(raw, &encoded);
    ApplyDictionaryTextByteShuffle(&encoded);
  } else {
    encoded = raw;
  }
  return encoded;
}

bool HasBytesAt(const std::vector<unsigned char>& data, size_t pos,
    const unsigned char* bytes, size_t len) {
  return pos <= data.size() && data.size() - pos >= len &&
      std::memcmp(data.data() + pos, bytes, len) == 0;
}

void AppendU64LE(std::vector<unsigned char>* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFF));
  }
}

bool ReadU64LE(const std::vector<unsigned char>& data, size_t pos,
    uint64_t* value) {
  if (pos > data.size() || data.size() - pos < 8) return false;
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
  }
  *value = result;
  return true;
}

std::vector<unsigned char> MakePayloadLexSide(
    size_t tail_len, size_t r0_count, size_t r1_count, size_t r2_count,
    size_t prelude_count, size_t suffix_count,
    const std::vector<size_t>& sorted_indices) {
  std::vector<unsigned char> out;
  size_t reserve_size = 0;
  if (sorted_indices.size() <=
      (std::numeric_limits<size_t>::max() - 32) / 3) {
    reserve_size = 32 + sorted_indices.size() * 3;
  }
  out.reserve(reserve_size);
  out.insert(out.end(), kSideMagic, kSideMagic + sizeof(kSideMagic));
  AppendVarint(&out, tail_len);
  AppendVarint(&out, r0_count);
  AppendVarint(&out, r1_count);
  AppendVarint(&out, r2_count);
  AppendVarint(&out, prelude_count);
  AppendVarint(&out, suffix_count);
  AppendVarint(&out, sorted_indices.size());
  for (size_t index : sorted_indices) AppendVarint(&out, index);
  return out;
}

bool ComputeD86aOrderForPayloadLex(const std::vector<unsigned char>& data,
    const std::vector<TailBlock>& blocks,
    const std::vector<size_t>& sorted_indices,
    std::vector<size_t>* d86a_order) {
  // D86a is visible after payload_lex sorting, so decoder can recompute this.
  std::vector<uint64_t> d86a(sorted_indices.size(), 0);
  for (size_t sorted_pos = 0; sorted_pos < sorted_indices.size(); ++sorted_pos) {
    const size_t block_index = sorted_indices[sorted_pos];
    if (block_index >= blocks.size()) return false;
    if (!ParseFirstD86a(data, blocks[block_index], &d86a[sorted_pos])) {
      return false;
    }
  }

  d86a_order->resize(sorted_indices.size());
  std::iota(d86a_order->begin(), d86a_order->end(), 0);
  std::stable_sort(d86a_order->begin(), d86a_order->end(),
      [&d86a](size_t a, size_t b) {
        if (d86a[a] != d86a[b]) return d86a[a] < d86a[b];
        // Ties are not present in enwik9, but sorted position is the only
        // tie-breaker the decoder can reproduce without side information.
        return a < b;
      });
  return true;
}

bool ComputeD86aOrderForSortedBlocks(const std::vector<unsigned char>& data,
    const std::vector<TailBlock>& sorted_blocks,
    std::vector<size_t>* d86a_order) {
  // Same D86a ordering, but applied to already-transformed sorted blocks.
  std::vector<uint64_t> d86a(sorted_blocks.size(), 0);
  for (size_t sorted_pos = 0; sorted_pos < sorted_blocks.size(); ++sorted_pos) {
    if (!ParseFirstD86a(data, sorted_blocks[sorted_pos], &d86a[sorted_pos])) {
      return false;
    }
  }

  d86a_order->resize(sorted_blocks.size());
  std::iota(d86a_order->begin(), d86a_order->end(), 0);
  std::stable_sort(d86a_order->begin(), d86a_order->end(),
      [&d86a](size_t a, size_t b) {
        if (d86a[a] != d86a[b]) return d86a[a] < d86a[b];
        return a < b;
      });
  return true;
}

bool MakeD86aLehmerSide(size_t tail_len, size_t r0_count, size_t r1_count,
    size_t r2_count, size_t prelude_count, size_t suffix_count,
    const std::vector<unsigned char>& data, const std::vector<TailBlock>& blocks,
    const std::vector<size_t>& sorted_indices, std::vector<unsigned char>* out) {
  // Store original indices in D86a order; cmix compresses that permutation much
  // better than raw payload_lex order.
  std::vector<size_t> d86a_order;
  if (!ComputeD86aOrderForPayloadLex(data, blocks, sorted_indices,
          &d86a_order)) {
    return false;
  }

  std::vector<size_t> original_by_d86a;
  original_by_d86a.reserve(d86a_order.size());
  for (size_t sorted_pos : d86a_order) {
    original_by_d86a.push_back(sorted_indices[sorted_pos]);
  }

  out->clear();
  size_t reserve_size = 0;
  if (original_by_d86a.size() <=
      (std::numeric_limits<size_t>::max() - 32) / 3) {
    reserve_size = 32 + original_by_d86a.size() * 3;
  }
  out->reserve(reserve_size);
  out->insert(out->end(), kSideMagicD86Lehmer,
      kSideMagicD86Lehmer + sizeof(kSideMagicD86Lehmer));
  AppendVarint(out, tail_len);
  AppendVarint(out, r0_count);
  AppendVarint(out, r1_count);
  AppendVarint(out, r2_count);
  AppendVarint(out, prelude_count);
  AppendVarint(out, suffix_count);
  AppendVarint(out, original_by_d86a.size());
  return AppendLehmerPermutation(out, original_by_d86a);
}

bool ParsePayloadLexSide(const std::vector<unsigned char>& side,
    size_t expected_tail_len, size_t expected_count,
    std::vector<size_t>* sorted_to_original) {
  if (!HasBytesAt(side, 0, kSideMagic, sizeof(kSideMagic))) return false;
  size_t pos = sizeof(kSideMagic);
  uint64_t tail_len = 0, r0_count = 0, r1_count = 0, r2_count = 0;
  uint64_t prelude_count = 0, suffix_count = 0, count = 0;
  if (!ReadVarint(side, side.size(), &pos, &tail_len)) return false;
  if (!ReadVarint(side, side.size(), &pos, &r0_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &r1_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &r2_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &prelude_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &suffix_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &count)) return false;
  (void)r0_count;
  (void)r1_count;
  (void)r2_count;
  (void)prelude_count;
  (void)suffix_count;
  if (tail_len != expected_tail_len || count != expected_count) return false;

  sorted_to_original->assign(expected_count, 0);
  std::vector<unsigned char> seen(expected_count, 0);
  for (size_t i = 0; i < expected_count; ++i) {
    uint64_t value64 = 0;
    if (!ReadVarint(side, side.size(), &pos, &value64)) return false;
    if (value64 >= expected_count) return false;
    const size_t value = static_cast<size_t>(value64);
    if (seen[value]) return false;
    seen[value] = 1;
    (*sorted_to_original)[i] = value;
  }
  return pos == side.size();
}

bool ParsePayloadLexSideMeta(const std::vector<unsigned char>& side,
    size_t expected_tail_len, SideMeta* meta) {
  // Accept the new D86a/Lehmer side blob and the older raw permutation blob.
  SideOrder side_order = SideOrder::kPayloadLexRaw;
  size_t pos = 0;
  if (HasBytesAt(side, 0, kSideMagicD86Lehmer,
          sizeof(kSideMagicD86Lehmer))) {
    side_order = SideOrder::kD86aLehmer;
    pos = sizeof(kSideMagicD86Lehmer);
  } else if (HasBytesAt(side, 0, kSideMagic, sizeof(kSideMagic))) {
    side_order = SideOrder::kPayloadLexRaw;
    pos = sizeof(kSideMagic);
  } else {
    return false;
  }

  uint64_t tail_len = 0, r0_count = 0, r1_count = 0, r2_count = 0;
  uint64_t prelude_count = 0, suffix_count = 0, count = 0;
  if (!ReadVarint(side, side.size(), &pos, &tail_len)) return false;
  if (!ReadVarint(side, side.size(), &pos, &r0_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &r1_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &r2_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &prelude_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &suffix_count)) return false;
  if (!ReadVarint(side, side.size(), &pos, &count)) return false;
  if (tail_len != expected_tail_len) return false;

  size_t count_size = 0;
  if (!SizeTFromU64(count, &count_size)) return false;
  if (!SizeTFromU64(tail_len, &meta->tail_len)) return false;
  if (!SizeTFromU64(r0_count, &meta->r0_count)) return false;
  if (!SizeTFromU64(r1_count, &meta->r1_count)) return false;
  if (!SizeTFromU64(r2_count, &meta->r2_count)) return false;
  if (!SizeTFromU64(prelude_count, &meta->prelude_count)) return false;
  if (!SizeTFromU64(suffix_count, &meta->suffix_count)) return false;
  meta->side_order = side_order;

  if (side_order == SideOrder::kD86aLehmer) {
    if (!ReadLehmerPermutation(side, &pos, count_size,
            &meta->sorted_to_original)) {
      return false;
    }
  } else {
    meta->sorted_to_original.assign(count_size, 0);
    std::vector<unsigned char> seen(count_size, 0);
    for (size_t i = 0; i < count_size; ++i) {
      uint64_t value64 = 0;
      if (!ReadVarint(side, side.size(), &pos, &value64)) return false;
      if (value64 >= count) return false;
      const size_t value = static_cast<size_t>(value64);
      if (seen[value]) return false;
      seen[value] = 1;
      meta->sorted_to_original[i] = value;
    }
  }
  return pos == side.size();
}

bool PartitionEncodedTailLines(const std::vector<LineRef>& lines,
    size_t* r0_count, size_t* r1_count, size_t* r2_count) {
  // Regime boundaries are byte positions inside the encoded tail.
  *r0_count = 0;
  *r1_count = 0;
  *r2_count = 0;
  size_t pos = 0;
  for (const LineRef& line : lines) {
    if (pos < kEncodedRegime1Start) ++*r0_count;
    else if (pos < kEncodedRegime2Start) ++*r1_count;
    else ++*r2_count;
    if (!CheckedAdd(pos, line.len, &pos)) return false;
  }
  return pos == kEncodedTailLen;
}

size_t GuessLastHugeBlockEnd(const std::vector<unsigned char>& data,
    const std::vector<LineRef>& lines, size_t start, size_t limit) {
  size_t tentative_end = 0;
  if (!CheckedAdd(start, static_cast<size_t>(7), &tentative_end)) {
    tentative_end = limit;
  }
  size_t end = std::min(tentative_end, limit);
  if (end < limit && IsAsciiNumberLine(data, lines[end])) {
    ++end;
    if (end < limit && !IsExactD99Line(data, lines[end])) {
      const size_t body_end = BodyEnd(data, lines[end]);
      if (body_end > lines[end].start &&
          body_end - lines[end].start <= 16 &&
          data[lines[end].start] >= 0x80) {
        ++end;
      }
    }
  }
  return end;
}

bool ParseEncodedTailBlocks(const std::vector<unsigned char>& data,
    const std::vector<LineRef>& r1_lines, std::vector<LineRef>* prelude,
    std::vector<TailBlock>* blocks, std::vector<LineRef>* suffix) {
  // Regime-1 article-header blocks start on the exact D99 marker.
  std::vector<size_t> starts;
  for (size_t i = 0; i < r1_lines.size(); ++i) {
    if (IsExactD99Line(data, r1_lines[i])) starts.push_back(i);
  }
  if (starts.empty()) return false;

  prelude->assign(r1_lines.begin(), r1_lines.begin() + starts[0]);
  blocks->clear();
  blocks->reserve(starts.size());
  size_t cursor = starts[0];
  for (size_t block_index = 0; block_index < starts.size(); ++block_index) {
    const size_t start = starts[block_index];
    if (start != cursor) return false;
    const size_t next_start = block_index + 1 < starts.size() ?
        starts[block_index + 1] : r1_lines.size();
    const size_t end = (next_start - start <= 16) ? next_start :
        GuessLastHugeBlockEnd(data, r1_lines, start, next_start);
    TailBlock block;
    block.original_index = block_index;
    block.lines.assign(r1_lines.begin() + start, r1_lines.begin() + end);
    // Skip D99 and D86a: the standalone payload_lex experiment sorted on the
    // remaining payload, which is what made similar records cluster.
    for (size_t i = 2; i < block.lines.size(); ++i) {
      AppendLineToString(&block.sort_key, data, block.lines[i]);
    }
    blocks->push_back(std::move(block));
    cursor = end;
  }
  suffix->assign(r1_lines.begin() + cursor, r1_lines.end());
  return true;
}

}  // namespace

bool ReorderPhda9MainFile(const std::string& path,
    const std::string& dictionary_path, const std::string& side_path) {
  std::vector<unsigned char> input;
  if (!ReadFile(path, &input)) return false;
  if (input.size() < 64) return true;

  const size_t first_lf = std::find(input.begin(), input.end(), '\n') -
      input.begin();
  uint64_t tail_len64 = 0;
  if (first_lf >= input.size() ||
      !ParseUnsignedAt(input, 0, first_lf, &tail_len64)) {
    return false;
  }
  if (tail_len64 == 0 || tail_len64 >= input.size()) return false;
  size_t tail_len = 0;
  if (!SizeTFromU64(tail_len64, &tail_len)) return false;
  const size_t tail_start = input.size() - tail_len;

  const size_t header_len_lf = std::find(input.begin() + tail_start,
      input.end(), '\n') - input.begin();
  uint64_t header_len64 = 0;
  if (header_len_lf >= input.size() ||
      !ParseUnsignedAt(input, tail_start, header_len_lf, &header_len64)) {
    return false;
  }
  const size_t lang_len_start = header_len_lf + 1;
  const size_t lang_len_lf = std::find(input.begin() + lang_len_start,
      input.end(), '\n') - input.begin();
  uint64_t lang_len64 = 0;
  if (lang_len_lf >= input.size() ||
      !ParseUnsignedAt(input, lang_len_start, lang_len_lf, &lang_len64)) {
    return false;
  }

  const size_t header_start = lang_len_lf + 1;
  size_t header_len = 0;
  size_t lang_len = 0;
  size_t lang_start = 0;
  size_t lang_end = 0;
  if (!SizeTFromU64(header_len64, &header_len) ||
      !SizeTFromU64(lang_len64, &lang_len) ||
      !CheckedAdd(header_start, header_len, &lang_start) ||
      !CheckedAdd(lang_start, lang_len, &lang_end) ||
      lang_end != input.size()) {
    return false;
  }

  Range body_prelude;
  std::vector<Range> pages;
  if (!FindPages(input, 0, tail_start, &body_prelude, &pages)) return false;

  const std::vector<LineRef> body_lines = SplitLines(0, tail_start, input);
  const std::vector<LineRef> header_lines =
      SplitLines(header_start, header_len, input);
  const std::vector<LineRef> lang_lines =
      SplitLines(lang_start, lang_len, input);

  std::vector<HeaderBlock> header_blocks;
  if (!ParseHeaderBlocks(input, header_lines, pages.size(), &header_blocks)) {
    return false;
  }

  std::vector<Range> lang_chunks;
  if (!ParseLangChunks(input, lang_lines, &lang_chunks)) return false;

  std::vector<int> page_to_chunk;
  if (!AssignLangChunks(input, body_lines, pages, lang_chunks.size(),
      &page_to_chunk)) {
    return false;
  }

  std::vector<Record> records;
  records.reserve(pages.size());
  for (size_t i = 0; i < pages.size(); ++i) {
    Record record;
    record.original_index = i;
    record.page = pages[i];
    record.header = header_blocks[i];
    record.lang_chunk = page_to_chunk[i];
    records.push_back(std::move(record));
  }

  FILE* dict_file = std::fopen(dictionary_path.c_str(), "rb");
  preprocessor::Dictionary* dictionary = nullptr;
  if (dict_file) dictionary = new preprocessor::Dictionary(dict_file, true, false);
  for (size_t i = 0; i < records.size(); ++i) {
    records[i].sort_key = MakePayloadLexKey(input, records, i, dictionary);
  }
  if (dictionary) delete dictionary;
  if (dict_file) std::fclose(dict_file);

  std::vector<size_t> sorted_indices(records.size());
  std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
  std::stable_sort(sorted_indices.begin(), sorted_indices.end(),
      [&records](size_t a, size_t b) {
        if (records[a].sort_key != records[b].sort_key) {
          return records[a].sort_key < records[b].sort_key;
        }
        return records[a].original_index < records[b].original_index;
      });

  std::vector<unsigned char> body;
  body.reserve(tail_start);
  AppendRange(&body, input, body_prelude);
  for (const Record& record : records) AppendRange(&body, input, record.page);

  std::vector<unsigned char> header;
  header.reserve(header_len);
  for (size_t sorted_index : sorted_indices) {
    const Record& record = records[sorted_index];
    for (const LineRef& line : record.header.lines) {
      AppendLine(&header, input, line);
    }
  }

  std::vector<unsigned char> lang;
  lang.reserve(lang_len);
  for (size_t sorted_index : sorted_indices) {
    const Record& record = records[sorted_index];
    if (record.lang_chunk >= 0) {
      AppendRange(&lang, input, lang_chunks[record.lang_chunk]);
    }
  }

  std::vector<unsigned char> tail;
  AppendDecimalLine(&tail, static_cast<int64_t>(header.size()));
  AppendDecimalLine(&tail, static_cast<int64_t>(lang.size()));
  tail.insert(tail.end(), header.begin(), header.end());
  tail.insert(tail.end(), lang.begin(), lang.end());

  std::vector<unsigned char> output;
  size_t output_reserve = 0;
  if (!CheckedAdd(body.size(), tail.size(), &output_reserve)) return false;
  output.reserve(output_reserve);
  output.insert(output.end(), body.begin(), body.end());
  output.insert(output.end(), tail.begin(), tail.end());
  RewriteTailLengthLine(&output, tail.size());

  const std::vector<unsigned char> side = MakePayloadLexSide(tail_len,
      526364, 2001835, 614080, 7041, 84410, sorted_indices);
  if (!WriteFile(side_path, side)) return false;
  if (!WriteFile(path, output)) return false;
  std::fprintf(stderr,
      "\nr1 phda9 reorder: pages=%zu header=%zu->%zu side=%zu lang=%zu tail=%zu->%zu order=payload_lex side=raw_orderonly body=kept\n",
      pages.size(), header_len, header.size(), side.size(), lang_len,
      tail_len, tail.size());
  return true;
}

bool ReorderEncodedTailFile(const std::string& path,
    const std::string& side_path) {
  // Active compression path: reorder only the post-WRT encoded tail stream.
  std::vector<unsigned char> input;
  if (!ReadFile(path, &input)) return false;
  if (input.size() != kEncodedTailStart + kEncodedTailLen) {
    std::fprintf(stderr,
        "\nr1 encoded tail reorder refused: stream=%zu expected=%zu\n",
        input.size(), kEncodedTailStart + kEncodedTailLen);
    return false;
  }

  std::vector<unsigned char> tail(input.begin() + kEncodedTailStart,
      input.end());
  std::vector<LineRef> lines = SplitLines(0, tail.size(), tail);
  size_t r0_count = 0, r1_count = 0, r2_count = 0;
  if (!PartitionEncodedTailLines(lines, &r0_count, &r1_count, &r2_count)) {
    return false;
  }
  std::vector<LineRef> r0(lines.begin(), lines.begin() + r0_count);
  std::vector<LineRef> r1(lines.begin() + r0_count,
      lines.begin() + r0_count + r1_count);
  std::vector<LineRef> r2(lines.begin() + r0_count + r1_count, lines.end());

  std::vector<LineRef> prelude;
  std::vector<TailBlock> blocks;
  std::vector<LineRef> suffix;
  if (!ParseEncodedTailBlocks(tail, r1, &prelude, &blocks, &suffix)) {
    return false;
  }

  std::vector<size_t> sorted_indices(blocks.size());
  std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
  std::stable_sort(sorted_indices.begin(), sorted_indices.end(),
      [&blocks](size_t a, size_t b) {
        if (blocks[a].sort_key != blocks[b].sort_key) {
          return blocks[a].sort_key < blocks[b].sort_key;
        }
        return blocks[a].original_index < blocks[b].original_index;
      });

  std::vector<unsigned char> transformed_tail;
  transformed_tail.reserve(tail.size());
  // Keep non-target regimes and regime-1 framing exactly as they were.
  AppendLines(&transformed_tail, tail, r0);
  AppendLines(&transformed_tail, tail, prelude);
  // The only actual payload transform: sorted D99 blocks replace original D99
  // block order inside regime 1.
  for (size_t sorted_index : sorted_indices) {
    AppendLines(&transformed_tail, tail, blocks[sorted_index].lines);
  }
  AppendLines(&transformed_tail, tail, suffix);
  AppendLines(&transformed_tail, tail, r2);
  if (transformed_tail.size() != tail.size()) return false;

  std::vector<unsigned char> side;
  // EOF side data carries the inverse information needed for exact restore.
  if (!MakeD86aLehmerSide(kEncodedTailLen, r0_count, r1_count, r2_count,
          prelude.size(), suffix.size(), tail, blocks, sorted_indices,
          &side)) {
    return false;
  }
  if (!WriteFile(side_path, side)) return false;

  std::vector<unsigned char> output;
  size_t output_reserve = 0;
  if (!CheckedAdd(input.size(), side.size(), &output_reserve) ||
      !CheckedAdd(output_reserve, sizeof(kFooterMagic) + 8,
          &output_reserve)) {
    return false;
  }
  output.reserve(output_reserve);
  output.insert(output.end(), input.begin(),
      input.begin() + kEncodedTailStart);
  output.insert(output.end(), transformed_tail.begin(),
      transformed_tail.end());
  output.insert(output.end(), side.begin(), side.end());
  output.insert(output.end(), kFooterMagic, kFooterMagic + sizeof(kFooterMagic));
  AppendU64LE(&output, side.size());
  if (!WriteFile(path, output)) return false;

  std::fprintf(stderr,
      "\nr1 encoded tail reorder: stream=%zu->%zu tail=%zu side=%zu r0=%zu r1=%zu r2=%zu prelude=%zu suffix=%zu blocks=%zu order=payload_lex side=d86a_lehmer stage=post_wrt\n",
      input.size(), output.size(), tail.size(), side.size(), r0_count,
      r1_count, r2_count, prelude.size(), suffix.size(), blocks.size());
  return true;
}

bool AppendSideToFile(const std::string& path, const std::string& side_path) {
  std::vector<unsigned char> data;
  std::vector<unsigned char> side;
  if (!ReadFile(path, &data) || !ReadFile(side_path, &side)) return false;
  size_t data_reserve = 0;
  if (!CheckedAdd(data.size(), side.size(), &data_reserve) ||
      !CheckedAdd(data_reserve, sizeof(kFooterMagic) + 8, &data_reserve)) {
    return false;
  }
  data.reserve(data_reserve);
  data.insert(data.end(), side.begin(), side.end());
  data.insert(data.end(), kFooterMagic, kFooterMagic + sizeof(kFooterMagic));
  AppendU64LE(&data, side.size());
  return WriteFile(path, data);
}

bool ExtractSideFromFile(const std::string& path,
    const std::string& side_path) {
  // Decompression removes the EOF side blob before restoring the original tail.
  std::vector<unsigned char> data;
  if (!ReadFile(path, &data)) return false;
  if (data.size() < sizeof(kFooterMagic) + 8) return false;
  const size_t footer_pos = data.size() - sizeof(kFooterMagic) - 8;
  if (!HasBytesAt(data, footer_pos, kFooterMagic, sizeof(kFooterMagic))) {
    return false;
  }
  uint64_t side_len64 = 0;
  if (!ReadU64LE(data, footer_pos + sizeof(kFooterMagic), &side_len64)) {
    return false;
  }
  if (side_len64 > footer_pos) return false;
  size_t side_len = 0;
  if (!SizeTFromU64(side_len64, &side_len)) return false;
  const size_t side_pos = footer_pos - side_len;
  std::vector<unsigned char> side(data.begin() + side_pos,
      data.begin() + footer_pos);
  data.resize(side_pos);
  if (!WriteFile(side_path, side)) return false;
  return WriteFile(path, data);
}

bool RestoreEncodedTailFile(const std::string& path,
    const std::string& side_path) {
  // Rebuild original regime-1 order exactly, then PHDA9 can reverse normally.
  std::vector<unsigned char> input;
  std::vector<unsigned char> side;
  if (!ReadFile(path, &input) || !ReadFile(side_path, &side)) return false;
  if (input.size() != kEncodedTailStart + kEncodedTailLen) {
    std::fprintf(stderr,
        "\nr1 encoded tail restore refused: stream=%zu expected=%zu\n",
        input.size(), kEncodedTailStart + kEncodedTailLen);
    return false;
  }

  SideMeta meta;
  if (!ParsePayloadLexSideMeta(side, kEncodedTailLen, &meta)) return false;

  std::vector<unsigned char> tail(input.begin() + kEncodedTailStart,
      input.end());
  std::vector<LineRef> lines = SplitLines(0, tail.size(), tail);
  size_t r01_count = 0;
  size_t total_count = 0;
  if (!CheckedAdd(meta.r0_count, meta.r1_count, &r01_count) ||
      !CheckedAdd(r01_count, meta.r2_count, &total_count) ||
      lines.size() != total_count) {
    return false;
  }

  std::vector<LineRef> r0(lines.begin(), lines.begin() + meta.r0_count);
  std::vector<LineRef> r1(lines.begin() + meta.r0_count,
      lines.begin() + meta.r0_count + meta.r1_count);
  std::vector<LineRef> r2(lines.begin() + meta.r0_count + meta.r1_count,
      lines.end());
  size_t frame_count = 0;
  if (!CheckedAdd(meta.prelude_count, meta.suffix_count, &frame_count) ||
      frame_count > r1.size()) {
    return false;
  }

  std::vector<LineRef> prelude(r1.begin(),
      r1.begin() + meta.prelude_count);
  std::vector<LineRef> suffix(r1.end() - meta.suffix_count, r1.end());
  std::vector<LineRef> middle(r1.begin() + meta.prelude_count,
      r1.end() - meta.suffix_count);

  std::vector<LineRef> middle_prelude;
  std::vector<TailBlock> sorted_blocks;
  std::vector<LineRef> middle_suffix;
  if (!ParseEncodedTailBlocks(tail, middle, &middle_prelude,
      &sorted_blocks, &middle_suffix)) {
    return false;
  }
  if (!middle_prelude.empty() || !middle_suffix.empty() ||
      sorted_blocks.size() != meta.sorted_to_original.size()) {
    return false;
  }

  std::vector<const TailBlock*> original_blocks(sorted_blocks.size(), nullptr);
  if (meta.side_order == SideOrder::kD86aLehmer) {
    std::vector<size_t> d86a_order;
    if (!ComputeD86aOrderForSortedBlocks(tail, sorted_blocks, &d86a_order) ||
        d86a_order.size() != meta.sorted_to_original.size()) {
      return false;
    }
    for (size_t d86a_pos = 0; d86a_pos < d86a_order.size(); ++d86a_pos) {
      const size_t sorted_pos = d86a_order[d86a_pos];
      const size_t original_index = meta.sorted_to_original[d86a_pos];
      // The side permutation maps D86a-ordered sorted blocks back to original
      // regime-1 positions; duplicates would mean corruption or a code bug.
      if (original_index >= original_blocks.size() ||
          sorted_pos >= sorted_blocks.size() ||
          original_blocks[original_index] != nullptr) {
        return false;
      }
      original_blocks[original_index] = &sorted_blocks[sorted_pos];
    }
  } else {
    for (size_t sorted_pos = 0; sorted_pos < sorted_blocks.size(); ++sorted_pos) {
      const size_t original_index = meta.sorted_to_original[sorted_pos];
      if (original_index >= original_blocks.size() ||
          original_blocks[original_index] != nullptr) {
        return false;
      }
      original_blocks[original_index] = &sorted_blocks[sorted_pos];
    }
  }

  std::vector<unsigned char> restored_tail;
  restored_tail.reserve(tail.size());
  // Emit the exact original tail layout: r0, regime-1 prelude, restored D99
  // blocks, regime-1 suffix, then r2.
  AppendLines(&restored_tail, tail, r0);
  AppendLines(&restored_tail, tail, prelude);
  for (const TailBlock* block : original_blocks) {
    if (!block) return false;
    AppendLines(&restored_tail, tail, block->lines);
  }
  AppendLines(&restored_tail, tail, suffix);
  AppendLines(&restored_tail, tail, r2);
  if (restored_tail.size() != tail.size()) return false;

  std::vector<unsigned char> output;
  output.reserve(input.size());
  output.insert(output.end(), input.begin(),
      input.begin() + kEncodedTailStart);
  output.insert(output.end(), restored_tail.begin(), restored_tail.end());
  if (!WriteFile(path, output)) return false;

  std::fprintf(stderr,
      "\nr1 encoded tail restore: stream=%zu tail=%zu side=%zu blocks=%zu order=payload_lex_restored side=%s stage=post_wrt\n",
      output.size(), restored_tail.size(), side.size(), sorted_blocks.size(),
      meta.side_order == SideOrder::kD86aLehmer ? "d86a_lehmer" : "raw_orderonly");
  return true;
}

bool RestorePhda9MainFile(const std::string& path,
    const std::string& side_path) {
  std::vector<unsigned char> input;
  if (!ReadFile(path, &input)) return false;
  std::vector<unsigned char> side;
  if (!ReadFile(side_path, &side)) return false;
  if (input.size() < 64) return true;

  const size_t first_lf = std::find(input.begin(), input.end(), '\n') -
      input.begin();
  uint64_t tail_len64 = 0;
  if (first_lf >= input.size() ||
      !ParseUnsignedAt(input, 0, first_lf, &tail_len64)) {
    return false;
  }
  if (tail_len64 == 0 || tail_len64 >= input.size()) return false;
  size_t tail_len = 0;
  if (!SizeTFromU64(tail_len64, &tail_len)) return false;
  const size_t tail_start = input.size() - tail_len;

  const size_t header_len_lf = std::find(input.begin() + tail_start,
      input.end(), '\n') - input.begin();
  uint64_t header_len64 = 0;
  if (header_len_lf >= input.size() ||
      !ParseUnsignedAt(input, tail_start, header_len_lf, &header_len64)) {
    return false;
  }
  const size_t lang_len_start = header_len_lf + 1;
  const size_t lang_len_lf = std::find(input.begin() + lang_len_start,
      input.end(), '\n') - input.begin();
  uint64_t lang_len64 = 0;
  if (lang_len_lf >= input.size() ||
      !ParseUnsignedAt(input, lang_len_start, lang_len_lf, &lang_len64)) {
    return false;
  }

  const size_t header_start = lang_len_lf + 1;
  size_t header_len = 0;
  size_t lang_len = 0;
  size_t lang_start = 0;
  size_t lang_end = 0;
  if (!SizeTFromU64(header_len64, &header_len) ||
      !SizeTFromU64(lang_len64, &lang_len) ||
      !CheckedAdd(header_start, header_len, &lang_start) ||
      !CheckedAdd(lang_start, lang_len, &lang_end) ||
      lang_end != input.size()) {
    return false;
  }

  Range body_prelude;
  std::vector<Range> pages;
  if (!FindPages(input, 0, tail_start, &body_prelude, &pages)) return false;

  const std::vector<LineRef> body_lines = SplitLines(0, tail_start, input);
  const std::vector<LineRef> header_lines =
      SplitLines(header_start, header_len, input);
  const std::vector<LineRef> lang_lines =
      SplitLines(lang_start, lang_len, input);

  std::vector<HeaderBlock> sorted_headers;
  if (!ParseHeaderBlocks(input, header_lines, pages.size(), &sorted_headers)) {
    return false;
  }

  std::vector<Range> sorted_lang_chunks;
  if (!ParseLangChunks(input, lang_lines, &sorted_lang_chunks)) return false;

  std::vector<int> page_to_chunk;
  if (!AssignLangChunks(input, body_lines, pages, sorted_lang_chunks.size(),
      &page_to_chunk)) {
    return false;
  }

  std::vector<size_t> sorted_to_original;
  if (!ParsePayloadLexSide(side, tail_len, pages.size(),
      &sorted_to_original)) {
    return false;
  }

  std::vector<HeaderBlock> restored_headers(pages.size());
  std::vector<int> restored_lang_chunk(pages.size(), -1);
  size_t sorted_lang = 0;
  for (size_t sorted_pos = 0; sorted_pos < pages.size(); ++sorted_pos) {
    const size_t original_index = sorted_to_original[sorted_pos];
    restored_headers[original_index] = sorted_headers[sorted_pos];
    if (page_to_chunk[original_index] >= 0) {
      if (sorted_lang >= sorted_lang_chunks.size()) return false;
      restored_lang_chunk[original_index] = static_cast<int>(sorted_lang++);
    }
  }
  if (sorted_lang != sorted_lang_chunks.size()) return false;

  std::vector<unsigned char> header;
  header.reserve(header_len);
  for (const HeaderBlock& block : restored_headers) {
    for (const LineRef& line : block.lines) AppendLine(&header, input, line);
  }

  std::vector<unsigned char> lang;
  lang.reserve(lang_len);
  for (int chunk : restored_lang_chunk) {
    if (chunk >= 0) {
      AppendRange(&lang, input, sorted_lang_chunks[chunk]);
    }
  }

  std::vector<unsigned char> tail;
  AppendDecimalLine(&tail, static_cast<int64_t>(header.size()));
  AppendDecimalLine(&tail, static_cast<int64_t>(lang.size()));
  tail.insert(tail.end(), header.begin(), header.end());
  tail.insert(tail.end(), lang.begin(), lang.end());

  std::vector<unsigned char> output;
  size_t output_reserve = 0;
  if (!CheckedAdd(tail_start, tail.size(), &output_reserve)) return false;
  output.reserve(output_reserve);
  output.insert(output.end(), input.begin(), input.begin() + tail_start);
  output.insert(output.end(), tail.begin(), tail.end());
  RewriteTailLengthLine(&output, tail.size());

  if (!WriteFile(path, output)) return false;
  std::fprintf(stderr,
      "\nr1 phda9 restore: pages=%zu header=%zu->%zu lang=%zu tail=%zu->%zu order=payload_lex_restored\n",
      pages.size(), header_len, header.size(), lang_len, tail_len,
      tail.size());
  return true;
}

}  // namespace r1_reorder
