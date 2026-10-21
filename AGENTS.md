# Project Rules

## Testing
- Always write tests for all possible (especially updated) functionality.
- Run tests before declaring code works or not.

## Cross-platform
- Avoid platform-dependent features as the project evolves; keep future cross-platform porting possible.
- Prefer SDL2/OpenAL/vcpkg abstractions over Win32/UWP-specific APIs.
