# OCaml Project

## Build with Optimizations

This project is configured for high optimization using Dune and Flambda2 settings.

### Prerequisites
Ensure you are using an OCaml compiler switch with Flambda 2 enabled. You can check your version with:
```bash
ocamlopt -version
```

### Building
To build the project with full optimizations (O3, unboxing):

```bash
dune build --profile release
```

The executable will be located at `_build/default/bin/main.exe`.
