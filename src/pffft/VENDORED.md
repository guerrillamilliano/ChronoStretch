# PFFFT (vendored)

- Upstream: https://bitbucket.org/jpommier/pffft, branch master, commit `67a4a6a113179ab4ac398298f46fce8dd60498dc`
  (2026-10-16), downloaded 2026-10-25.
- Files: `pffft.c` (sha256 `052633eaec428327b4e2991fd1c0a8967740df8d07566a34b9c234d9119620d8`), `pffft.h`
  (sha256 `d6ac7f26f7c3f87ed2ad7f0264c09d72285526d937b4dccc5fc1c97645a0d55d`).
- Licence: the FFTPACKv5 licence (BSD-style), in the header of `pffft.c`; reproduced in `../../THIRD-PARTY-NOTICES.md`.
- Local changes: none. ChronoStretch uses the ordered real transforms (`pffft_transform_ordered`) for its phase vocoder;
  NEON is used on the device build (`-mfpu=neon-vfpv4`), SSE on the x86 host test.
