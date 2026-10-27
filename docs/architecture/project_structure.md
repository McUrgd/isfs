# Project Structure

This document describes the organization of the ISFS codebase and the purpose of its directories and key files.

## Source Code (`isfs/`)
The core logic of the file system is implemented in the `isfs/` directory:

- **`isfs.c`**: The main entry point and the WinFsp file system implementation. It handles file system requests (Open, Read, Write, Rename, Delete) and manages the internal state of open handles and directory caches.
- **`atelier.c`**: The API client layer. It implements the communication with the Atelier v1 API via HTTP, including request formation, authentication, and response parsing.
- **`helper.c`**: General-purpose utility functions, such as string manipulations, path conversions, and logging helpers.

## External Libraries
- **`cjson/`**: Contains the `cJSON` library, used to parse JSON responses received from the Atelier API.

## Integration and Dependencies (`passthrough/`)
- **`passthrough/`**: Contains the necessary headers and static libraries (`winfsp-x64.lib`) required to link the project with the WinFsp driver. It acts as the integration layer between the ISFS logic and the Windows File System Proxy.

## Testing and Automation (`scripts/`)
The `scripts/` folder contains tools for development and manual verification:

- **`mock-atelier.ps1`**: A mock server that simulates the Atelier API endpoints. It allows testing the file system without a live backend and logs all incoming requests for verification.
- **`rename-test.ps1`**: A comprehensive manual test suite for document renaming (REQ-MAP-016, REQ-FS-032..036).
- **`unmount-test.ps1`**: A manual check for the unmount behavior and data discarding (REQ-MOUNT-023).

## Documentation (`docs/`)
- **`requirements/`**: Contains functional and mapping requirements (e.g., `functional/filesystem-operations.md`, `functional/atelier-mapping.md`).
- **`architecture/`**: High-level design documents, including the API description (`api.md`) and general overview (`overview.md`).

## Build Configuration
The project is built using the Microsoft Visual C++ (MSVC) compiler. Build commands are typically defined in `.vscode/tasks.json` for IDE integration.
