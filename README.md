# Pelican2

game engine

## How to build

```sh
cmake . -B build
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

vulkan SDK 1.4.321以下

## Test

```sh
ctest --test-dir ./build
```
