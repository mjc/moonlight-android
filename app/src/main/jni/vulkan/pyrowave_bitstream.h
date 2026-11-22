#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Finds the parts of a PyroWave frame that arrived whole when some of it was lost. No Android
// or Vulkan dependencies, so it can be tested on its own (tests/pyrowave_partial_test.cpp).

namespace vkr {

// A stretch of a frame that didn't arrive
struct PyrowaveGap {
    size_t offset;
    size_t length;
};

// Missing ranges and decoder probes are both ordered by frame offset. Advance once through the
// ranges instead of rescanning every gap for each block or record in a partial frame.
struct PyrowaveGapScanner {
    const PyrowaveGap* gaps;
    size_t count;
    size_t next = 0;

    bool overlaps(size_t begin, size_t end) {
        while (next < count) {
            const PyrowaveGap& gap = gaps[next];
            if (gap.offset >= begin || gap.length > begin - gap.offset) {
                break;
            }
            next++;
        }
        return next < count && gaps[next].offset < end;
    }
};

// A stretch that arrived, in order, without the gaps. Pointers stay valid for the decode that
// walks them and no longer.
struct PyrowaveSpan {
    size_t offset;
    const uint8_t* data;
    size_t length;
};

constexpr size_t kMaxPyrowaveFrameBytes = 32u * 1024u * 1024u;

// A frame as one buffer, or as spans. Spans are in order and do not overlap.
struct PyrowaveBytes {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const PyrowaveSpan* spans = nullptr;
    size_t spanCount = 0;

    // Spans are ordered and non-overlapping. Start at the last span whose offset is at or
    // before the requested byte, rather than rescanning packet spans from the beginning for
    // every record in a frame.
    size_t firstSpanCandidate(size_t offset) const {
        size_t low = 0;
        size_t high = spanCount;
        while (low < high) {
            const size_t mid = low + (high - low) / 2;
            if (spans[mid].offset <= offset) {
                low = mid + 1;
            }
            else {
                high = mid;
            }
        }
        return low == 0 ? 0 : low - 1;
    }

    // True when [offset, length) sits in the contiguous buffer or in one span, so a push can
    // hand that pointer over. A contiguous frame is one span: its runs stay coalesced.
    bool oneSpan(size_t offset, size_t length) const {
        if (length == 0) {
            return true;
        }
        if (spanCount == 0) {
            return data != nullptr;
        }
        const size_t end = offset + length;
        if (end < offset) {
            return false;
        }
        for (size_t i = firstSpanCandidate(offset); i < spanCount; i++) {
            const PyrowaveSpan& span = spans[i];
            const size_t spanEnd = span.offset + span.length;
            if (span.length == 0 || span.data == nullptr || spanEnd < span.offset) {
                return false;
            }
            if (span.offset <= offset && end <= spanEnd) {
                return true;
            }
            if (span.offset > offset) {
                return false;
            }
        }
        return false;
    }

    // Like oneSpan(), for callers that visit packet runs in increasing offset order. The cursor
    // avoids searching the span list again for each run.
    bool oneSpan(size_t offset, size_t length, size_t& spanIndex) const {
        if (length == 0 || spanCount == 0) {
            return length == 0 || data != nullptr;
        }
        const size_t end = offset + length;
        if (end < offset) {
            return false;
        }
        while (spanIndex < spanCount) {
            const PyrowaveSpan& span = spans[spanIndex];
            const size_t spanEnd = span.offset + span.length;
            if (span.length == 0 || span.data == nullptr || spanEnd < span.offset) {
                return false;
            }
            if (spanEnd > offset) {
                return span.offset <= offset && end <= spanEnd;
            }
            spanIndex++;
        }
        return false;
    }

    // Pointer to [offset, length). Into the contiguous buffer or the one span that holds it.
    // Across spans, copied into scratch, which must hold `length` bytes. Null if a byte is absent.
    const uint8_t* at(size_t offset, size_t length, uint8_t* scratch) const {
        if (length == 0) {
            return scratch;
        }
        if (spanCount == 0) {
            if (data == nullptr || offset > size || length > size - offset) {
                return nullptr;
            }
            return data + offset;
        }
        const size_t end = offset + length;
        if (end < offset) {
            return nullptr;
        }
        size_t filled = 0;
        for (size_t i = firstSpanCandidate(offset); i < spanCount; i++) {
            const PyrowaveSpan& span = spans[i];
            if (span.length == 0 || span.data == nullptr) {
                return nullptr;
            }
            const size_t spanEnd = span.offset + span.length;
            if (spanEnd < span.offset) {
                return nullptr;
            }
            if (spanEnd <= offset) {
                continue;
            }
            if (span.offset >= end) {
                break;
            }
            const size_t from = offset > span.offset ? offset : span.offset;
            const size_t to = end < spanEnd ? end : spanEnd;
            if (from != offset + filled) {
                return nullptr;
            }
            const size_t n = to - from;
            if (filled == 0 && to == end) {
                return span.data + (from - span.offset);
            }
            if (scratch == nullptr) {
                return nullptr;
            }
            memcpy(scratch + filled, span.data + (from - span.offset), n);
            filled += n;
        }
        return filled == length ? scratch : nullptr;
    }

    // Read records in ascending offset order without binary-searching the spans for each one.
    const uint8_t* at(size_t offset, size_t length, uint8_t* scratch, size_t& spanIndex) const {
        if (length == 0) {
            return scratch;
        }
        if (spanCount == 0) {
            if (data == nullptr || offset > size || length > size - offset) {
                return nullptr;
            }
            return data + offset;
        }
        const size_t end = offset + length;
        if (end < offset) {
            return nullptr;
        }
        if (spanIndex < spanCount) {
            const PyrowaveSpan& span = spans[spanIndex];
            if (span.length != 0 && span.data != nullptr && offset >= span.offset &&
                    offset - span.offset <= span.length && length <= span.length - (offset - span.offset)) {
                return span.data + (offset - span.offset);
            }
        }
        while (spanIndex < spanCount) {
            const PyrowaveSpan& span = spans[spanIndex];
            const size_t spanEnd = span.offset + span.length;
            if (span.length == 0 || span.data == nullptr || spanEnd < span.offset) {
                return nullptr;
            }
            if (spanEnd > offset) {
                break;
            }
            spanIndex++;
        }

        size_t filled = 0;
        for (size_t i = spanIndex; i < spanCount; i++) {
            const PyrowaveSpan& span = spans[i];
            if (span.length == 0 || span.data == nullptr) {
                return nullptr;
            }
            const size_t spanEnd = span.offset + span.length;
            if (spanEnd < span.offset) {
                return nullptr;
            }
            if (spanEnd <= offset) {
                continue;
            }
            if (span.offset >= end) {
                break;
            }
            const size_t from = offset > span.offset ? offset : span.offset;
            const size_t to = end < spanEnd ? end : spanEnd;
            if (from != offset + filled) {
                return nullptr;
            }
            const size_t n = to - from;
            if (filled == 0 && to == end) {
                spanIndex = i;
                return span.data + (from - span.offset);
            }
            if (scratch == nullptr) {
                return nullptr;
            }
            memcpy(scratch + filled, span.data + (from - span.offset), n);
            filled += n;
        }
        return filled == length ? scratch : nullptr;
    }
};

namespace pyrowave_bitstream {
    // PyroWave's bitstream (docs/bitstream.md in PyroWave): an 8 byte header starts the frame,
    // then each 32x32 block of coefficients is a whole number of 32-bit words with an 8 byte
    // header of its own. Both headers share the first word's top half: payload words (12 bits,
    // blocks only), a 3 bit frame sequence number, and whether it's the frame header.
    struct BlockHeader {
        uint32_t payloadWords;
        uint32_t sequence;
        bool frameHeader;
        uint32_t blockIndex;
    };

    inline BlockHeader readHeader(const uint8_t* p) {
        uint16_t word;
        uint32_t second;
        memcpy(&word, p + 2, sizeof(word));
        memcpy(&second, p + 4, sizeof(second));
        return {word & 0xFFFu, (word >> 12) & 0x7u, (word >> 15) != 0, second >> 8};
    }

    constexpr size_t kHeaderSize = 8;
}

// Calls push(offset, length) for each run of whole blocks in a frame with gaps (in order), for
// the decoder to take. A block that lost any of its data is left out. Its header gives its
// length, so the next block is still found; if the header itself was lost, the next block is
// found by trying each following word as a header, which counts once it's followed by another
// that looks right (or by a gap, or the end). width and height are the frame's. Returns whether
// anything was pushed.
//
// lostBlocks gets a bit set for each block that may have been lost, by block index (bit i of
// word i / 32), including every block after the last one found. The encoder leaves out blocks
// that are all zero, and blocks come in increasing order, so a block missing between two that
// arrived next to each other was never sent. PyroWave needs that to tell when a partial frame
// still has all of its coarsest blocks.
template <typename Push>
bool pushArrivedBlocks(const PyrowaveBytes& frame, const PyrowaveGap* gaps, size_t gapCount, uint32_t width,
                       uint32_t height, std::vector<uint32_t>& lostBlocks, Push push) {
    using namespace pyrowave_bitstream;
    const size_t size = frame.size;
    uint8_t headerScratch[kHeaderSize];
    size_t headerSpanIndex = 0;
    size_t runSpanIndex = 0;
    // Stack scratch only when an 8-byte header crosses a span. The caller uses it before the next read.
    const auto headerAt = [&](size_t offset) -> const uint8_t* {
        return frame.at(offset, kHeaderSize, headerScratch, headerSpanIndex);
    };
    const auto probeHeaderAt = [&](size_t offset) -> const uint8_t* {
        return frame.at(offset, kHeaderSize, headerScratch);
    };

    PyrowaveGapScanner gapScanner {gaps, gapCount};
    const auto missing = [&](size_t begin, size_t end) { return gapScanner.overlaps(begin, end); };

    // More blocks than a frame of this size can have, to reject garbage
    const uint32_t maxBlocks = (width / 32 + 2) * (height / 32 + 2) * 2;

    lostBlocks.assign((maxBlocks + 31) / 32, 0);
    const auto markLost = [&](int64_t first, int64_t last) {
        for (int64_t i = first < 0 ? 0 : first; i <= last && i < static_cast<int64_t>(maxBlocks); i++) {
            lostBlocks[static_cast<size_t>(i) / 32] |= 1u << (i % 32);
        }
    };
    // Set once track of the blocks was lost, until the next block is found
    bool lostTrack = false;

    int sequence = -1;           // The frame's, from its header or first block
    int64_t lastBlock = -1;      // Blocks come in increasing order
    const auto plausible = [&](const BlockHeader& h, size_t offset) {
        return !h.frameHeader && h.payloadWords * 4 >= kHeaderSize && offset + h.payloadWords * 4 <= size &&
               (sequence < 0 || h.sequence == static_cast<uint32_t>(sequence)) &&
               static_cast<int64_t>(h.blockIndex) > lastBlock && h.blockIndex < maxBlocks;
    };
    const auto headerArrived = [&](size_t offset) {
        return offset + kHeaderSize <= size && !missing(offset, offset + kHeaderSize);
    };

    const auto resync = [&](size_t from) -> size_t {
        for (size_t offset = (from + 3) & ~size_t(3); offset + kHeaderSize <= size; offset += 4) {
            if (!headerArrived(offset)) {
                continue;
            }
            const uint8_t* hdr = probeHeaderAt(offset);
            if (!hdr) {
                continue;
            }
            const BlockHeader h = readHeader(hdr);
            if (!plausible(h, offset)) {
                continue;
            }
            const size_t next = offset + h.payloadWords * 4;
            if (next + kHeaderSize > size || !headerArrived(next)) {
                return offset;
            }
            const uint8_t* nextHdr = probeHeaderAt(next);
            if (!nextHdr) {
                return offset;
            }
            const BlockHeader after = readHeader(nextHdr);
            const bool padding = after.payloadWords == 0 && after.blockIndex == 0;
            if (padding || (!after.frameHeader && after.payloadWords * 4 >= kHeaderSize &&
                            after.sequence == h.sequence && after.blockIndex > h.blockIndex &&
                            after.blockIndex < maxBlocks)) {
                return offset;
            }
        }
        return size;
    };

    // Runs of whole blocks, pushed as they end
    size_t runStart = SIZE_MAX;
    size_t runEnd = 0;
    bool pushed = false;
    const auto flush = [&]() {
        if (runStart != SIZE_MAX) {
            if (push(runStart, runEnd - runStart)) {
                pushed = true;
            }
            runStart = SIZE_MAX;
        }
    };
    const auto extend = [&](size_t begin, size_t end) {
        // A run stays inside one span. A block that itself crosses is pushed on its own; the
        // caller copies that block. With no spans, oneSpan is true and runs stay coalesced.
        if (runStart != SIZE_MAX && !frame.oneSpan(runStart, end - runStart, runSpanIndex)) {
            flush();
        }
        if (runStart == SIZE_MAX) {
            runStart = begin;
        }
        runEnd = end;
    };

    size_t offset = 0;
    while (offset + kHeaderSize <= size) {
        if (!headerArrived(offset)) {
            flush();
            lostTrack = true;
            offset = resync(offset + 4);
            continue;
        }

        const uint8_t* hdr = headerAt(offset);
        if (!hdr) {
            flush();
            lostTrack = true;
            offset = resync(offset + 4);
            continue;
        }
        const BlockHeader h = readHeader(hdr);
        if (h.frameHeader) {
            // Only the frame starts with one
            if (offset == 0) {
                sequence = static_cast<int>(h.sequence);
                extend(0, kHeaderSize);
                offset = kHeaderSize;
            }
            else {
                flush();
                lostTrack = true;
                offset = resync(offset + 4);
            }
            continue;
        }

        if (!plausible(h, offset)) {
            // Padding after the last block, or data that isn't what it should be
            flush();
            lostTrack = true;
            offset = resync(offset + 4);
            continue;
        }

        const size_t end = offset + h.payloadWords * 4;
        if (sequence < 0) {
            sequence = static_cast<int>(h.sequence);
        }
        if (lostTrack) {
            // Any block since the last one found may have been in what was lost
            markLost(lastBlock + 1, static_cast<int64_t>(h.blockIndex) - 1);
            lostTrack = false;
        }
        lastBlock = h.blockIndex;
        if (missing(offset, end)) {
            // Its header arrived, so the next block's place is still known
            markLost(h.blockIndex, h.blockIndex);
            flush();
        }
        else {
            extend(offset, end);
        }
        offset = end;
    }
    flush();
    // Anything after the last block found may have been lost too: the frame's end may not have
    // arrived, or it was cut short at its deadline, even where the data ends with a whole block
    markLost(lastBlock + 1, static_cast<int64_t>(maxBlocks) - 1);
    return pushed;
}

// Calls push(offset, length) for each run of records in a whole frame in record framing (as the
// nonary host sends it), leaving out its padding records: 0xFFFFFFFF, a word count N, then N
// zero words. PyroWave would take one for a broken frame header. Returns false if the records
// don't add up to the frame, or push does.
template <typename Push>
bool pushRecords(const PyrowaveBytes& frame, Push push) {
    using namespace pyrowave_bitstream;
    const size_t size = frame.size;
    if (frame.spanCount == 0) {
        const uint8_t* data = frame.data;
        if (data == nullptr) {
            return size == 0;
        }
        size_t runStart = 0;
        size_t pos = 0;
        while (pos < size) {
            if (size - pos < kHeaderSize) {
                return false;
            }
            uint32_t first;
            memcpy(&first, data + pos, sizeof(first));
            if (first == 0xFFFFFFFFu) {
                uint32_t words;
                memcpy(&words, data + pos + 4, sizeof(words));
                if (pos > runStart && !push(runStart, pos - runStart)) {
                    return false;
                }
                if (words > (size - pos - kHeaderSize) / 4) {
                    return false;
                }
                pos += kHeaderSize + 4 * static_cast<size_t>(words);
                runStart = pos;
                continue;
            }

            const bool frameHeader = (first >> 31) != 0;
            const size_t length = frameHeader ? kHeaderSize : 4 * static_cast<size_t>((first >> 16) & 0xFFFu);
            if (length < kHeaderSize || length > size - pos) {
                return false;
            }
            pos += length;
        }
        return pos == runStart || push(runStart, pos - runStart);
    }

    uint8_t headerScratch[kHeaderSize];
    size_t headerSpanIndex = 0;

    size_t runStart = 0;
    size_t runSpanIndex = 0;
    size_t pos = 0;
    while (pos < size) {
        if (size - pos < kHeaderSize) {
            return false;
        }
        const uint8_t* hdr = frame.at(pos, kHeaderSize, headerScratch, headerSpanIndex);
        if (!hdr) {
            return false;
        }
        uint32_t first;
        memcpy(&first, hdr, sizeof(first));
        if (first == 0xFFFFFFFFu) {
            uint32_t words;
            memcpy(&words, hdr + 4, sizeof(words));
            if (pos > runStart && !push(runStart, pos - runStart)) {
                return false;
            }
            if (words > (size - pos - kHeaderSize) / 4) {
                return false;
            }
            pos += kHeaderSize + 4 * static_cast<size_t>(words);
            runStart = pos;
            continue;
        }

        const bool frameHeader = (first >> 31) != 0;
        const size_t length = frameHeader ? kHeaderSize : 4 * static_cast<size_t>((first >> 16) & 0xFFFu);
        if (length < kHeaderSize || length > size - pos) {
            return false;
        }
        const size_t end = pos + length;
        // Same split as pushArrivedBlocks: complete records in one span stay one push.
        if (runStart == pos) {
            runSpanIndex = headerSpanIndex;
        }
        else {
            const PyrowaveSpan& runSpan = frame.spans[runSpanIndex];
            const size_t runSpanEnd = runSpan.offset + runSpan.length;
            if (runSpanEnd < runSpan.offset || runStart < runSpan.offset || end > runSpanEnd) {
                if (!push(runStart, pos - runStart)) {
                    return false;
                }
                runStart = pos;
                runSpanIndex = headerSpanIndex;
            }
        }
        pos = end;
    }
    return pos == runStart || push(runStart, pos - runStart);
}

// Calls push(offset, length) for each run of records that arrived whole in a partial frame in
// record framing, leaving out padding records, as pushRecords() does for a whole frame. A record
// that lost any of its data is left out. Its header gives its length, so the next record is still
// found; if the header itself was lost, parsing resumes at the next of recordStarts (in
// increasing order), where the host flagged a packet as starting with a record. Records aren't in
// block order, so there's no telling where one starts otherwise. Returns whether anything was
// pushed.
template <typename Push>
bool pushArrivedRecords(const PyrowaveBytes& frame, const PyrowaveGap* gaps, size_t gapCount,
                        const size_t* recordStarts, size_t recordStartCount, Push push) {
    using namespace pyrowave_bitstream;
    const size_t size = frame.size;
    uint8_t headerScratch[kHeaderSize];
    size_t headerSpanIndex = 0;

    PyrowaveGapScanner gapScanner {gaps, gapCount};
    const auto missing = [&](size_t begin, size_t end) { return gapScanner.overlaps(begin, end); };

    // The first record start after from, or the end if there's none
    size_t nextStart = 0;
    const auto resync = [&](size_t from) -> size_t {
        while (nextStart < recordStartCount && recordStarts[nextStart] <= from) {
            nextStart++;
        }
        return nextStart < recordStartCount ? recordStarts[nextStart] : size;
    };

    // Runs of whole records, pushed as they end
    size_t runStart = SIZE_MAX;
    size_t runEnd = 0;
    size_t runSpanIndex = 0;
    bool pushed = false;
    const auto flush = [&]() {
        if (runStart != SIZE_MAX) {
            if (push(runStart, runEnd - runStart)) {
                pushed = true;
            }
            runStart = SIZE_MAX;
        }
    };

    size_t pos = 0;
    while (pos < size) {
        if (size - pos < kHeaderSize || missing(pos, pos + kHeaderSize)) {
            flush();
            pos = resync(pos);
            continue;
        }

        const uint8_t* hdr = frame.at(pos, kHeaderSize, headerScratch, headerSpanIndex);
        if (!hdr) {
            flush();
            pos = resync(pos);
            continue;
        }
        uint32_t first;
        memcpy(&first, hdr, sizeof(first));
        const bool padding = first == 0xFFFFFFFFu;
        size_t length;
        if (padding) {
            uint32_t words;
            memcpy(&words, hdr + 4, sizeof(words));
            length = words <= (size - pos - kHeaderSize) / 4 ? kHeaderSize + 4 * static_cast<size_t>(words) : SIZE_MAX;
        }
        else {
            const bool frameHeader = (first >> 31) != 0;
            length = frameHeader ? kHeaderSize : 4 * static_cast<size_t>((first >> 16) & 0xFFFu);
        }
        if (length < kHeaderSize || length > size - pos) {
            // Not a record, or it runs past what arrived of the frame
            flush();
            pos = resync(pos);
            continue;
        }

        const size_t end = pos + length;
        if (padding || missing(pos, end)) {
            flush();
        }
        else {
            if (runStart != SIZE_MAX && !frame.oneSpan(runStart, end - runStart, runSpanIndex)) {
                flush();
            }
            if (runStart == SIZE_MAX) {
                runStart = pos;
            }
            runEnd = end;
        }
        pos = end;
    }
    flush();
    return pushed;
}

}  // namespace vkr
