# handlords

A retro game of Rock-Paper-Scissors

For now, this contains the imgui development version for modern computers.


## How to build

### Debug Build
```bash
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

### Release Build
```bash
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

### With Address Sanitizer (Debug)
```bash
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DHANDLORDS_SANITIZE=ON ..
cmake --build .
```
