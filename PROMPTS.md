# AI Prompts & Interaction Log

Chronological, unedited record of AI prompts used during project development.

---

### Prompt 1
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 14:24 IST
- **Prompt:**
```text
You are working on a 24-hour engineering hackathon project.

PROJECT:
Build a single-process embedded key-value database from scratch using C++17.

The database must store data in a single file and support:
- PUT
- GET
- DELETE
- BATCH
- OPEN
- CLOSE

Important requirements:
- Support at least 1 million keys.
- Support batches containing up to 10,000 operations.
- Recover in under 2 seconds.
- Support concurrent readers and writers.
- Guarantee atomic batches.
- Guarantee durability for every operation acknowledged as successful.
- Recover safely after unexpected process termination.
- Implement compaction.
- Support configurable maximum file size.
- Handle disk-full conditions safely.
- Provide a terminal-based CLI.
- Provide a torture-testing tool.

STRICT RESTRICTIONS:
- Do not use SQLite, RocksDB, LevelDB, Redis or any existing database engine.
- Do not use existing WAL, B-tree or LSM storage libraries.
- Implement the storage engine ourselves.
- Use C++17 and standard libraries wherever possible.

YOUR FIRST TASK IS REPOSITORY SETUP AND UNDERSTANDING.

Follow these steps:

1. Inspect the complete GitHub repository.
2. Read README.md and CONTRACT.md.
3. Understand the problem statement and all requirements.
4. Inspect the existing directory structure and available source files.
5. Identify the build system and check whether the project compiles.
6. Understand the proposed architecture and module boundaries.
7. Identify dependencies between your assigned module and other modules.
8. Identify any unclear or conflicting decisions in CONTRACT.md.

IMPORTANT:
- Do not start implementing the database yet.
- Do not modify another member's assigned module.
- Do not invent a different binary file format.
- Do not silently change shared interfaces.
- Do not overwrite existing files without understanding them.
- Do not introduce unnecessary dependencies.

FIRST RESPONSE:
Give me:
1. Your understanding of the project.
2. Your understanding of the current repository.
3. Your proposed implementation plan.
4. Files you expect to create or modify.
5. Interfaces you need from other modules.
6. Potential technical risks.
7. Questions that need team-level decisions.

Wait for the shared architecture and interfaces to be agreed upon before implementing. and i will also send the prompt of my role after this
```

---

### Prompt 2
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 14:35 IST
- **Prompt:**
```text
before y role we will do the strucure that my every team members will work on the same we are four in team of each of us have our own roles so we will do the structre first
```

---

### Prompt 3 (Storage Engine)
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 15:11 IST
- **Prompt:**
```text
You are responsible for the STORAGE ENGINE of our C++17 embedded key-value database.

YOUR RESPONSIBILITIES:

1. Design and implement the core storage engine.
2. Implement PUT, GET and DELETE operations.
3. Implement binary serialization and deserialization.
4. Maintain an in-memory index using suitable C++ data structures.
5. Support at least 1 million keys.
6. Handle large keys and values safely.
7. Implement efficient file read and write operations.
8. Provide clean interfaces for the durability and recovery modules.

BEFORE CODING:

- Read CONTRACT.md.
- Understand the agreed binary file format.
- Coordinate with Member 2 about transaction records and checksums.
- Coordinate with Member 3 about index synchronization.
- Do not independently change the file format.

EXPECTED OUTPUT:

- Storage engine header and implementation files.
- Data structures for indexing.
- Serialization/deserialization logic.
- Unit tests for PUT, GET and DELETE.
- Tests for large values and 1 million keys.
- Documentation explaining your implementation.

CODE QUALITY:

- Use clean C++17.
- Avoid unnecessary memory copies.
- Handle invalid input and file errors.
- Keep functions modular.
- Add meaningful comments around critical storage logic.

Work only on your assigned branch.

Do not claim an operation is complete until its required durability guarantees are implemented through the agreed interface.
```

---

### Prompt 4 (Team Lead & Integration)
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 15:06 IST
- **Prompt:**
```text
so my responsibility is develop only CLI and torture. You are the TEAM LEAD AND INTEGRATION OWNER of a C++17 embedded key-value database hackathon project.

YOUR RESPONSIBILITIES:

1. Maintain the overall project architecture.
2. Coordinate the three other developers.
3. Establish and maintain shared interfaces.
4. Review pull requests before merging.
5. Integrate all modules into the main branch.
6. Implement or coordinate the terminal CLI.
7. Implement or coordinate the independent torture-testing tool.
8. Maintain project documentation.
9. Run integration and regression tests.
10. Prepare the final demonstration.

CLI REQUIREMENTS:

Support commands similar to:

kvdb open <path> [--max-bytes N]
kvdb put <path> <key> <value>
kvdb get <path> <key>
kvdb del <path> <key>
kvdb batch <path> <file>
kvdb dump <path>
kvdb check <path>
kvdb serve <path>

TORTURE TESTING REQUIREMENTS:

Build a testing tool that:

- Generates random database operations.
- Runs concurrent writers.
- Independently records acknowledged operations.
- Randomly terminates the database process.
- Restarts the database.
- Checks recovered data against the independent ACK oracle.
- Detects missing acknowledged operations and corrupted data.
- Supports reproducible seeds.
- Runs at least 500 iterations by default.

GITHUB RULES:

- main is the integration branch.
- Each developer works on their assigned feature branch.
- Every change must come through a pull request.
- Review the implementation and tests before merging.
- Never merge code that breaks the build.
- Avoid merging conflicting changes to shared interfaces.

DOCUMENTATION:

Maintain:
- README.md
- CONTRACT.md
- ARCHITECTURE.md
- BREAKING.md
- PROMPTS.md

IMPORTANT:

Do not blindly trust AI-generated code.

Review:
- File operations.
- Error handling.
- Memory management.
- Synchronization.
- Crash recovery.
- Transaction atomicity.
- Compaction safety.

Your goal is not just to make the project compile. Your goal is to ensure the complete database satisfies the hackathon's correctness requirements.
```

---

### Prompt 4
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 16:46 IST
- **Prompt:**
```text
Continue
```

---

### Prompt 5
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 16:47 IST
- **Prompt:**
```text
tell me devolpment phases of CLI and TORTURE . i want to build only CLI and TORTURE.
```

---

### Prompt 6
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 17:02 IST
- **Prompt:**
```text
Continue
```

---

### Prompt 7
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 16:52 IST
- **Prompt:**
```text
now devlope only phase 1.  
```

---

### Prompt 8
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 17:13 IST
- **Prompt:**
```text
Continue
```

---

### Prompt 9
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 17:14 IST
- **Prompt:**
```text
develop phase 2 only
```

---

### Prompt 10
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 17:27 IST
- **Prompt:**
```text
Continue
```

---

### Prompt 11
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 17:28 IST
- **Prompt:**
```text
raise the PR
```

---

### Prompt 12
- **Tool:** Antigravity IDE (Gemini 3.8 Flash)
- **Approximate Time:** 2026-11-06 17:41 IST
- **Prompt:**
```text
now, develop phase 3
```


