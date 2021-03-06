// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cmap.h"

#include <set>
#include <utility>
#include <vector>

#include "maxp.h"
#include "os2.h"

// cmap - Character To Glyph Index Mapping Table
// http://www.microsoft.com/opentype/otspec/cmap.htm

namespace {

struct CMAPSubtableHeader {
  uint16_t platform;
  uint16_t encoding;
  uint32_t offset;
  uint16_t format;
  uint32_t length;
};

struct Subtable314Range {
  uint16_t start_range;
  uint16_t end_range;
  int16_t id_delta;
  uint16_t id_range_offset;
  uint32_t id_range_offset_offset;
};

// The maximum number of groups in format 12 or 13 subtables.
// Note: 0xFFFF is the maximum number of glyphs in a single font file.
const unsigned kMaxCMAPGroups = 0xFFFF;

// Glyph array size for the Mac Roman (format 0) table.
const size_t kFormat0ArraySize = 256;

// The upper limit of the Unicode code point.
const uint32_t kUnicodeUpperLimit = 0x10FFFF;

// Parses either 3.0.4 or 3.1.4 tables.
bool Parse3x4(ots::OpenTypeFile *file, int encoding,
              const uint8_t *data, size_t length, uint16_t num_glyphs) {
  ots::Buffer subtable(data, length);

  // 3.0.4 or 3.1.4 subtables are complex and, rather than expanding the
  // whole thing and recompacting it, we validate it and include it verbatim
  // in the output.

  if (!file->os2) {
    return OTS_FAILURE();
  }

  if (!subtable.Skip(4)) {
    return OTS_FAILURE();
  }
  uint16_t language = 0;
  if (!subtable.ReadU16(&language)) {
    return OTS_FAILURE();
  }
  if (language) {
    // Platform ID 3 (windows) subtables should have language '0'.
    return OTS_FAILURE();
  }

  uint16_t segcountx2, search_range, entry_selector, range_shift;
  segcountx2 = search_range = entry_selector = range_shift = 0;
  if (!subtable.ReadU16(&segcountx2) ||
      !subtable.ReadU16(&search_range) ||
      !subtable.ReadU16(&entry_selector) ||
      !subtable.ReadU16(&range_shift)) {
    return OTS_FAILURE();
  }

  if (segcountx2 & 1 || search_range & 1) {
    return OTS_FAILURE();
  }
  const uint16_t segcount = segcountx2 >> 1;
  // There must be at least one segment according the spec.
  if (segcount < 1) {
    return OTS_FAILURE();
  }

  // log2segcount is the maximal x s.t. 2^x < segcount
  unsigned log2segcount = 0;
  while (1u << (log2segcount + 1) <= segcount) {
    log2segcount++;
  }

  const uint16_t expected_search_range = 2 * 1u << log2segcount;
  if (expected_search_range != search_range) {
    return OTS_FAILURE();
  }

  if (entry_selector != log2segcount) {
    return OTS_FAILURE();
  }

  const uint16_t expected_range_shift = segcountx2 - search_range;
  if (range_shift != expected_range_shift) {
    return OTS_FAILURE();
  }

  std::vector<Subtable314Range> ranges(segcount);

  for (unsigned i = 0; i < segcount; ++i) {
    if (!subtable.ReadU16(&ranges[i].end_range)) {
      return OTS_FAILURE();
    }
  }

  uint16_t padding;
  if (!subtable.ReadU16(&padding)) {
    return OTS_FAILURE();
  }
  if (padding) {
    return OTS_FAILURE();
  }

  for (unsigned i = 0; i < segcount; ++i) {
    if (!subtable.ReadU16(&ranges[i].start_range)) {
      return OTS_FAILURE();
    }
  }
  for (unsigned i = 0; i < segcount; ++i) {
    if (!subtable.ReadS16(&ranges[i].id_delta)) {
      return OTS_FAILURE();
    }
  }
  for (unsigned i = 0; i < segcount; ++i) {
    ranges[i].id_range_offset_offset = subtable.offset();
    if (!subtable.ReadU16(&ranges[i].id_range_offset)) {
      return OTS_FAILURE();
    }

    if (ranges[i].id_range_offset & 1) {
      // Some font generators seem to put 65535 on id_range_offset
      // for 0xFFFF-0xFFFF range.
      // (e.g., many fonts in http://www.princexml.com/fonts/)
      if (i == segcount - 1u) {
        OTS_WARNING("bad id_range_offset");
        ranges[i].id_range_offset = 0;
        // The id_range_offset value in the transcoded font will not change
        // since this table is not actually "transcoded" yet.
      } else {
        return OTS_FAILURE();
      }
    }
  }

  // ranges must be ascending order, based on the end_code. Ranges may not
  // overlap.
  for (unsigned i = 1; i < segcount; ++i) {
    if ((i == segcount - 1u) &&
        (ranges[i - 1].start_range == 0xffff) &&
        (ranges[i - 1].end_range == 0xffff) &&
        (ranges[i].start_range == 0xffff) &&
        (ranges[i].end_range == 0xffff)) {
      // Some fonts (e.g., Germania.ttf) have multiple 0xffff terminators.
      // We'll accept them as an exception.
      OTS_WARNING("multiple 0xffff terminators found");
      continue;
    }

    // Note: some Linux fonts (e.g., LucidaSansOblique.ttf, bsmi00lp.ttf) have
    // unsorted table...
    if (ranges[i].end_range <= ranges[i - 1].end_range) {
      return OTS_FAILURE();
    }
    if (ranges[i].start_range <= ranges[i - 1].end_range) {
      return OTS_FAILURE();
    }

    // On many fonts, the value of {first, last}_char_index are incorrect.
    // Fix them.
    if (file->os2->first_char_index != 0xFFFF &&
        ranges[i].start_range != 0xFFFF &&
        file->os2->first_char_index > ranges[i].start_range) {
      file->os2->first_char_index = ranges[i].start_range;
    }
    if (file->os2->last_char_index != 0xFFFF &&
        ranges[i].end_range != 0xFFFF &&
        file->os2->last_char_index < ranges[i].end_range) {
      file->os2->last_char_index = ranges[i].end_range;
    }
  }

  // The last range must end at 0xffff
  if (ranges[segcount - 1].end_range != 0xffff) {
    return OTS_FAILURE();
  }

  // A format 4 CMAP subtable is complex. To be safe we simulate a lookup of
  // each code-point defined in the table and make sure that they are all valid
  // glyphs and that we don't access anything out-of-bounds.
  for (unsigned i = 1; i < segcount; ++i) {
    for (unsigned cp = ranges[i].start_range; cp <= ranges[i].end_range; ++cp) {
      const uint16_t code_point = cp;
      if (ranges[i].id_range_offset == 0) {
        // this is explictly allowed to overflow in the spec
        const uint16_t glyph = code_point + ranges[i].id_delta;
        if (glyph >= num_glyphs) {
          return OTS_FAILURE();
        }
      } else {
        const uint16_t range_delta = code_point - ranges[i].start_range;
        // this might seem odd, but it's true. The offset is relative to the
        // location of the offset value itself.
        const uint32_t glyph_id_offset = ranges[i].id_range_offset_offset +
                                         ranges[i].id_range_offset +
                                         range_delta * 2;
        // We need to be able to access a 16-bit value from this offset
        if (glyph_id_offset + 1 >= length) {
          return OTS_FAILURE();
        }
        uint16_t glyph;
        memcpy(&glyph, data + glyph_id_offset, 2);
        glyph = ntohs(glyph);
        if (glyph >= num_glyphs) {
          return OTS_FAILURE();
        }
      }
    }
  }

  // We accept the table.
  // TODO(yusukes): transcode the subtable.
  if (encoding == 0) {
    file->cmap->subtable_3_0_4_data = data;
    file->cmap->subtable_3_0_4_length = length;
  } else if (encoding == 1) {
    file->cmap->subtable_3_1_4_data = data;
    file->cmap->subtable_3_1_4_length = length;
  } else {
    return OTS_FAILURE();
  }

  return true;
}

bool Parse31012(ots::OpenTypeFile *file,
                const uint8_t *data, size_t length, uint16_t num_glyphs) {
  ots::Buffer subtable(data, length);

  // Format 12 tables are simple. We parse these and fully serialise them
  // later.

  if (!subtable.Skip(8)) {
    return OTS_FAILURE();
  }
  uint32_t language = 0;
  if (!subtable.ReadU32(&language)) {
    return OTS_FAILURE();
  }
  if (language) {
    return OTS_FAILURE();
  }

  uint32_t num_groups = 0;
  if (!subtable.ReadU32(&num_groups)) {
    return OTS_FAILURE();
  }
  if (num_groups == 0 || num_groups > kMaxCMAPGroups) {
    return OTS_FAILURE();
  }

  std::vector<ots::OpenTypeCMAPSubtableRange> &groups
      = file->cmap->subtable_3_10_12;
  groups.resize(num_groups);

  for (unsigned i = 0; i < num_groups; ++i) {
    if (!subtable.ReadU32(&groups[i].start_range) ||
        !subtable.ReadU32(&groups[i].end_range) ||
        !subtable.ReadU32(&groups[i].start_glyph_id)) {
      return OTS_FAILURE();
    }

    if (groups[i].start_range > kUnicodeUpperLimit ||
        groups[i].end_range > kUnicodeUpperLimit ||
        groups[i].start_glyph_id > 0xFFFF) {
      return OTS_FAILURE();
    }

    // [0xD800, 0xDFFF] are surrogate code points.
    if (groups[i].start_range >= 0xD800 &&
        groups[i].start_range <= 0xDFFF) {
      return OTS_FAILURE();
    }
    if (groups[i].end_range >= 0xD800 &&
        groups[i].end_range <= 0xDFFF) {
      return OTS_FAILURE();
    }
    if (groups[i].start_range < 0xD800 &&
        groups[i].end_range > 0xDFFF) {
      return OTS_FAILURE();
    }

    // We assert that the glyph value is within range. Because of the range
    // limits, above, we don't need to worry about overflow.
    if (groups[i].end_range < groups[i].start_range) {
      return OTS_FAILURE();
    }
    if ((groups[i].end_range - groups[i].start_range) +
        groups[i].start_glyph_id > num_glyphs) {
      return OTS_FAILURE();
    }
  }

  // the groups must be sorted by start code and may not overlap
  for (unsigned i = 1; i < num_groups; ++i) {
    if (groups[i].start_range <= groups[i - 1].start_range) {
      return OTS_FAILURE();
    }
    if (groups[i].start_range <= groups[i - 1].end_range) {
      return OTS_FAILURE();
    }
  }

  return true;
}

bool Parse31013(ots::OpenTypeFile *file,
                const uint8_t *data, size_t length, uint16_t num_glyphs) {
  ots::Buffer subtable(data, length);

  // Format 13 tables are simple. We parse these and fully serialise them
  // later.

  if (!subtable.Skip(8)) {
    return OTS_FAILURE();
  }
  uint16_t language = 0;
  if (!subtable.ReadU16(&language)) {
    return OTS_FAILURE();
  }
  if (language) {
    return OTS_FAILURE();
  }

  uint32_t num_groups = 0;
  if (!subtable.ReadU32(&num_groups)) {
    return OTS_FAILURE();
  }

  // We limit the number of groups in the same way as in 3.10.12 tables. See
  // the comment there in
  if (num_groups == 0 || num_groups > kMaxCMAPGroups) {
    return OTS_FAILURE();
  }

  std::vector<ots::OpenTypeCMAPSubtableRange> &groups
      = file->cmap->subtable_3_10_13;
  groups.resize(num_groups);

  for (unsigned i = 0; i < num_groups; ++i) {
    if (!subtable.ReadU32(&groups[i].start_range) ||
        !subtable.ReadU32(&groups[i].end_range) ||
        !subtable.ReadU32(&groups[i].start_glyph_id)) {
      return OTS_FAILURE();
    }

    // We conservatively limit all of the values to protect some parsers from
    // overflows
    if (groups[i].start_range > kUnicodeUpperLimit ||
        groups[i].end_range > kUnicodeUpperLimit ||
        groups[i].start_glyph_id > 0xFFFF) {
      return OTS_FAILURE();
    }

    if (groups[i].start_glyph_id >= num_glyphs) {
      return OTS_FAILURE();
    }
  }

  // the groups must be sorted by start code and may not overlap
  for (unsigned i = 1; i < num_groups; ++i) {
    if (groups[i].start_range <= groups[i - 1].start_range) {
      return OTS_FAILURE();
    }
    if (groups[i].start_range <= groups[i - 1].end_range) {
      return OTS_FAILURE();
    }
  }

  return true;
}

// Parses 0.5.14 tables.
bool Parse0514(ots::OpenTypeFile *file,
               const uint8_t *data, size_t length, uint16_t num_glyphs) {
  ots::Buffer subtable(data, length);

  // Format 14 subtables are a bit complex, so rather than rebuilding the
  // entire thing, we validate it and then include it verbatim in the output.

  const off_t offset_var_selector_records = 10;
  const size_t size_of_var_selector_record = 11;

  if (!subtable.Skip(6)) { // skip format and length
    return OTS_FAILURE();
  }
  uint32_t num_var_selector_records = 0;
  if (!subtable.ReadU32(&num_var_selector_records)) {
    return OTS_FAILURE();
  }
  if ((length - offset_var_selector_records) / size_of_var_selector_record <
      num_var_selector_records) {
    return OTS_FAILURE();
  }

  uint32_t prev_var_selector = 0;
  for (uint32_t i = 0; i < num_var_selector_records; i++) {
    uint32_t var_selector = 0, def_uvs_offset = 0, non_def_uvs_offset = 0;
    if (!subtable.ReadU24(&var_selector) ||
        !subtable.ReadU32(&def_uvs_offset) ||
        !subtable.ReadU32(&non_def_uvs_offset)) {
      return OTS_FAILURE();
    }
    if (var_selector <= prev_var_selector ||
        var_selector > kUnicodeUpperLimit ||
        def_uvs_offset > length - 4 ||
        non_def_uvs_offset > length - 4) {
      return OTS_FAILURE();
    }
    prev_var_selector = var_selector;

    if (def_uvs_offset) {
      ots::Buffer uvs_table(data + def_uvs_offset, length - def_uvs_offset);
      uint32_t num_unicode_value_ranges = 0;
      if (!uvs_table.ReadU32(&num_unicode_value_ranges)) {
        return OTS_FAILURE();
      }

      uint32_t prev_end_unicode = 0;
      for (uint32_t j = 0; j < num_unicode_value_ranges; j++) {
        uint32_t start_unicode = 0, end_unicode;
        uint8_t additional = 0;
        if (!uvs_table.ReadU24(&start_unicode) ||
            !uvs_table.ReadU8(&additional)) {
          return OTS_FAILURE();
        }
        end_unicode = start_unicode + additional;
        if ((j > 0 && start_unicode <= prev_end_unicode) ||
            end_unicode > kUnicodeUpperLimit) {
          return OTS_FAILURE();
        }
        prev_end_unicode = end_unicode;
      }
    }

    if (non_def_uvs_offset) {
      ots::Buffer uvs_table(data + non_def_uvs_offset,
                            length - non_def_uvs_offset);
      uint32_t num_uvs_mappings = 0;
      if (!uvs_table.ReadU32(&num_uvs_mappings)) {
        return OTS_FAILURE();
      }

      uint32_t prev_unicode = 0;
      for (uint32_t j = 0; j < num_uvs_mappings; j++) {
        uint32_t unicode_value = 0;
        if (!uvs_table.ReadU24(&unicode_value)) {
          return OTS_FAILURE();
        }
        if ((j > 0 && unicode_value <= prev_unicode) ||
            unicode_value > kUnicodeUpperLimit) {
          return OTS_FAILURE();
        }
        uint16_t glyph = 0;
        if (!uvs_table.ReadU16(&glyph)) {
          return OTS_FAILURE();
        }
        if (glyph >= num_glyphs) {
          return OTS_FAILURE();
        }
        prev_unicode = unicode_value;
      }
    }
  }

  // We accept the table.
  // TODO: transcode the subtable.
  file->cmap->subtable_0_5_14_data = data;
  file->cmap->subtable_0_5_14_length = length;

  return true;
}

bool Parse100(ots::OpenTypeFile *file, const uint8_t *data, size_t length) {
  // Mac Roman table
  ots::Buffer subtable(data, length);

  if (!subtable.Skip(4)) {
    return OTS_FAILURE();
  }
  uint16_t language = 0;
  if (!subtable.ReadU16(&language)) {
    return OTS_FAILURE();
  }
  if (language) {
    // simsun.ttf has non-zero language id.
    OTS_WARNING("language id should be zero: %u", language);
  }

  file->cmap->subtable_1_0_0.reserve(kFormat0ArraySize);
  for (size_t i = 0; i < kFormat0ArraySize; ++i) {
    uint8_t glyph_id = 0;
    if (!subtable.ReadU8(&glyph_id)) {
      return OTS_FAILURE();
    }
    file->cmap->subtable_1_0_0.push_back(glyph_id);
  }

  return true;
}

}  // namespace

namespace ots {

bool ots_cmap_parse(OpenTypeFile *file, const uint8_t *data, size_t length) {
  Buffer table(data, length);
  file->cmap = new OpenTypeCMAP;

  uint16_t version = 0;
  uint16_t num_tables = 0;
  if (!table.ReadU16(&version) ||
      !table.ReadU16(&num_tables)) {
    return OTS_FAILURE();
  }

  if (version != 0) {
    return OTS_FAILURE();
  }
  if (!num_tables) {
    return OTS_FAILURE();
  }

  std::vector<CMAPSubtableHeader> subtable_headers;

  // read the subtable headers
  subtable_headers.reserve(num_tables);
  for (unsigned i = 0; i < num_tables; ++i) {
    CMAPSubtableHeader subt;

    if (!table.ReadU16(&subt.platform) ||
        !table.ReadU16(&subt.encoding) ||
        !table.ReadU32(&subt.offset)) {
      return OTS_FAILURE();
    }

    subtable_headers.push_back(subt);
  }

  const size_t data_offset = table.offset();

  // make sure that all the offsets are valid.
  uint32_t last_id = 0;
  for (unsigned i = 0; i < num_tables; ++i) {
    if (subtable_headers[i].offset > 1024 * 1024 * 1024) {
      return OTS_FAILURE();
    }
    if (subtable_headers[i].offset < data_offset ||
        subtable_headers[i].offset >= length) {
      return OTS_FAILURE();
    }

    // check if the table is sorted first by platform ID, then by encoding ID.
    uint32_t current_id
        = (subtable_headers[i].platform << 16) + subtable_headers[i].encoding;
    if ((i != 0) && (last_id >= current_id)) {
      return OTS_FAILURE();
    }
    last_id = current_id;
  }

  // the format of the table is the first couple of bytes in the table. The
  // length of the table is stored in a format-specific way.
  for (unsigned i = 0; i < num_tables; ++i) {
    table.set_offset(subtable_headers[i].offset);
    if (!table.ReadU16(&subtable_headers[i].format)) {
      return OTS_FAILURE();
    }

    uint16_t len = 0;
    switch (subtable_headers[i].format) {
      case 0:
      case 4:
        if (!table.ReadU16(&len)) {
          return OTS_FAILURE();
        }
        subtable_headers[i].length = len;
        break;
      case 12:
      case 13:
        if (!table.Skip(2)) {
          return OTS_FAILURE();
        }
        if (!table.ReadU32(&subtable_headers[i].length)) {
          return OTS_FAILURE();
        }
        break;
      case 14:
        if (!table.ReadU32(&subtable_headers[i].length)) {
          return OTS_FAILURE();
        }
        break;
      default:
        subtable_headers[i].length = 0;
        break;
    }
  }

  // Now, verify that all the lengths are sane
  for (unsigned i = 0; i < num_tables; ++i) {
    if (!subtable_headers[i].length) continue;
    if (subtable_headers[i].length > 1024 * 1024 * 1024) {
      return OTS_FAILURE();
    }
    // We know that both the offset and length are < 1GB, so the following
    // addition doesn't overflow
    const uint32_t end_byte
        = subtable_headers[i].offset + subtable_headers[i].length;
    if (end_byte > length) {
      return OTS_FAILURE();
    }
  }

  // check that the cmap subtables are not overlapping.
  std::set<std::pair<uint32_t, uint32_t> > uniq_checker;
  std::vector<std::pair<uint32_t, uint8_t> > overlap_checker;
  for (unsigned i = 0; i < num_tables; ++i) {
    const uint32_t end_byte
        = subtable_headers[i].offset + subtable_headers[i].length;

    if (!uniq_checker.insert(std::make_pair(subtable_headers[i].offset,
                                            end_byte)).second) {
      // Sometimes Unicode table and MS table share exactly the same data.
      // We'll allow this.
      continue;
    }
    overlap_checker.push_back(
        std::make_pair(subtable_headers[i].offset, 1 /* start */));
    overlap_checker.push_back(
        std::make_pair(end_byte, 0 /* end */));
  }
  std::sort(overlap_checker.begin(), overlap_checker.end());
  int overlap_count = 0;
  for (unsigned i = 0; i < overlap_checker.size(); ++i) {
    overlap_count += (overlap_checker[i].second ? 1 : -1);
    if (overlap_count > 1) {
      return OTS_FAILURE();
    }
  }

  // we grab the number of glyphs in the file from the maxp table to make sure
  // that the character map isn't referencing anything beyound this range.
  if (!file->maxp) {
    return OTS_FAILURE();
  }
  const uint16_t num_glyphs = file->maxp->num_glyphs;

  // We only support a subset of the possible character map tables. Microsoft
  // 'strongly recommends' that everyone supports the Unicode BMP table with
  // the UCS-4 table for non-BMP glyphs. We'll pass the following subtables:
  //   Platform ID   Encoding ID  Format
  //   0             0            4       (Unicode Default)
  //   0             3            4       (Unicode BMP)
  //   0             3            12      (Unicode UCS-4)
  //   0             5            14      (Unicode Variation Sequences)
  //   1             0            0       (Mac Roman)
  //   3             0            4       (MS Symbol)
  //   3             1            4       (MS Unicode BMP)
  //   3             10           12      (MS Unicode UCS-4)
  //   3             10           13      (MS UCS-4 Fallback mapping)
  //
  // Note:
  //  * 0-0-4 table is (usually) written as a 3-1-4 table. If 3-1-4 table
  //    also exists, the 0-0-4 table is ignored.
  //  * 0-3-4 table is written as a 3-1-4 table. If 3-1-4 table also exists,
  //    the 0-3-4 table is ignored.
  //  * 0-3-12 table is written as a 3-10-12 table. If 3-10-12 table also
  //    exists, the 0-3-12 table is ignored.
  //

  for (unsigned i = 0; i < num_tables; ++i) {
    if (subtable_headers[i].platform == 0) {
      // Unicode platform

      if ((subtable_headers[i].encoding == 0) &&
          (subtable_headers[i].format == 4)) {
        // parse and output the 0-0-4 table as 3-1-4 table. Sometimes the 0-0-4
        // table actually points to MS symbol data and thus should be parsed as
        // 3-0-4 table (e.g., marqueem.ttf and quixotic.ttf). This error will be
        // recovered in ots_cmap_serialise().
        if (!Parse3x4(file, 1, data + subtable_headers[i].offset,
                      subtable_headers[i].length, num_glyphs)) {
          return OTS_FAILURE();
        }
      } else if ((subtable_headers[i].encoding == 3) &&
                 (subtable_headers[i].format == 4)) {
        // parse and output the 0-3-4 table as 3-1-4 table.
        if (!Parse3x4(file, 1, data + subtable_headers[i].offset,
                      subtable_headers[i].length, num_glyphs)) {
          return OTS_FAILURE();
        }
      } else if ((subtable_headers[i].encoding == 3) &&
                 (subtable_headers[i].format == 12)) {
        // parse and output the 0-3-12 table as 3-10-12 table.
        if (!Parse31012(file, data + subtable_headers[i].offset,
                        subtable_headers[i].length, num_glyphs)) {
          return OTS_FAILURE();
        }
      } else if ((subtable_headers[i].encoding == 5) &&
                 (subtable_headers[i].format == 14)) {
        if (!Parse0514(file, data + subtable_headers[i].offset,
                       subtable_headers[i].length, num_glyphs)) {
          return OTS_FAILURE();
        }
      }

    } else if (subtable_headers[i].platform == 1) {
      // Mac platform

      if ((subtable_headers[i].encoding == 0) &&
          (subtable_headers[i].format == 0)) {
        // parse and output the 1-0-0 table.
        if (!Parse100(file, data + subtable_headers[i].offset,
                      subtable_headers[i].length)) {
          return OTS_FAILURE();
        }
      }

    } else if (subtable_headers[i].platform == 3) {
      // MS platform

      switch (subtable_headers[i].encoding) {
        case 0:
        case 1:
          if (subtable_headers[i].format == 4) {
            // parse 3-0-4 or 3-1-4 table.
            if (!Parse3x4(file, subtable_headers[i].encoding,
                          data + subtable_headers[i].offset,
                          subtable_headers[i].length, num_glyphs)) {
              return OTS_FAILURE();
            }
          }
          break;
        case 10:
          if (subtable_headers[i].format == 12) {
            file->cmap->subtable_3_10_12.clear();
            if (!Parse31012(file, data + subtable_headers[i].offset,
                            subtable_headers[i].length, num_glyphs)) {
              return OTS_FAILURE();
            }
          } else if (subtable_headers[i].format == 13) {
            file->cmap->subtable_3_10_13.clear();
            if (!Parse31013(file, data + subtable_headers[i].offset,
                            subtable_headers[i].length, num_glyphs)) {
              return OTS_FAILURE();
            }
          }
          break;
      }
    }
  }

  return true;
}

bool ots_cmap_should_serialise(OpenTypeFile *file) {
  return file->cmap;
}

bool ots_cmap_serialise(OTSStream *out, OpenTypeFile *file) {
  const bool have_0514 = file->cmap->subtable_0_5_14_data;
  const bool have_100 = file->cmap->subtable_1_0_0.size();
  const bool have_304 = file->cmap->subtable_3_0_4_data;
  // MS Symbol and MS Unicode tables should not co-exist.
  // See the comment above in 0-0-4 parser.
  const bool have_314 = (!have_304) && file->cmap->subtable_3_1_4_data;
  const bool have_31012 = file->cmap->subtable_3_10_12.size();
  const bool have_31013 = file->cmap->subtable_3_10_13.size();
  const unsigned num_subtables = static_cast<unsigned>(have_0514) +
                                 static_cast<unsigned>(have_100) +
                                 static_cast<unsigned>(have_304) +
                                 static_cast<unsigned>(have_314) +
                                 static_cast<unsigned>(have_31012) +
                                 static_cast<unsigned>(have_31013);
  const off_t table_start = out->Tell();

  // Some fonts don't have 3-0-4 MS Symbol nor 3-1-4 Unicode BMP tables
  // (e.g., old fonts for Mac). We don't support them.
  if (!have_304 && !have_314) {
    return OTS_FAILURE();
  }

  if (!out->WriteU16(0) ||
      !out->WriteU16(num_subtables)) {
    return OTS_FAILURE();
  }

  const off_t record_offset = out->Tell();
  if (!out->Pad(num_subtables * 8)) {
    return OTS_FAILURE();
  }

  const off_t offset_100 = out->Tell();
  if (have_100) {
    if (!out->WriteU16(0) ||  // format
        !out->WriteU16(6 + kFormat0ArraySize) ||  // length
        !out->WriteU16(0)) {  // language
      return OTS_FAILURE();
    }
    if (!out->Write(&(file->cmap->subtable_1_0_0[0]), kFormat0ArraySize)) {
      return OTS_FAILURE();
    }
  }

  const off_t offset_304 = out->Tell();
  if (have_304) {
    if (!out->Write(file->cmap->subtable_3_0_4_data,
                    file->cmap->subtable_3_0_4_length)) {
      return OTS_FAILURE();
    }
  }

  const off_t offset_314 = out->Tell();
  if (have_314) {
    if (!out->Write(file->cmap->subtable_3_1_4_data,
                    file->cmap->subtable_3_1_4_length)) {
      return OTS_FAILURE();
    }
  }

  const off_t offset_31012 = out->Tell();
  if (have_31012) {
    std::vector<OpenTypeCMAPSubtableRange> &groups
        = file->cmap->subtable_3_10_12;
    const unsigned num_groups = groups.size();
    if (!out->WriteU16(12) ||
        !out->WriteU16(0) ||
        !out->WriteU32(num_groups * 12 + 16) ||
        !out->WriteU32(0) ||
        !out->WriteU32(num_groups)) {
      return OTS_FAILURE();
    }

    for (unsigned i = 0; i < num_groups; ++i) {
      if (!out->WriteU32(groups[i].start_range) ||
          !out->WriteU32(groups[i].end_range) ||
          !out->WriteU32(groups[i].start_glyph_id)) {
        return OTS_FAILURE();
      }
    }
  }

  const off_t offset_31013 = out->Tell();
  if (have_31013) {
    std::vector<OpenTypeCMAPSubtableRange> &groups
        = file->cmap->subtable_3_10_13;
    const unsigned num_groups = groups.size();
    if (!out->WriteU16(13) ||
        !out->WriteU16(0) ||
        !out->WriteU32(num_groups * 12 + 14) ||
        !out->WriteU32(0) ||
        !out->WriteU32(num_groups)) {
      return OTS_FAILURE();
    }

    for (unsigned i = 0; i < num_groups; ++i) {
      if (!out->WriteU32(groups[i].start_range) ||
          !out->WriteU32(groups[i].end_range) ||
          !out->WriteU32(groups[i].start_glyph_id)) {
        return OTS_FAILURE();
      }
    }
  }

  const off_t offset_0514 = out->Tell();
  if (have_0514) {
    if (!out->Write(file->cmap->subtable_0_5_14_data,
                    file->cmap->subtable_0_5_14_length)) {
      return OTS_FAILURE();
    }
  }

  const off_t table_end = out->Tell();
  // We might have hanging bytes from the above's checksum which the OTSStream
  // then merges into the table of offsets.
  OTSStream::ChecksumState saved_checksum = out->SaveChecksumState();
  out->ResetChecksum();

  // Now seek back and write the table of offsets
  if (!out->Seek(record_offset)) {
    return OTS_FAILURE();
  }

  if (have_0514) {
    if (!out->WriteU16(0) ||
        !out->WriteU16(5) ||
        !out->WriteU32(offset_0514 - table_start)) {
      return OTS_FAILURE();
    }
  }

  if (have_100) {
    if (!out->WriteU16(1) ||
        !out->WriteU16(0) ||
        !out->WriteU32(offset_100 - table_start)) {
      return OTS_FAILURE();
    }
  }

  if (have_304) {
    if (!out->WriteU16(3) ||
        !out->WriteU16(0) ||
        !out->WriteU32(offset_304 - table_start)) {
      return OTS_FAILURE();
    }
  }

  if (have_314) {
    if (!out->WriteU16(3) ||
        !out->WriteU16(1) ||
        !out->WriteU32(offset_314 - table_start)) {
      return OTS_FAILURE();
    }
  }

  if (have_31012) {
    if (!out->WriteU16(3) ||
        !out->WriteU16(10) ||
        !out->WriteU32(offset_31012 - table_start)) {
      return OTS_FAILURE();
    }
  }

  if (have_31013) {
    if (!out->WriteU16(3) ||
        !out->WriteU16(10) ||
        !out->WriteU32(offset_31013 - table_start)) {
      return OTS_FAILURE();
    }
  }

  if (!out->Seek(table_end)) {
    return OTS_FAILURE();
  }
  out->RestoreChecksum(saved_checksum);

  return true;
}

void ots_cmap_free(OpenTypeFile *file) {
  delete file->cmap;
}

}  // namespace ots
