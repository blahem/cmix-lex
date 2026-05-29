# cmix-lex Changes

This document describes the main algorithmic differences between cmix-lex and
fx2-cmix. The compressor remains a self-contained cmix/PAQ-family compressor
specialized for `enwik9`; no external data, network access, or hidden database
is needed at decompression time.

## High-Level Pipeline

The `-e` path follows the same broad shape as fx2-cmix:

1. Split `enwik9` into PHDA9-style intro/main/coda regions.
2. Reorder article bodies using the packaged article-order file.
3. Apply the PHDA9 Wikipedia preprocessing.
4. Apply the cmix dictionary word-replacement transform, abbreviated WRT.
5. Apply the cmix-lex `payload_lex` transform to a structured tail region.
6. Compress the resulting byte stream with cmix.
7. Package the compressed stream, dictionary, and decompressor into `archive9`.

On decompression, cmix first reconstructs the transformed byte stream. Then
cmix-lex removes and decodes the `payload_lex` side data, restores the original
tail order exactly, dictionary-decodes the stream, applies the PHDA9 inverse
transform, sorts restored page bodies by their embedded page IDs, and writes
`enwik9_uncompressed`.

The packaged compressor `cmix` contains the updated article-order file because
compression needs it to reorder the original article bodies. The final
self-extracting `archive9` does not need to contain that file: after PHDA9
restoration, the original page IDs are present in the reconstructed page bodies,
so decompression reverses the article-body reorder by sorting those bodies by
ID.

## Tail Structure

After PHDA9 and WRT, the ready stream used by the final run had this size:

```text
post-WRT stream before payload_lex   586,459,321 bytes
encoded tail start                   541,126,651
encoded tail length                   45,332,670
```

The last 45 MB is not ordinary article text. It is structured line-oriented
PHDA9 metadata. We describe it as three regimes because the line formats and
statistics change sharply inside the tail:

```text
regime 0: beginning of tail
regime 1: starts 13,599,801 bytes into the tail
regime 2: starts 30,372,888 bytes into the tail
```

The important part is regime 1. It contains 243,425 article-related metadata
blocks, matching the article count used by the Wikipedia preprocessing path.
These blocks contain repeated fields such as `D99` and `D86a` lines plus
payload-like text. In the original PHDA9 order, many similar payloads are close,
but not close enough for the context models to exploit the structure fully.

## Payload-Lex Reordering

The `payload_lex` transform only changes regime 1. Regime 0 and regime 2 are
left byte-for-byte in their original positions.

For regime 1, cmix-lex parses the metadata blocks and sorts the `D99` blocks by
their visible payload bytes after the `D99` and first `D86a` lines. This is a
lexical ordering of the PHDA9 tail metadata, not an article-body reorder. The
effect is that similar payload content becomes clustered inside the stream seen
by cmix.

This cannot be done for free. PHDA9 decompression needs the page-body metadata
to be restored in its exact original one-to-one order. Without that, the inverse
article-order step would associate headers/tail metadata with the wrong page
bodies. Therefore the transform stores side data at the end of the transformed
stream.

## EOF Side Data

The current side-data format is named `R1ORD3`.

An early version stored a cruder raw permutation. The current version uses the
visible `D86a` field as a predictor of the original block order and stores the
remaining correction as Lehmer ranks. The side blob is appended at EOF:

```text
transformed stream size              587,138,826 bytes
side blob size                           679,489 bytes
restored stream size                 586,459,321 bytes
```

Appending the side data at EOF is intentional. It lets cmix exploit the reordered
regime 1 for almost the whole tail, and only then pay the cost of compressing
the correction data. In the completed full run, the final EOF side-data region
started near 99.88% of the payload. From 99.88% to the final payload, cmix
wrote `346,948` bytes.

The transform is exactly reversible:

1. Read the transformed stream.
2. Extract the EOF `R1ORD3` side blob.
3. Reconstruct the original regime-1 block order from the D86a/Lehmer data.
4. Restore the original tail and remove the EOF side blob.
5. Continue with the normal dictionary/PHDA9 inverse pipeline and sort restored
   page bodies by their embedded IDs.

## Reversibility Verification

A post-cmix roundtrip check was run on the final source and packaged compressor.
It verifies the decompression-side preprocessing path after arithmetic decoding
has produced the transformed ready stream:

```text
roundtrip input ready stream: 587,138,826 bytes
extracted side: 679,489 bytes
restored ready stream: 586,459,321 bytes
dictionary-decoded stream: 934,220,701 bytes
reconstructed enwik9: 1,000,000,000 bytes
roundtrip byte-compare: OK
```

Both original `enwik9` and reconstructed `enwik9_roundtrip` had SHA256:

```text
159b85351e5f76e60cbe32e04c677847a9ecba3adc79addab6f4c6c7aa3744bc
```

The final `archive9` was also started through the real self-extracting
decompression path and matched the known transformed payload prefix
byte-for-byte.

## fxcm_v26 Integration

cmix-lex integrates Kaido Orav's standalone `fxcm_v26` text model into the
cmix predictor stack. The integrated source is in `src/models/fxcmv1.cpp` and
keeps Kaido's copyright notice.

The port adds many text-specific predictions compared with the old fx2-cmix
fxcm model, including:

* dictionary codeword-aware word comparisons,
* word contexts that use decoded words and dictionary codewords,
* pronoun word type support,
* sentence contexts with recent sentence memory,
* separate sentence-context groups for regular text, lists, tables, and
  wikilinks,
* partial sentence contexts,
* direct/indirect state-map style predictors,
* extra stationary and context-map predictors,
* context gating for low-value Wikipedia sections such as category links,
  "See also", "References", "Bibliography", and "External links".

These changes are inherited from the `fxcm_v26` family, but the integration,
build, and final combination are part of cmix-lex.

## Disk-Backed PPM Improvements

The large PPM model is backed by a file (`ppm.temp`) through `mmap`, with
`mmap_to_disk=true`. This is necessary because the logical PPM heap is larger
than the Hutter memory limit, while only a moving working set needs to be
resident.

The old fx2-cmix path periodically used `munmap()` followed by `mmap()` to drop
resident pages. That approach worked, but it destroyed and recreated a huge
mapping repeatedly and relied on the kernel returning the same virtual address,
even though the PPM allocator stores raw pointers into the mapped heap.

cmix-lex keeps the mapping stable and uses:

```text
madvise(MADV_DONTNEED)
```

every 5,000 processed bytes to drop resident file-backed PPM pages from VmRSS.
This preserves the virtual address range and does not change model bytes or any
probability calculation.

The mapping is also marked:

```text
madvise(MADV_RANDOM)
```

The PPM heap is accessed by pointer-chasing rather than sequential scanning, so
`MADV_RANDOM` avoids unnecessary readahead on the file-backed heap.

`MADV_RANDOM` suppresses unnecessary readahead on this non-sequential workload;
the final run shifted from read-amplified I/O toward mostly dirty-page
writeback from actual PPM updates.

Other small PPM I/O cleanups:

* `O_NOATIME` avoids access-time metadata writes on `ppm.temp` page faults.
* `ftruncate()` sizes the sparse backing file exactly instead of using
  `lseek()+write()`.

These PPM changes are intended to be byte-for-byte output-neutral. They do not
inspect `/proc`, call `mincore()`, or change any PPM probability/update logic.

## Resource Measurements

Completed May 26, 2026 compression run:

```text
compressed payload                 108,929,925 bytes
archive9                           109,190,109 bytes
packaged cmix                          459,938 bytes
max RSS                              9,696,008 KiB
CPU time                              43:33:23
wall time                             51:30:03
swap used                                    0
```

The corresponding full decompression verification completed on May 29, 2026
with exit status 0. It produced a 1,000,000,000 byte output file whose SHA256
matched the original `enwik9`.

Estimated peak run-directory disk use was about 20.8 GB decimal, or 19.39 GiB.
This includes the 14.68 GB `ppm.temp` backing file, the transformed ready-stream
temporary file, preprocessing scratch files, and run artifacts.
