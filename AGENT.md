# Build

This repository builds with CMake.

## Configure

For a backend-only compile, use:

```sh
cmake -S . -B build -DBUILD_FRONTEND=OFF -DVCPKG_APPLOCAL_DEPS=OFF
```

## Build

If you are not already in a Visual Studio Developer Shell, make sure the MSVC and Windows SDK `INCLUDE` and `LIB` paths are available first, then run:

```sh
cmake --build build --config Release
```

## Notes

- Frontend build is optional and can stay off for backend-only compilation.
- The verified build produced both `iot-manager` and `iot-edgenode` successfully.
