# MOVEscape

movescape is Aptos Move decompiler written in C++

The source at the end is only half of the project, the other half is making the recovery inspectable.

This is not an attempt to reproduce the original file character for character, as compilation loses too much information.
The goal is to recover the program represented by the bytecode without quietly changing control flow.

## A small example
The checked example package contains this function:
```move
public fun measure(value: u8, owner: address): u8 {
    if (owner == @0x42 && value < 255) {
        value + 1
    } else {
        value
    }
}
```

After compiling and decompiling `NumberDemo.mv` with its source map, movescape emits:
```move
#[persistent]
public fun measure(value: u8, owner: address): u8 {
    let _t4: bool;

    _t4 = owner == @0x42 && value < 255u8;
    return if (_t4) { value + 1u8 } else { value }
}
```

While not textually identical, the bytecode compiles the same.

## Build
Build movescape with CMake and a C++20 compiler:
```bash
cmake -S . -B build
cmake --build build -j
./build/movescape_tests
```

Compile the included Move package:
```bash
aptos move compile --package-dir examples/round_trip_demo/original
```

Inspect the same module at each stage:
```bash
MODULE=examples/round_trip_demo/original/build/movescapeRoundTripDemo/bytecode_modules/NumberDemo.mv

./build/movescape module "$MODULE"
./build/movescape symbols "$MODULE"
./build/movescape disassemble "$MODULE"
./build/movescape cfg "$MODULE" 1
./build/movescape analyze "$MODULE" 1
./build/movescape lift "$MODULE" 1
./build/movescape dataflow "$MODULE" 1
./build/movescape expressions "$MODULE" 1
./build/movescape structure "$MODULE" 1
./build/movescape decompile "$MODULE"
```

To recover available local and type-parameter names:
```bash
MAP=examples/round_trip_demo/original/build/movescapeRoundTripDemo/source_maps/NumberDemo.mvsm
./build/movescape decompile "$MODULE" --source-map "$MAP"
```

## Repository layout

```text
include/movescape/   library interfaces and intermediate representations
src/                 implementation
tools/               command line UI
tests/               AI generated unit tests
examples/            Move packages used for checks
```

## License

Apache-2.0. See [LICENSE](LICENSE).
