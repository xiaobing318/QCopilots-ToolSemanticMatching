# Third-party notices

QCopilots Tool Semantic Matching is licensed under MIT. Build, deployment and runtime integration portions are adapted from QCopilots-Search, Copyright (c) 2026 xbyang, MIT.

- llama.cpp and ggml: MIT. The exact upstream commit and archive digest are in `dependencies.lock.json`. Upstream source is not patched.
- cpp-httplib: MIT, from the locked llama.cpp vendor directory.
- Text embedding golden vectors and prompt reference fixtures: DigUp, Copyright (c) 2026 Abdur Rahim, MIT. The complete notice is preserved in `tests/runtime/fixtures/LICENSE-DigUp` and installed with the product notices.
- SQLite: public domain. The locked amalgamation is built for the standalone product. QGIS uses its existing SQLite target.
- Qt 6: LGPL v3/GPL v3 or commercial terms as supplied by The Qt Company. Distributed Qt libraries remain dynamically linked. SDK packages and checksums are in `tools/qt-packages.lock.json`.
- EmbeddingGemma 2 weights: distributed separately under their upstream model terms. The application downloads only the checked model requested by the user.

Packaged upstream licenses are installed alongside the corresponding runtime. Model weights and GPU SDK source packages are not part of this repository.
