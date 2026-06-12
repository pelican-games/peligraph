# Pelican2

game engine

## Requisites

- Vulkan SDK 1.4.x (verified with 1.4.350; 1.3.x is no longer supported since vma-hpp v3.3.0)
- Qt6

## How to build

```sh
cmake . -B build -DCMAKE_PREFIX_PATH=(Qt install path)   # ex. CMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64
cmake --build ./build
```

only player:

```sh
cmake . -B build -DSKIP_DEVSTUDIO
cmake --build ./build
```

skip test:

```sh
cmake . -B build -DSKIP_TEST
cmake --build ./build
```

## Debug

run Pelican Studio:
```sh
cmake --build ./build --target run_studio
```

## Test

```sh
ctest --test-dir ./build
```
