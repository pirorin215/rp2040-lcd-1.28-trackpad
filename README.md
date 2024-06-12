# rp2040-lcd-1.28-trackpad
### rp2040-lcd-1.28-trackpad for trackpad

## How to build

### Windows WLS2

1. Download program source

```
git clone https://github.com/pirorin215/rp2040-lcd-1.28-trackpad
cd rp2040-lcd-1.28-trackpad
```

2. pico-sdk PATH
```
export PICO_SDK_PATH=/mnt/c/pico-sdk
```

3. cmake
```
cmake CMakeLists.txt 
```

4. make
```
make
```

5. Write uf2 to raspberry pi pico

### Mac

1. Download program source
```
git clone https://github.com/pirorin215/rp2040-lcd-1.28-trackpad
```

2. pico-sdk PATH
```
export PICO_SDK_PATH=/mnt/c/pico-sdk
export PICO_TOOLCHAIN_PATH=/Users/[user]/Library/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/13.2.1-1.1.1/.content/bin/
```

3. cmake
```
mkdir -p build
cd build
cmake ../CMakeLists.txt 
```

4. make
```
make
```

5. Write uf2 to raspberry pi pico
