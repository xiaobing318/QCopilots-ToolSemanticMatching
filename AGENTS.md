# QCopilots Tool Semantic Matching development

- Application code is C++20 and Qt Widgets. Owned target prefix is qctsm and namespace is qctsm. Keep runtime independent of QGIS and Python.
- The shared retrieval SDK depends only on Qt Core/Network and SQLite. Never link llama, GUI or GPU into the SDK consumed by QGIS.
- Agents edit only their assigned modules. Do not commit, push, switch branches or overwrite another agent's files. The coordinating agent handles Git and integration.
- All build, model, dependency, test and package outputs stay outside the source checkout. Tests use unique temporary profiles and close owned processes.
- Preserve upstream sources and third-party notices. Application license is MIT. Documentation is primarily Chinese with few semicolons.
- QGIS and the standalone program can open the same local SQLite index. Use WAL, a shared schema and cross-process single-writer coordination. Reads remain available while indexing. Never mix embeddings from distinct profiles.
- Real behavior only. No fake embeddings, false success, or skipped checks reported as passed. CI covers the same 11 variants as the reference QCopilots-Search.
- Never read AGENTS.md or .codex files inside QGIS or QGIS-Materials, per the user's instruction.
